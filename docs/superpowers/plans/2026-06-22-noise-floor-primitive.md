# Noise-Floor Detection Primitive — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure a per-channel ambient/amp-hiss noise floor from the live capture and expose it (per-channel + a single averaged user-facing value), replacing the fixed `armFloor` SNR denominator with the measured floor once trusted.

**Architecture:** A pure `NoiseFloorMath` (header) holds the testable decisions; a header-only RT-safe `NoiseFloorTracker` accumulates quiet windows (pre-sweep silence + inter-sweep gaps) and publishes the floor via atomics; `HealthMonitor` owns one tracker, is fed the per-ear block peak it already computes, and exposes the floor; the engine feeds the tracker each block and uses the measured floor in `evaluateSnr` when valid.

**Tech Stack:** C++20, JUCE 8, Catch2 v3. Build/test: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build`. New test files register in `tests/CMakeLists.txt`.

## Global Constraints
- `observeBlock` runs on the audio callback: **allocation/lock-free**, plain loops + atomics (an infrequent bounded `std::sort` over a fixed ≤256-element window at fold time is acceptable — no alloc).
- Per-channel (L=0, R=1) independent. Floor in **dBFS / linear** (SPL conversion is a later feature).
- Floor fed the per-ear **block PEAK** (reuse `HealthMonitor::blockPeakPerEar` output `spkL/spkR`), not RMS — keeps the floor the same measure as the peak-based SNR numerator `maxSweepPeak`. *(This refines the spec's "RMS" wording.)*
- Quiet-window detection = level below a **loose absolute ceiling** (`kQuietCeilingLin` = `kGoodLevelLinear` ≈ −24 dBFS) AND a **sustained** run (≥500 ms), NOT a "recent-peak margin" — the pre-sweep baseline has no prior loud peak. *(Refines the spec's `isQuietBlock(level, recentPeak, margin)`.)*
- Robust-low estimate = **median** of the quiet window (Prawda et al. precedent); slow `blendFloor` update.
- Commit attribution: `Elevatormusic` / `22101396+Elevatormusic@users.noreply.github.com`. **No Claude co-author trailer.** Commit each task with `git -c user.name=... -c user.email=... commit`.

---

### Task 1: `NoiseFloorMath` pure helpers

**Files:**
- Create: `src/audio/NoiseFloorMath.h`
- Test: `tests/test_noisefloor.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_noisefloor.cpp` after `test_freezegate.cpp`)

**Interfaces — Produces:**
- `bool eb::isQuietBlock(float level, float ceiling)`
- `float eb::robustLowFloor(float* vals, int n)` — median; sorts `vals` in place; returns 0 if `n<=0`
- `float eb::blendFloor(float current, float candidate, float alpha)`
- `float eb::averageFloorDb(float linL, float linR)` — power-mean → dB
- constants `eb::kQuietCeilingLin` (0.0631f), `eb::kFloorBlendAlpha` (0.3f)

- [ ] **Step 1: Write the failing test** — add to `tests/test_noisefloor.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/NoiseFloorMath.h"
using Catch::Matchers::WithinAbs;

TEST_CASE("isQuietBlock: below the ceiling is quiet, at/above is not") {
    CHECK (eb::isQuietBlock (0.003f, eb::kQuietCeilingLin));        // ~-50 dBFS quiet
    CHECK_FALSE (eb::isQuietBlock (0.5f, eb::kQuietCeilingLin));    // a loud sweep block
    CHECK_FALSE (eb::isQuietBlock (eb::kQuietCeilingLin, eb::kQuietCeilingLin)); // exactly at ceiling
    CHECK_FALSE (eb::isQuietBlock (std::nanf(""), eb::kQuietCeilingLin));        // non-finite is not quiet
}

TEST_CASE("robustLowFloor: median rejects a transient spike in an otherwise-quiet window") {
    float w[] = { 0.0031f, 0.0030f, 0.0032f, 0.5f, 0.0031f };   // one cough at 0.5
    CHECK_THAT (eb::robustLowFloor (w, 5), WithinAbs (0.0031f, 1e-4)); // median, not the spike
    CHECK (eb::robustLowFloor (nullptr, 0) == 0.0f);
}

TEST_CASE("blendFloor: slow EMA toward the candidate; uninitialized adopts it") {
    CHECK_THAT (eb::blendFloor (0.0f, 0.004f, eb::kFloorBlendAlpha), WithinAbs (0.004f, 1e-6)); // 0 -> adopt
    CHECK_THAT (eb::blendFloor (0.004f, 0.006f, 0.5f), WithinAbs (0.005f, 1e-6));               // halfway
}

TEST_CASE("averageFloorDb: power-mean of two channel floors, in dB") {
    // equal channels -> same level; -40 dBFS both -> -40 dBFS
    CHECK_THAT (eb::averageFloorDb (0.01f, 0.01f), WithinAbs (-40.0f, 0.1f));
    // one silent channel pulls the power-mean down ~3 dB
    CHECK_THAT (eb::averageFloorDb (0.01f, 0.0f), WithinAbs (-43.0f, 0.2f));
}
```

- [ ] **Step 2: Run it to verify it fails** — `./tools/dev.cmd cmake --build build --target eb_tests` → FAIL: `NoiseFloorMath.h` not found. (After adding to CMakeLists in Step 3.)

- [ ] **Step 3: Register the test + implement** — add `test_noisefloor.cpp` to `tests/CMakeLists.txt` (after line `    test_freezegate.cpp`), then create `src/audio/NoiseFloorMath.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
namespace eb {

// "Quiet" = below a loose absolute ceiling that no real sweep block sits under. Reused -24 dBFS
// (kGoodLevelLinear): a healthy sweep peaks above it, the gap/pre-sweep silence sit well below it.
// Keyed off an absolute ceiling, not a recent peak, so the FIRST (pre-sweep) window has no prior
// loud reference to need. Non-finite is never quiet. On-device-tunable.
constexpr float kQuietCeilingLin = 0.0631f;   // ~-24 dBFS
constexpr float kFloorBlendAlpha = 0.3f;      // slow per-fold EMA toward a new quiet-window estimate

[[nodiscard]] inline bool isQuietBlock (float level, float ceiling) noexcept {
    return std::isfinite (level) && level < ceiling;
}

// Median of a quiet-window's per-block levels. Median (not mean) rejects a transient bump a cough/
// creak would add (Prawda, Schlecht & Valimaki, JASA 2022). Sorts in place; 0 for an empty window.
[[nodiscard]] inline float robustLowFloor (float* vals, int n) noexcept {
    if (vals == nullptr || n <= 0) return 0.0f;
    std::sort (vals, vals + n);
    return vals[n / 2];
}

// Slow EMA toward a new quiet-window estimate; an uninitialized floor (<=0) adopts the candidate so
// the first baseline is exact. Slow alpha so the steady amp hiss converges without per-gap jitter.
[[nodiscard]] inline float blendFloor (float current, float candidate, float alpha) noexcept {
    if (current <= 0.0f) return candidate;
    return current + alpha * (candidate - current);
}

// Single user-facing number: the POWER mean (quadratic mean) of the two channel floors -> dB. Power-
// correct for combining levels; averaging dB directly would be level-wrong. Tiny floor guards log(0).
[[nodiscard]] inline float averageFloorDb (float linL, float linR) noexcept {
    const float tiny = 1.0e-9f;
    const float p = 0.5f * (linL * linL + linR * linR);
    return 20.0f * std::log10 (std::sqrt (std::max (p, tiny)));
}

} // namespace eb
```

- [ ] **Step 4: Run tests to verify they pass** — `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build` → the 4 new cases PASS, full suite still green.

- [ ] **Step 5: Commit** — `git add src/audio/NoiseFloorMath.h tests/test_noisefloor.cpp tests/CMakeLists.txt` then commit `feat(audio): NoiseFloorMath pure helpers (quiet-gate, median floor, blend, power-avg)`.

---

### Task 2: `NoiseFloorTracker` (RT-safe wrapper)

**Files:**
- Create: `src/audio/NoiseFloorTracker.h`
- Test: append to `tests/test_noisefloor.cpp`

**Interfaces — Consumes:** Task 1 helpers. **Produces:**
- `class eb::NoiseFloorTracker` with `void prepare(double sr, int maxBlock)`, `void reset()`, `void observeBlock(float levelL, float levelR, double blockSeconds) noexcept`, `float floorLinear(int ch) const`, `float floorDb(int ch) const`, `float floorDbAveraged() const`, `bool valid() const`.
- `constexpr double eb::kFloorSustainSec = 0.5;`

- [ ] **Step 1: Write the failing test** — append to `tests/test_noisefloor.cpp`:

```cpp
#include "audio/NoiseFloorTracker.h"

namespace {
// feed `blocks` blocks of constant per-ear level at 10 ms each
void feed (eb::NoiseFloorTracker& t, float lvlL, float lvlR, int blocks) {
    for (int i = 0; i < blocks; ++i) t.observeBlock (lvlL, lvlR, 0.010);
}
}

TEST_CASE("NoiseFloorTracker: invalid until a sustained quiet window, then baselines from silence") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    CHECK_FALSE (t.valid());
    feed (t, 0.0040f, 0.0030f, 10);   // 100 ms quiet — not yet sustained (need >=500 ms)
    CHECK_FALSE (t.valid());
    feed (t, 0.0040f, 0.0030f, 50);   // now >500 ms of quiet -> baseline captured
    CHECK (t.valid());
    CHECK_THAT (t.floorLinear (0), Catch::Matchers::WithinAbs (0.0040f, 5e-4));
    CHECK_THAT (t.floorLinear (1), Catch::Matchers::WithinAbs (0.0030f, 5e-4));  // L/R independent
}

TEST_CASE("NoiseFloorTracker: a loud sweep does not corrupt the floor; gap refines it") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    feed (t, 0.0050f, 0.0050f, 60);   // pre-sweep silence -> baseline ~0.005
    REQUIRE (t.valid());
    feed (t, 0.4f, 0.4f, 60);         // a loud sweep -> NOT quiet -> floor unchanged
    CHECK_THAT (t.floorLinear (0), Catch::Matchers::WithinAbs (0.0050f, 1e-3));
    feed (t, 0.0030f, 0.0030f, 60);   // inter-sweep gap, quieter -> floor refines downward (slowly)
    CHECK (t.floorLinear (0) < 0.0050f);
    CHECK (t.floorLinear (0) > 0.0030f);   // slow blend, not a jump to the new value
}

TEST_CASE("NoiseFloorTracker: averaged readout is the power-mean of the two channels") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    feed (t, 0.0100f, 0.0100f, 60);
    CHECK_THAT (t.floorDbAveraged(), Catch::Matchers::WithinAbs (-40.0f, 0.5f));
}
```

- [ ] **Step 2: Run it to verify it fails** — build → FAIL: `NoiseFloorTracker.h` not found.

- [ ] **Step 3: Implement** — create `src/audio/NoiseFloorTracker.h`:

```cpp
#pragma once
#include "audio/NoiseFloorMath.h"
#include <array>
#include <atomic>
namespace eb {

constexpr double kFloorSustainSec = 0.5;   // sustained quiet before a window folds (AES-2id pre-sweep >=0.5s)

// Per-channel measured ambient/amp-hiss floor. Fed the per-ear block PEAK every capture block; folds a
// sustained quiet window (>= kFloorSustainSec of consecutive below-ceiling blocks) into the floor via a
// median + slow blend. RT-safe: plain loops + atomics; the only non-trivial op is a bounded std::sort
// over <=kWindowCap elements at fold time (infrequent, no alloc).
class NoiseFloorTracker {
public:
    void prepare (double sampleRate, int /*maxBlock*/) noexcept { sr_ = sampleRate; reset(); }

    void reset() noexcept {
        for (int c = 0; c < 2; ++c) {
            count_[c] = 0; quietSec_[c] = 0.0;
            floorMilli_[c].store (0);
        }
        valid_.store (false);
    }

    void observeBlock (float levelL, float levelR, double blockSeconds) noexcept {
        const float lv[2] = { levelL, levelR };
        for (int c = 0; c < 2; ++c) {
            if (isQuietBlock (lv[c], kQuietCeilingLin)) {
                if (count_[c] < kWindowCap) win_[c][count_[c]++] = lv[c];
                quietSec_[c] += blockSeconds;
                if (quietSec_[c] >= kFloorSustainSec) {
                    const float est = robustLowFloor (win_[c].data(), count_[c]);
                    const float cur = floorMilli_[c].load() / 1.0e6f;
                    const float next = blendFloor (cur, est, kFloorBlendAlpha);
                    floorMilli_[c].store ((int) std::lround (next * 1.0e6f));
                    valid_.store (true);
                    count_[c] = 0; quietSec_[c] = 0.0;   // start the next window fresh
                }
            } else {
                count_[c] = 0; quietSec_[c] = 0.0;       // a loud/non-quiet block breaks the run
            }
        }
    }

    float floorLinear (int ch) const noexcept { return floorMilli_[ch & 1].load() / 1.0e6f; }
    float floorDb (int ch) const noexcept {
        const float lin = floorLinear (ch);
        return 20.0f * std::log10 (std::max (lin, 1.0e-9f));
    }
    float floorDbAveraged() const noexcept { return averageFloorDb (floorLinear (0), floorLinear (1)); }
    bool  valid() const noexcept { return valid_.load(); }

private:
    static constexpr int kWindowCap = 256;   // ~2.5 s of 10 ms blocks; bounds the per-fold sort
    double sr_ = 48000.0;
    std::array<std::array<float, kWindowCap>, 2> win_ {};
    int    count_[2] { 0, 0 };
    double quietSec_[2] { 0.0, 0.0 };
    std::atomic<int> floorMilli_[2] { {0}, {0} };   // floor * 1e6 (linear), atomic-published
    std::atomic<bool> valid_ { false };
};

} // namespace eb
```

- [ ] **Step 4: Run tests to verify they pass** — build + ctest → the 3 tracker cases PASS, full suite green.

- [ ] **Step 5: Commit** — `git add src/audio/NoiseFloorTracker.h tests/test_noisefloor.cpp` then commit `feat(audio): NoiseFloorTracker — measured per-channel floor from quiet windows`.

---

### Task 3: Wire the tracker into `HealthMonitor`

**Files:**
- Modify: `src/audio/HealthMonitor.h` (add member + feed method + accessors; include `NoiseFloorTracker.h`)
- Modify: `src/audio/HealthMonitor.cpp` (reset the tracker in `reset()`)
- Test: append to `tests/test_healthmonitor.cpp`

**Interfaces — Consumes:** `NoiseFloorTracker`. **Produces (on `HealthMonitor`):**
- `void prepareNoiseFloor(double sampleRate, int maxBlock)`, `void observeFloorBlock(float pkL, float pkR, double blockSeconds) noexcept`, `float measuredFloorLinear(int ch) const`, `float measuredFloorDbAveraged() const`, `bool floorValid() const`.

- [ ] **Step 1: Write the failing test** — append to `tests/test_healthmonitor.cpp`:

```cpp
TEST_CASE("HealthMonitor: exposes the measured noise floor once a quiet window is captured") {
    eb::HealthMonitor hm; hm.prepareNoiseFloor (48000.0, 480);
    CHECK_FALSE (hm.floorValid());
    for (int i = 0; i < 60; ++i) hm.observeFloorBlock (0.004f, 0.004f, 0.010);  // >500ms quiet
    CHECK (hm.floorValid());
    CHECK_THAT (hm.measuredFloorLinear (0), Catch::Matchers::WithinAbs (0.004f, 5e-4));
    CHECK_THAT (hm.measuredFloorDbAveraged(), Catch::Matchers::WithinAbs (-48.0f, 1.0));
}
```

- [ ] **Step 2: Run it to verify it fails** — build → FAIL: no member `prepareNoiseFloor`.

- [ ] **Step 3: Implement** — in `src/audio/HealthMonitor.h`: add `#include "audio/NoiseFloorTracker.h"` near the includes; in the `public:` section (next to the SNR getters) add:

```cpp
    // ---- Measured noise floor (replaces the fixed armFloor SNR denominator) ----
    void prepareNoiseFloor (double sampleRate, int maxBlock) noexcept { floor_.prepare (sampleRate, maxBlock); }
    // Fed every capture block with the per-ear block PEAK (the engine already computes spkL/spkR). The
    // tracker self-gates on quiet windows, so this is called unconditionally. RT-safe.
    void observeFloorBlock (float pkL, float pkR, double blockSeconds) noexcept { floor_.observeBlock (pkL, pkR, blockSeconds); }
    float measuredFloorLinear (int ch) const noexcept { return floor_.floorLinear (ch); }
    float measuredFloorDbAveraged() const noexcept { return floor_.floorDbAveraged(); }
    bool  floorValid() const noexcept { return floor_.valid(); }
```

and add to the `private:` members: `NoiseFloorTracker floor_;`. In `src/audio/HealthMonitor.cpp` `reset()`, add `floor_.reset();` alongside the other latch resets.

- [ ] **Step 4: Run tests to verify they pass** — build + ctest → the new case PASS, full suite green.

- [ ] **Step 5: Commit** — `git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp` then commit `feat(health): own a NoiseFloorTracker; expose the measured floor`.

---

### Task 4: Feed the tracker from the engine + use the measured floor in SNR

**Files:**
- Modify: `src/audio/AudioEngine.cpp` (capture callback ~line 64; test seam ~line 751; both `evaluateSnr` call sites ~89 and ~761)
- Modify: `src/audio/AudioEngine.h` (add `prepare` hook call + a `noiseFloorDbAveraged()` getter)
- Test: append to `tests/test_audioengine.cpp` (drive `processCaptureBlockForTest` with quiet blocks → engine getter reports a floor)

**Interfaces — Consumes:** `HealthMonitor` floor accessors. **Produces (on `AudioEngine`):** `float noiseFloorDbAveraged() const noexcept; bool noiseFloorValid() const noexcept;`

- [ ] **Step 1: Write the failing test** — append to `tests/test_audioengine.cpp` (mirror the existing `processCaptureBlockForTest` pattern in that file):

```cpp
TEST_CASE("AudioEngine: measured noise floor populates from quiet capture blocks") {
    eb::AudioEngine eng; /* prepare as the sibling tests do */
    // feed >500 ms of quiet (low-level) blocks through the capture seam
    std::vector<float> q (480, 0.004f);
    for (int i = 0; i < 60; ++i) eng.processCaptureBlockForTest (q.data(), q.data(), 480);
    CHECK (eng.noiseFloorValid());
    CHECK (eng.noiseFloorDbAveraged() < -24.0f);   // a real (quiet) measured floor
}
```

- [ ] **Step 2: Run it to verify it fails** — build → FAIL: no `noiseFloorValid`.

- [ ] **Step 3: Implement** —
  1. In the capture callback (`src/audio/AudioEngine.cpp` after the `observeSweepPeak` block ~line 70) add, unconditionally:
     ```cpp
     e.hm.observeFloorBlock (spkL, spkR, (double) numSamples / e.captureRate_);
     ```
  2. Mirror it in the test seam (`processCaptureBlockForTest`, after its `observeSweepPeak` ~line 753):
     ```cpp
     hm.observeFloorBlock (spkL, spkR, (double) numSamples / captureRate_);
     ```
  3. At BOTH `evaluateSnr` call sites (~89 and ~761), replace the first arg `session_.armNoiseFloor()` / `e.session_.armNoiseFloor()` with the measured floor when valid:
     ```cpp
     const float snrFloor = e.hm.floorValid() ? e.hm.measuredFloorLinear (0) : e.session_.armNoiseFloor();
     const auto v = eb::evaluateSnr (snrFloor, e.hm.maxSweepPeakL(), e.hm.maxSweepPeakR(),
                                     e.session_.completedFloorStable() || e.hm.floorValid());
     ```
     *(Per-channel floor in the SNR — passing each ear its own floor — is deferred to the calibration grade (#27); `evaluateSnr` takes a single floor today. Using channel 0's measured floor here is the minimal, valid substitution.)*
  4. Where the engine is prepared (its `prepare`/start path), call `hm.prepareNoiseFloor (captureRate_, maxBlock)`. Add the getters to `AudioEngine.h` + `.cpp`:
     ```cpp
     float AudioEngine::noiseFloorDbAveraged() const noexcept { return hm.measuredFloorDbAveraged(); }
     bool  AudioEngine::noiseFloorValid()      const noexcept { return hm.floorValid(); }
     ```

- [ ] **Step 4: Run tests to verify they pass** — build + ctest → the new case PASS, **full suite green (390+ tests)**.

- [ ] **Step 5: Commit** — `git add src/audio/AudioEngine.cpp src/audio/AudioEngine.h tests/test_audioengine.cpp` then commit `feat(engine): feed the noise-floor tracker; use the measured floor in SNR when valid`.

---

## Notes for the implementer
- After all four tasks: run the full suite once more and confirm green; the feature branch `feat/noise-floor-primitive` is then ready for `finishing-a-development-branch`.
- The `kQuietCeilingLin`, `kFloorSustainSec`, the median percentile, and `kFloorBlendAlpha` are synthetic-tuned — leave the on-device ratification TODO in `NoiseFloorMath.h` as-is.
- Do NOT add per-band, SPL, or averaging-of-sweeps here — those belong to the calibration feature (#27).
