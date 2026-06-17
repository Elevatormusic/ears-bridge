# Confirmed, Measurement-Invalidating Clipping Detection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make EARS Bridge's clipping verdict honest and measurement-invalidating: detect a *confirmed digital clip* (a consecutive near-rail run) and a *non-finite sample* on the raw EARS input, latch both as **invalid measurement** (distinct from a dropout), collapse the two contradictory clip thresholds into one, and tell the user the truth.

**Architecture:** Extend the existing lock-free `HealthMonitor` (no new threads, no rewrite). Add a per-channel consecutive-rail-run detector and a NaN/Inf scan as two RT-safe `HealthMonitor` methods that the capture callback calls; route the existing test seam through the same methods so the production logic is unit-tested. Add two invalidating `HealthFlag`s and a pure status-message helper so the GUI can name the failure honestly.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

This plan implements audit findings **D1** (clip must invalidate), **D3** (one clip threshold), **D4** (NaN/Inf guard), and the confirmed-clip definition + statistics from `docs/EARS_DIRAC_CLIPPING_AUDIT.md` §7. **Out of scope** (named follow-up plans): D2 native-rate pin / shared-mode SRC (device-format plan), D5 sweep-session state machine (session plan), D6 frozen SRC ratio during the sweep (clock-domain plan), D7 combine-mode gating, and full `AudioIODeviceCallback` instantiation in tests. This plan still leaves the shared-mode-SRC caveat (D2) open — it makes the *detector* honest about runs, not the *device path* raw.

## Global Constraints

- **Build (tests):** `./tools/dev.cmd cmake --build build --target eb_tests` — run `tools/dev.cmd` **directly from Bash, never `cmd /c`** (it wraps Ninja + MSVC vcvars; CMake 3.30 has no VS-2026 generator).
- **Run a test:** `./tools/dev.cmd ctest --test-dir build -R "<name-regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green — currently 93 tests).
- **RT-safety (hard rule):** code reachable from an audio callback (everything in `HealthMonitor` `analyzeInputBlock`/`scanAndFlagNonFinite`, and the `CaptureCallback` body) must do **no** heap allocation, lock, syscall, logging, or exception. Use plain locals + `std::atomic` only. Cross-thread state is atomic; capture-thread-only scratch may be plain members written solely on that thread.
- **Do not** add `std::cout`/`DBG`/`jassert`-that-throws on the audio path.
- **HealthFlag** is not persisted — new enum values may be appended freely. (Unlike `CombineMode`, whose values are persisted; do not touch it here.)
- **No real EARS serial** in any file (tests use synthetic buffers only).
- **Commit trailer (every commit):**
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```
- Work on a feature branch, not `main` (the executing skill will create the worktree/branch).

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/EngineTypes.h` | `HealthFlag` enum | add `ClipConfirmed`, `NonFinite` |
| `src/audio/HealthMonitor.h` | telemetry surface + thresholds + members | add constants, methods, run/NaN state |
| `src/audio/HealthMonitor.cpp` | telemetry impl | add `analyzeInputBlock`, `scanAndFlagNonFinite`; extend `raise()` mask + `reset()` |
| `src/audio/AudioEngine.cpp` | capture/render callbacks + test seam | route raw input + processed mono through the new methods; unify the output clip bool |
| `src/gui/MainComponent.cpp` | status line + clip hint copy | honest confirmed-clip / non-finite branches; reword the near-rail hint |
| `src/gui/ClipStatus.h` | **new** — pure flag→message helper | testable status text |
| `tests/test_healthmonitor.cpp` | HealthMonitor unit tests | add confirmed-clip, NaN, threshold tests |
| `tests/test_audioengine.cpp` | engine seam tests | add clip-through-seam tests |
| `tests/test_clipstatus.cpp` | **new** — message helper tests | register in `tests/CMakeLists.txt` |

---

## Task 1: NaN/Inf guard + the two invalidating flags

**Files:**
- Modify: `src/audio/EngineTypes.h:15-24` (HealthFlag enum)
- Modify: `src/audio/HealthMonitor.h:46-60` (declare method), `:61-84` (members)
- Modify: `src/audio/HealthMonitor.cpp:8-18` (raise mask), `:29-39` (reset), add method body
- Test: `tests/test_healthmonitor.cpp`

**Interfaces:**
- Produces: `bool eb::HealthMonitor::scanAndFlagNonFinite(const float* buf, int n) noexcept` — returns true and raises `HealthFlag::NonFinite` (which clears `cleanCapture`) if any sample is non-finite; false otherwise. `HealthFlag::ClipConfirmed` / `HealthFlag::NonFinite` enum values (Task 2 raises `ClipConfirmed`).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("HealthMonitor: scanAndFlagNonFinite invalidates on NaN/Inf, ignores finite") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    const float clean[4] = { 0.0f, 0.5f, -0.5f, 0.25f };
    CHECK_FALSE (h.scanAndFlagNonFinite (clean, 4));
    CHECK (h.cleanCapture());
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::NonFinite));

    const float bad[4] = { 0.1f, std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.3f };
    CHECK (h.scanAndFlagNonFinite (bad, 4));
    CHECK (eb::any (h.flags() & eb::HealthFlag::NonFinite));
    CHECK_FALSE (h.cleanCapture());                 // NonFinite is invalidating

    const float inf[2] = { std::numeric_limits<float>::infinity(), 0.0f };
    eb::HealthMonitor h2; h2.prepare (eb::EarsModel::Ears, 4096);
    CHECK (h2.scanAndFlagNonFinite (inf, 2));
    CHECK_FALSE (h2.cleanCapture());
}
```
Add `#include <limits>` to the test file's includes if not present (it is not — add it next to `#include <cmath>` at `tests/test_healthmonitor.cpp:4`).

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `scanAndFlagNonFinite` / `HealthFlag::NonFinite` are undeclared.

- [ ] **Step 3: Add the enum values**

In `src/audio/EngineTypes.h`, change the `HealthFlag` enum (currently ending at `FifoStarved = 1u << 6`):
```cpp
enum class HealthFlag : unsigned {
    None        = 0,
    Xrun        = 1u << 0,
    Dropout     = 1u << 1,
    ExcessDrift = 1u << 2,
    ClipInput   = 1u << 3,   // guidance: near full scale (-1 dBFS); does NOT invalidate
    LowLevel    = 1u << 4,
    ClipOutput  = 1u << 5,   // guidance: program output hit the clamp; does NOT invalidate
    FifoStarved = 1u << 6,
    ClipConfirmed = 1u << 7, // INVALIDATING: a consecutive near-rail run = confirmed digital clip
    NonFinite     = 1u << 8  // INVALIDATING: a NaN/Inf sample reached the path
};
```

- [ ] **Step 4: Extend the invalidating mask and declare/implement the scan**

In `src/audio/HealthMonitor.cpp`, `raise()` (lines 8-18) — add the two flags to the mask:
```cpp
    const unsigned invalidating =
        static_cast<unsigned> (HealthFlag::Xrun)          |
        static_cast<unsigned> (HealthFlag::Dropout)       |
        static_cast<unsigned> (HealthFlag::ExcessDrift)   |
        static_cast<unsigned> (HealthFlag::FifoStarved)   |
        static_cast<unsigned> (HealthFlag::ClipConfirmed) |
        static_cast<unsigned> (HealthFlag::NonFinite);
```

In `src/audio/HealthMonitor.h`, declare the method in the public Plan-4 section (after `recentInputClip()`, near line 53):
```cpp
    // Scan a block for non-finite samples (NaN/Inf). Raises HealthFlag::NonFinite (invalidating)
    // and returns true if any are found, so the caller can zero the block before it reaches the cable.
    // RT-safe: a plain loop, no allocation. Called on the audio thread.
    bool scanAndFlagNonFinite (const float* buf, int n) noexcept;
```

In `src/audio/HealthMonitor.cpp`, add the body (near the other Plan-4 methods, e.g. after `reportOutLevel`, and ensure `#include <cmath>` is present — it is, at line 3):
```cpp
bool HealthMonitor::scanAndFlagNonFinite (const float* buf, int n) noexcept {
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (buf[i])) { raise (HealthFlag::NonFinite); return true; }
    return false;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "scanAndFlagNonFinite" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "feat(health): NaN/Inf scan + ClipConfirmed/NonFinite invalidating flags

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Confirmed-clip run detector + statistics

**Files:**
- Modify: `src/audio/HealthMonitor.h:28-34` (constants), `:46-60` (declare `analyzeInputBlock` + stat accessors), `:61-84` (members)
- Modify: `src/audio/HealthMonitor.cpp:29-39` (reset), add `analyzeInputBlock` body
- Test: `tests/test_healthmonitor.cpp`

**Interfaces:**
- Consumes: `HealthFlag::ClipConfirmed` (Task 1), `kClipLinear` (existing, `HealthMonitor.h:29`).
- Produces:
  - `void eb::HealthMonitor::analyzeInputBlock(const float* l, const float* r, int n) noexcept` — peaks both channels, detects a per-channel consecutive run of `>= kRailRunMin` samples with `|x| >= kRailCeiling` as a confirmed clip (raises `ClipConfirmed`), accumulates rail-sample counts + longest run, scans for non-finite (raises `NonFinite`), and feeds the existing guidance path (`reportInLevels` at the `kClipLinear` threshold). Replaces the inline peak loop in the capture callback.
  - `bool clipConfirmed() const noexcept`, `int clipRailSamples() const noexcept`, `int clipLongestRun() const noexcept`.
  - Constants `kRailCeiling` (0.9999f), `kRailRunMin` (3).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("HealthMonitor: a consecutive near-rail run is a confirmed clip; an isolated peak is not") {
    using eb::HealthFlag; using eb::any;
    // helper: build an L buffer, R silent
    auto runL = [] (eb::HealthMonitor& h, std::vector<float> l) {
        std::vector<float> r (l.size(), 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
    };

    SECTION ("positive rail run -> confirmed + invalid") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        runL (h, { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f });   // 3 consecutive at the rail
        CHECK (h.clipConfirmed());
        CHECK (any (h.flags() & HealthFlag::ClipConfirmed));
        CHECK_FALSE (h.cleanCapture());               // confirmed clip invalidates
        CHECK (h.clipLongestRun() >= 3);
        CHECK (h.clipRailSamples() >= 3);
    }
    SECTION ("negative rail run -> confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        runL (h, { -1.0f, -1.0f, -1.0f });
        CHECK (h.clipConfirmed());
        CHECK_FALSE (h.cleanCapture());
    }
    SECTION ("isolated full-scale peak (clean 0 dBFS sine sample) -> NOT confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        runL (h, { 0.7f, 1.0f, 0.7f, -1.0f, 0.7f });  // no run of 3
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());                     // stays valid
    }
    SECTION ("clean -1 dBFS constant -> near-rail guidance, NOT confirmed, still valid") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l (16, eb::HealthMonitor::kClipLinear);   // exactly -1.0 dBFS
        std::vector<float> r (16, 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), 16);
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());
        CHECK (any (h.flags() & HealthFlag::ClipInput));   // guidance fires
    }
    SECTION ("clean -0.1 dBFS -> neither confirmed nor near-rail-invalid") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l (16, 0.98855f);   // -0.1 dBFS, below kRailCeiling
        std::vector<float> r (16, 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), 16);
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());
    }
    SECTION ("right-channel-only rail run -> confirmed (per-channel)") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l (8, 0.1f);
        std::vector<float> r { 0.1f, 1.0f, 1.0f, 1.0f, 0.1f };
        h.analyzeInputBlock (l.data(), r.data(), (int) r.size());
        CHECK (h.clipConfirmed());
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `analyzeInputBlock`, `clipConfirmed`, `clipLongestRun`, `clipRailSamples`, `kRailCeiling` undeclared.

- [ ] **Step 3: Add constants, members, accessors (header)**

In `src/audio/HealthMonitor.h`, in the thresholds block (after `kClipLinear`, line 29):
```cpp
    // Confirmed digital clip: a RUN of consecutive samples at/above the rail. A real ADC clip is a
    // flat-topped run; a clean full-scale sine touches the rail for a single sample (no run). On a
    // shared-mode FLOAT stream we cannot read exact integer rails, so this run-based proxy is the
    // strongest reliable signal (see docs/EARS_DIRAC_CLIPPING_AUDIT.md D2). 24-bit +FS ~= 0.99999988.
    static constexpr float kRailCeiling = 0.9999f;   // -0.00087 dBFS
    static constexpr int   kRailRunMin  = 3;         // consecutive rail samples => confirmed clip
```
Declare the method + accessors in the public Plan-4 section (next to `scanAndFlagNonFinite`):
```cpp
    // Peak + confirmed-clip-run + NaN/Inf analysis on the RAW per-channel input. Replaces the inline
    // peak loop in the capture callback; feeds the existing guidance path via reportInLevels at the
    // kClipLinear (-1 dBFS) near-rail threshold. RT-safe (plain loop + atomics).
    void analyzeInputBlock (const float* l, const float* r, int n) noexcept;
    bool clipConfirmed()   const noexcept { return clipConfirmed_.load(); }
    int  clipRailSamples() const noexcept { return railSamplesL_.load() + railSamplesR_.load(); }
    int  clipLongestRun()  const noexcept { return longestRunA_.load(); }
```
Add members in the private section (after `recentClip_`/`reachedGood_`, near line 80):
```cpp
    // Confirmed-clip detection. railRun*_ / longestRun_ are CAPTURE-THREAD-ONLY scratch (written only
    // in analyzeInputBlock); the *_A_ atomics publish to the GUI thread.
    int  railRunL_ = 0, railRunR_ = 0, longestRun_ = 0;
    std::atomic<int>  railSamplesL_ { 0 }, railSamplesR_ { 0 }, longestRunA_ { 0 };
    std::atomic<bool> clipConfirmed_ { false };
```

- [ ] **Step 4: Reset the new state + implement analyzeInputBlock (cpp)**

In `src/audio/HealthMonitor.cpp` `reset()` (after `reachedGood_.store(false);`, line 37):
```cpp
    railRunL_ = railRunR_ = longestRun_ = 0;
    railSamplesL_.store (0); railSamplesR_.store (0); longestRunA_.store (0);
    clipConfirmed_.store (false);
```
Add the body (place it just above `reportInLevels`, since it calls it):
```cpp
void HealthMonitor::analyzeInputBlock (const float* l, const float* r, int n) noexcept {
    float pkL = 0.0f, pkR = 0.0f;
    int   railL = 0, railR = 0;
    bool  nonFinite = false, confirmed = false;
    for (int i = 0; i < n; ++i) {
        const float a = l[i], b = r[i];
        if (! std::isfinite (a) || ! std::isfinite (b)) { nonFinite = true; railRunL_ = railRunR_ = 0; continue; }
        const float ma = std::abs (a), mb = std::abs (b);
        pkL = juce::jmax (pkL, ma);  pkR = juce::jmax (pkR, mb);
        railRunL_ = (ma >= kRailCeiling) ? railRunL_ + 1 : 0;
        railRunR_ = (mb >= kRailCeiling) ? railRunR_ + 1 : 0;
        if (ma >= kRailCeiling) ++railL;
        if (mb >= kRailCeiling) ++railR;
        longestRun_ = juce::jmax (longestRun_, juce::jmax (railRunL_, railRunR_));
        if (railRunL_ >= kRailRunMin || railRunR_ >= kRailRunMin) confirmed = true;
    }
    if (railL > 0) railSamplesL_.fetch_add (railL);
    if (railR > 0) railSamplesR_.fetch_add (railR);
    longestRunA_.store (longestRun_);
    if (nonFinite)  raise (HealthFlag::NonFinite);
    if (confirmed) { clipConfirmed_.store (true); raise (HealthFlag::ClipConfirmed); }
    // Guidance path (peak meter + near-rail ClipInput + low-level + reached-good), unified on kClipLinear.
    reportInLevels (pkL, pkR, pkL >= kClipLinear, pkR >= kClipLinear);
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "consecutive near-rail" --output-on-failure`
Expected: PASS (all six sections).

- [ ] **Step 6: Commit**

```bash
git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "feat(health): confirmed-clip run detector + per-channel rail stats

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Route the capture path (and the test seam) through the new analysis

**Files:**
- Modify: `src/audio/AudioEngine.cpp:32-39` (CaptureCallback body), `:70-72` (RenderCallback output clip bool), `:347-354` (test seam)
- Test: `tests/test_audioengine.cpp`

**Interfaces:**
- Consumes: `HealthMonitor::analyzeInputBlock`, `scanAndFlagNonFinite` (Tasks 1-2); `HealthMonitor::kClipLinear`.
- Produces: the capture callback and `processCaptureBlockForTest` both run the *same* input analysis, so an engine-level test of the seam exercises the production logic. `prepareForTest` now resets `hm` so the seam starts clean.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_audioengine.cpp`:
```cpp
TEST_CASE("AudioEngine seam: a clipped raw input invalidates the measurement") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> inL { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };  // 3-sample rail run
    std::vector<float> inR (inL.size(), 0.0f);
    std::vector<float> mono (inL.size(), 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), (int) inL.size());
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine seam: a clean input stays valid") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> inL (8, 0.5f), inR (8, 0.3f), mono (8, 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), 8);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.cleanCapture());
}
```
(Confirm `tests/test_audioengine.cpp` already includes `"audio/AudioEngine.h"` and `<vector>`; add `<vector>` if missing.)

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "AudioEngine seam" --output-on-failure`
Expected: FAIL — `cleanCapture()` is true / `ClipConfirmed` absent, because the seam only runs `graph.process`.

- [ ] **Step 3: Wire the capture callback**

In `src/audio/AudioEngine.cpp` `CaptureCallback::audioDeviceIOCallbackWithContext`, replace the input-peak block + the `graph.process`/`pushCapture` pair (lines 32-39):
```cpp
        const float* l = in[0]; const float* r = in[1];
        e.hm.analyzeInputBlock (l, r, numSamples);             // peak + confirmed-clip-run + NaN/Inf (RAW input)

        e.graph.process (l, r, mono.data(), numSamples);       // per-ear FIR + combine
        if (e.hm.scanAndFlagNonFinite (mono.data(), numSamples))   // a non-finite sample would corrupt Dirac
            juce::FloatVectorOperations::clear (mono.data(), numSamples);
        e.bridge.pushCapture (mono.data(), numSamples);
```
(Removes the old `float pkL=0,pkR=0; for(...) ...; e.hm.reportInLevels(pkL,pkR,pkL>=0.999f,pkR>=0.999f);`.)

- [ ] **Step 4: Unify the output clip threshold (D3)**

In `src/audio/AudioEngine.cpp` `RenderCallback`, change line 72 from the magic literal to the shared constant so `Levels.clipOut` and `HealthFlag::ClipOutput` agree:
```cpp
        e.hm.reportOutLevel (pk, pk >= HealthMonitor::kClipLinear);   // unified near-rail threshold
```

- [ ] **Step 5: Route the test seam + reset hm in prepareForTest**

In `src/audio/AudioEngine.cpp` (lines 347-354):
```cpp
void AudioEngine::prepareForTest (double sampleRate, int block) {
    activeRate = sampleRate; blockSize = block;
    graph.prepare (sampleRate, block);
    hm.prepare (eb::EarsModel::Ears, juce::jmax (8192, block * 4));   // reset + size so the seam mirrors a run
}
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                              float* outMono, int numSamples) {
    hm.analyzeInputBlock (inL, inR, numSamples);                      // same analysis as the capture callback
    graph.process (inL, inR, outMono, numSamples);
    if (hm.scanAndFlagNonFinite (outMono, numSamples))
        juce::FloatVectorOperations::clear (outMono, numSamples);
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "AudioEngine seam" --output-on-failure`
Expected: PASS. Then run the full suite to catch regressions (the input meter now clips at −1 dBFS, not 0.999 — `test_levelmeter` / `test_healthmonitor` level cases should still pass; if any asserted on the old 0.999 input bool, update it to the unified threshold):
`./tools/dev.cmd ctest --test-dir build --output-on-failure`
Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add src/audio/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "feat(engine): route capture + test seam through confirmed-clip + NaN analysis; unify output clip threshold

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Honest status wording (confirmed clip / corruption distinct from dropouts)

**Files:**
- Create: `src/gui/ClipStatus.h`
- Modify: `src/gui/MainComponent.cpp:646-690` (updateStatusLine), `:241-243` (inputClipHint copy)
- Create + register test: `tests/test_clipstatus.cpp`, `tests/CMakeLists.txt:1-20`

**Interfaces:**
- Consumes: `HealthFlag::ClipConfirmed`, `HealthFlag::NonFinite`, `Health` (`AudioEngine::health()`).
- Produces: `const char* eb::invalidMeasurementMessage(eb::HealthFlag flags)` — given the flags of an *invalid* capture (`cleanCapture == false`), returns the most-specific honest message. Used by `updateStatusLine`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_clipstatus.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "gui/ClipStatus.h"
#include <string>

TEST_CASE("invalidMeasurementMessage names the most specific cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::NonFinite))
           == "Measurement invalidated by a corrupted audio sample.");
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::Dropout))
           == "Dropouts detected - this measurement is invalid.");
    // Confirmed clip takes precedence over a co-occurring dropout (most actionable for the user).
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::Dropout))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
}
```
Register it in `tests/CMakeLists.txt` by adding `    test_clipstatus.cpp` to the `add_executable(eb_tests ...)` list (after `test_updatecheck.cpp`).

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `gui/ClipStatus.h` does not exist.

- [ ] **Step 3: Create the pure helper**

Create `src/gui/ClipStatus.h`:
```cpp
#pragma once
#include "audio/EngineTypes.h"

namespace eb {

// Maps the flags of an INVALID capture (cleanCapture == false) to an honest, specific message.
// Order = most actionable first. Pure + header-only so it is unit-testable without the GUI.
inline const char* invalidMeasurementMessage (HealthFlag flags) noexcept {
    if (any (flags & HealthFlag::ClipConfirmed))
        return "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.";
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    return "Dropouts detected - this measurement is invalid.";
}

} // namespace eb
```

- [ ] **Step 4: Use it in the status line + reword the near-rail hint**

In `src/gui/MainComponent.cpp`, add the include near the top with the other `gui/` includes:
```cpp
#include "gui/ClipStatus.h"
```
In `updateStatusLine()` replace the `if (! h.cleanCapture) { ... "Dropouts detected" ... }` branch (lines 650-652) with:
```cpp
        if (! h.cleanCapture) {
            statusLine.setText (eb::invalidMeasurementMessage (h.flags), juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::danger());
        } else if (any (h.flags & HealthFlag::ClipOutput)) {
```
Reword the transient near-rail hint (lines 241-243) so it no longer asserts analog overload ("overloading") or sweep timing ("on the sweep") — it fires at −1 dBFS, which is a *near-rail warning*, not confirmed clipping:
```cpp
    inputClipHint.setText ("Input near full scale - if the meter hits the top, lower the EARS gain "
                           "switch a step and/or Dirac's playback level, then re-measure.",
                           juce::dontSendNotification);
```

- [ ] **Step 5: Run tests + build the app**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "invalidMeasurementMessage" --output-on-failure`
Expected: PASS.
Then confirm the app still builds: `./tools/dev.cmd cmake --build build --target EarsBridge`
Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git add src/gui/ClipStatus.h src/gui/MainComponent.cpp tests/test_clipstatus.cpp tests/CMakeLists.txt
git commit -m "feat(ui): name confirmed clip / corruption as invalid measurements; reword near-rail hint

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (run before finishing)

- [ ] Full suite green: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (expect ≥ 93 + the new cases, 0 failed).
- [ ] App builds: `./tools/dev.cmd cmake --build build --target EarsBridge`.
- [ ] Manual smoke (optional, real hardware): an over-driven sweep now shows the red **"Input reached digital full scale - this measurement is invalid…"** status (not green "clean"); a clean loud sweep at ~−0.5 dBFS does **not** print that or "overloading."

Then use **superpowers:finishing-a-development-branch** to verify and integrate.

---

## Self-review

**Spec coverage (against `docs/EARS_DIRAC_CLIPPING_AUDIT.md`):**
- D1 (clip must invalidate) → Task 2 raises `ClipConfirmed` (invalidating), Task 3 wires it, Task 4 surfaces it. ✓
- D3 (one clip threshold) → Task 2/3 unify the near-rail meter+flag on `kClipLinear`; confirmed clip is the separate run-based `kRailCeiling`. ✓
- D4 (NaN/Inf) → Task 1 method + Task 3 wiring (zeroes the processed block). ✓
- Confirmed-clip definition + statistics (audit §7) → Task 2 (run detector, rail count, longest run). ✓
- R22 (tests exercise the production path) → Task 3 routes the seam through the same methods the callback calls; Tasks 1-2 unit-test those methods directly. Partial: full `AudioIODeviceCallback` instantiation is explicitly deferred. ✓ (noted)
- **Deferred (named follow-ups, by design):** D2 native-rate pin / shared-mode SRC; D5 sweep-session machine; D6 frozen SRC ratio during the sweep; D7 combine-mode gating; D9 pre-clamp output peak. These do not block this plan's deliverable but the no-clipping claim still needs D2/D5/D6 before it is fully defensible.

**Placeholder scan:** every code step contains complete, compilable content; no "TBD"/"add error handling"/"similar to" placeholders.

**Type consistency:** `analyzeInputBlock(const float*, const float*, int)`, `scanAndFlagNonFinite(const float*, int)`, `clipConfirmed()/clipRailSamples()/clipLongestRun()`, `invalidMeasurementMessage(HealthFlag)`, `kRailCeiling`/`kRailRunMin`/`kClipLinear` are used identically in every task that references them. `HealthFlag::ClipConfirmed`/`NonFinite` are defined once (Task 1) and consumed consistently.
