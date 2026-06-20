# Measurement-Session State Machine Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Scope clipping/integrity validity to Dirac's actual sweep window, not the whole Start->Stop engine run. Add a `MeasurementSession` state machine (Idle -> Preflight -> SweepActive -> Complete/Invalid) driven by an input LEVEL THRESHOLD. Clear the clip/integrity latches ONCE at the genuine sweep onset; retain events during the sweep; ignore pre-sweep room events. Dirac measures BOTH earcups (left sweep -> gap -> right sweep) in one routine, so the validity window must span the WHOLE measurement, not just the first sweep. This closes audit finding **D5 / requirement R18**.

**Architecture:** A new pure, lock-free `eb::MeasurementSession` (no JUCE audio deps, atomics + plain locals only — same shape as `LrVerify`) owned by `AudioEngine`. The capture callback computes the current block's input peak and feeds it to the session BEFORE analyzing the block; on the FIRST sweep-onset edge it calls `HealthMonitor::resetMeasurementLatches()` (validity-only) so a pre-sweep room event doesn't brand the sweep invalid, then analyzes the current block so a clip ON the onset block still counts. The session phase is published via an atomic the GUI reads on its 30 Hz timer and that **D6 (frozen SRC ratio)** will later consume via `sweepActive()`.

**Tech Stack:** C++17, JUCE 8.0.4, Catch2 (`Catch2WithMain`), CMake + Ninja + MSVC via `tools/dev.cmd`.

## Pre-build review applied (why this plan differs from a naive D5)

This plan was reviewed against CURRENT `main` (the clipping-review-fixes + D2 raw-rail slices both merged) by a 4-angle workflow before writing. Five critical corrections are baked in; do NOT undo them:

1. **Do not revert the merged NaN-poison fix.** `graph.process(...)` now returns `bool` and sanitizes input+output internally; the capture callback is `if (e.graph.process(...)) e.hm.reportNonFinite();` with NO `scanAndFlagNonFinite(mono)`+`clear`. The D5 session lines are INSERTED around the existing callback body — never a wholesale replacement (which would silently revert the fix and drop the null guard + dropped-frames block).
2. **`resetMeasurementLatches()` must PRESERVE D2's `OsResampled`** (a per-run guidance flag, raised once in `start()`) — clear all sticky flags EXCEPT it via `flagBits.fetch_and((unsigned)HealthFlag::OsResampled)` — and must also reset the fix-slice's flat-run scratch `prevL_/prevR_`.
3. **Current-block-peak arming (no lag).** The session is fed the CURRENT block's peak (a cheap no-latch `HealthMonitor::blockPeak`), not the prior block's quantized meter value, so the sweep arms with zero lag and a clip on the onset block is retained.
4. **Re-armable, time-based `Complete` for Dirac's two sweeps.** `Complete` is provisional: a fresh sustained rise re-enters `SweepActive` WITHOUT re-clearing latches, so a clip on the RIGHT earcup still invalidates. The terminal-silence window is expressed in TIME (configured from block size + sample rate), not raw blocks.
5. **Single-writer `phase_`.** Only the capture thread writes the session phase. The render callback does NOT call `markInvalid()`; a render-side dropout latches `cleanCapture=false`, which the next capture block re-derives into `Invalid`.

**On-device ratification (like the D2 smoke, not headless-testable):** the exact `kSweepStartLinear` and `kSilenceCompleteSeconds` should be confirmed against one real EARS+Dirac sweep capture. The chosen defaults (-24 dBFS arm, 3-block sustain, 1.5 s terminal silence) are designed to be correct for any realistic gap/level; ratification only tunes them.

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests` — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never cmd /c; it wraps Ninja + MSVC.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; **109 tests today**).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + std::atomic only.
- HealthFlag (src/audio/EngineTypes.h) is NOT persisted — append enum values freely. Current highest bit is `OsResampled = 1u << 9`. D5 adds NO HealthFlag (it adds a separate `SessionPhase` enum). CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

## File Structure

| File | Create/Modify | Responsibility |
|---|---|---|
| `src/audio/MeasurementSession.h` | Create | Pure lock-free `MeasurementSession`: `observeBlockPeak(float)` advances Idle->Preflight->SweepActive->Complete (re-armable) via a sustained level threshold; `configure(blockSize,sampleRate)`; `consumeSweepStarted()`; `markInvalid()`; `reset()`; `phase()`/`sweepActive()`/`inMeasurement()`. Atomics-only. |
| `src/audio/MeasurementSession.cpp` | Create | State-transition bodies. |
| `src/audio/EngineTypes.h` | Modify | Add `SessionPhase` enum + a `SessionPhase session` field in `Health` (additive, not persisted). |
| `src/audio/HealthMonitor.h` | Modify | Add static `blockPeak(l,r,n)` (no-latch max-abs); add `resetMeasurementLatches()` (validity-only, preserves OsResampled + config + telemetry). |
| `src/audio/HealthMonitor.cpp` | Modify | Implement `resetMeasurementLatches()`. |
| `src/audio/AudioEngine.h` | Modify | Own `MeasurementSession session_`; add `sessionPhase()`/`sweepActive()`. |
| `src/audio/AudioEngine.cpp` | Modify | Capture callback: feed the session the current block peak, re-scope on the sweep-onset edge, mark Invalid in-measurement. Snapshot phase into `Health.session`. `configure()`+`reset()` the session in `start()`/`prepareForTest()`. |
| `src/gui/MainComponent.cpp` | Modify | In the Running branch of `updateStatusLine`, gate the verdict on the session phase. |
| `tests/test_measurementsession.cpp` | Create | Unit tests for the pure state machine. |
| `tests/test_healthmonitor.cpp` | Modify | `resetMeasurementLatches` test (incl. OsResampled survives). |
| `tests/test_audioengine.cpp` | Modify | Seam tests: pre-sweep events dropped, in-sweep events score, two-sweep re-arm. |
| `tests/CMakeLists.txt` | Modify | Add `test_measurementsession.cpp`. |

---

### Task 1: Pure `MeasurementSession` state machine (no AudioEngine wiring yet)

A self-contained, lock-free phase machine driven by per-block input peaks plus an external `markInvalid()`. It does NOT know about HealthMonitor — the engine wires the reset edge in Task 3. Mirrors `LrVerify`'s "pure state machine, touched only on the audio thread, GUI reads an atomic" contract.

**Files:** `src/audio/MeasurementSession.h`, `src/audio/MeasurementSession.cpp`, `src/audio/EngineTypes.h`, `tests/test_measurementsession.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: per-block input peak `float` (max of the two channel peaks), and an external `markInvalid()`.
- Produces:
  - `enum class eb::SessionPhase { Idle, Preflight, SweepActive, Complete, Invalid }`
  - `void reset() noexcept`
  - `void configure (int blockSize, double sampleRate) noexcept` — sizes the terminal-silence window in TIME
  - `void observeBlockPeak (float peak) noexcept`
  - `bool consumeSweepStarted() noexcept` — true exactly once, on the FIRST (genuine) sweep onset (drives the one latch re-scope); re-arms after a provisional Complete do NOT fire it
  - `void markInvalid() noexcept`
  - `SessionPhase phase() const noexcept`, `bool sweepActive() const noexcept`, `bool inMeasurement() const noexcept`
  - public constants: `kSweepStartLinear` (-24 dBFS), `kArmSustainBlocks` (3), `kSilenceFloorLinear` (-50 dBFS), `kSilenceCompleteSeconds` (1.5), `kDefaultSilenceBlocks` (140)

Steps:

- [ ] Add the `SessionPhase` enum to `src/audio/EngineTypes.h` and a `session` field to `Health`. Put the enum next to `EngineStatus`:

```cpp
enum class EarsModel  { Unknown, Ears, EarsPro };
enum class EngineStatus { Stopped, Running, Error };

// D5 / R18: the measurement-session phase, scoping validity to Dirac's sweep window (left earcup ->
// gap -> right earcup) instead of the whole engine run. Driven by the input level threshold in
// MeasurementSession. NOT persisted.
enum class SessionPhase { Idle, Preflight, SweepActive, Complete, Invalid };
```

  And inside `struct Health` add one field (additive; default keeps existing snapshots unchanged). Match the real current struct — add the field after `flags`:

```cpp
    SessionPhase session = SessionPhase::Idle;   // D5: measurement-session phase snapshot
```

- [ ] Write the failing test `tests/test_measurementsession.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/MeasurementSession.h"

using eb::MeasurementSession;
using eb::SessionPhase;

static const float kLoud  = 0.5f;    // -6 dBFS, well above the -24 dB arm threshold
static const float kQuiet = 0.0f;    // below the -50 dB silence floor

static void blocks (MeasurementSession& s, int n, float peak) {
    for (int i = 0; i < n; ++i) s.observeBlockPeak (peak);
}
// Arm the sweep: kArmSustainBlocks consecutive loud blocks.
static void arm (MeasurementSession& s) { blocks (s, MeasurementSession::kArmSustainBlocks, kLoud); }

TEST_CASE("MeasurementSession starts Idle, a quiet warm-up moves to Preflight, not SweepActive") {
    MeasurementSession s; s.reset();
    CHECK (s.phase() == SessionPhase::Idle);
    blocks (s, 4, kQuiet);
    CHECK (s.phase() == SessionPhase::Preflight);
    CHECK_FALSE (s.consumeSweepStarted());
}

TEST_CASE("MeasurementSession arms only after a SUSTAINED rise (a single transient does not arm)") {
    MeasurementSession s; s.reset();
    blocks (s, 2, kQuiet);
    s.observeBlockPeak (kLoud);                       // 1 loud block: a door slam, not the sweep
    CHECK (s.phase() == SessionPhase::Preflight);     // not armed by one block
    s.observeBlockPeak (kQuiet);                      // the run breaks
    CHECK (s.phase() == SessionPhase::Preflight);
    arm (s);                                          // kArmSustainBlocks loud blocks: the sweep
    CHECK (s.phase() == SessionPhase::SweepActive);
    CHECK (s.consumeSweepStarted());                  // genuine onset fires exactly once
    CHECK_FALSE (s.consumeSweepStarted());
}

TEST_CASE("MeasurementSession: a peak just under the arm threshold never arms") {
    MeasurementSession s; s.reset();
    // 0.05 (~ -26 dBFS) is below kSweepStartLinear (-24 dBFS): typical room noise must not arm.
    blocks (s, MeasurementSession::kArmSustainBlocks + 5, 0.05f);
    CHECK (s.phase() == SessionPhase::Preflight);
    CHECK_FALSE (s.consumeSweepStarted());
}

TEST_CASE("MeasurementSession reaches Complete after sustained silence following signal") {
    MeasurementSession s; s.reset();
    arm (s);
    CHECK (s.phase() == SessionPhase::SweepActive);
    blocks (s, MeasurementSession::kDefaultSilenceBlocks - 1, kQuiet);
    CHECK (s.phase() == SessionPhase::SweepActive);             // not yet
    s.observeBlockPeak (kQuiet);                                // crosses the sustain threshold
    CHECK (s.phase() == SessionPhase::Complete);
}

TEST_CASE("MeasurementSession: a brief gap mid-sweep does not end the sweep") {
    MeasurementSession s; s.reset();
    arm (s);
    blocks (s, MeasurementSession::kDefaultSilenceBlocks - 1, kQuiet);   // a gap, not sustained
    s.observeBlockPeak (kLoud);                                          // signal returns -> resets the run
    blocks (s, MeasurementSession::kDefaultSilenceBlocks - 1, kQuiet);
    CHECK (s.phase() == SessionPhase::SweepActive);
}

TEST_CASE("MeasurementSession: the SECOND Dirac sweep re-arms after a provisional Complete, no re-clear") {
    MeasurementSession s; s.reset();
    arm (s);                                                            // left earcup sweep
    CHECK (s.consumeSweepStarted());                                    // drained the one re-scope edge
    blocks (s, MeasurementSession::kDefaultSilenceBlocks + 2, kQuiet);  // long inter-sweep gap
    CHECK (s.phase() == SessionPhase::Complete);                        // provisional complete
    arm (s);                                                            // right earcup sweep
    CHECK (s.phase() == SessionPhase::SweepActive);                     // re-armed
    CHECK_FALSE (s.consumeSweepStarted());                              // re-arm does NOT re-scope latches
}

TEST_CASE("MeasurementSession: markInvalid latches Invalid and is absorbing") {
    MeasurementSession s; s.reset();
    arm (s);
    s.markInvalid();
    CHECK (s.phase() == SessionPhase::Invalid);
    arm (s);                                                            // further audio cannot un-invalidate
    blocks (s, MeasurementSession::kDefaultSilenceBlocks + 2, kQuiet);
    CHECK (s.phase() == SessionPhase::Invalid);
}

TEST_CASE("MeasurementSession: configure sizes the terminal silence in TIME, not raw blocks") {
    MeasurementSession s; s.reset();
    s.configure (256, 48000.0);   // 1.5 s @ 256/48k ~= 281 blocks
    arm (s);
    blocks (s, 280, kQuiet);
    CHECK (s.phase() == SessionPhase::SweepActive);   // 280 < ~281: not yet complete at the smaller block
    blocks (s, 3, kQuiet);
    CHECK (s.phase() == SessionPhase::Complete);
}

TEST_CASE("MeasurementSession: reset returns to Idle and clears the arm run") {
    MeasurementSession s; s.reset();
    s.observeBlockPeak (kLoud); s.observeBlockPeak (kLoud);   // partial arm run
    s.reset();
    CHECK (s.phase() == SessionPhase::Idle);
    arm (s);                                                  // a fresh full run arms cleanly
    CHECK (s.phase() == SessionPhase::SweepActive);
}
```

- [ ] Add `test_measurementsession.cpp` to `tests/CMakeLists.txt`. Find the `eb_tests` source list (it lists `test_*.cpp` explicitly — `test_clipstatus.cpp`, `test_rawrailstatus.cpp`, etc.) and add the new file alongside them.

- [ ] Create the header `src/audio/MeasurementSession.h`:

```cpp
#pragma once
#include "audio/EngineTypes.h"   // eb::SessionPhase
#include <atomic>
#include <cmath>

namespace eb {

// Lock-free measurement-session state machine (D5 / R18). Scopes clip/integrity validity to Dirac's
// actual sweep window (left earcup -> gap -> right earcup) instead of the whole Start->Stop run.
// Driven on the CAPTURE thread by the per-block input peak; the GUI reads phase() via an atomic
// (never the internal scratch). Pure: no JUCE audio deps, no alloc, no lock — same contract as LrVerify.
//
// Phases:
//   Idle        - fresh, before the first observed block
//   Preflight   - armed; input is below the sweep-start threshold (room noise / silence)
//   SweepActive - input sustained above the start threshold; THIS is the validity window. D6 will read
//                 sweepActive() to freeze the ClockBridge SRC ratio for the sweep.
//   Complete    - sustained silence after signal. PROVISIONAL: a fresh sustained rise re-arms
//                 SweepActive (the right-earcup sweep / a re-take) WITHOUT re-clearing latches.
//   Invalid     - an invalidating condition fired in-measurement (latched; absorbing)
class MeasurementSession {
public:
    // Sustained input peak (linear) that arms the sweep. -24 dBFS == the app's healthy-level floor
    // (HealthMonitor::kGoodLevelLinear): a correctly-leveled Dirac sweep (the app targets -18..-12 dBFS)
    // clears it, while typical room noise (~ -30 dBFS) stays below. Arming also requires kArmSustainBlocks
    // consecutive blocks so a single loud transient (door slam, cable bump) cannot arm.
    static constexpr float kSweepStartLinear      = 0.06310f;   // -24 dBFS
    static constexpr int   kArmSustainBlocks      = 3;          // consecutive above-threshold blocks to arm
    // Below this counts as silence for the sweep-end detector. Same -50 dBFS no-signal floor the level
    // guidance uses, kept independent so the two can't desync by accident.
    static constexpr float kSilenceFloorLinear    = 0.00316f;   // -50 dBFS
    // Terminal post-signal silence that ends a sweep segment, expressed in TIME. configure() converts
    // it to a block count for the real block size; kDefaultSilenceBlocks is the pre-configure default
    // (~1.5 s @ 512/48k) used by unit tests. 1.5 s is long enough that the quiet tail of a sweep (or a
    // short inter-sweep gap) does not end it early; Complete is re-armable so a longer gap is also safe.
    static constexpr double kSilenceCompleteSeconds = 1.5;
    static constexpr int    kDefaultSilenceBlocks   = 140;

    void reset() noexcept;
    void configure (int blockSize, double sampleRate) noexcept;   // size the terminal-silence window

    // Advance the machine with one block's input peak (max of the two channel peaks). RT-safe.
    void observeBlockPeak (float peak) noexcept;

    // Edge: true exactly once, on the FIRST (genuine) sweep onset (Preflight -> SweepActive) — drives
    // the one validity re-scope in the engine. Re-arms out of a provisional Complete do NOT set it.
    bool consumeSweepStarted() noexcept;

    // Latch Invalid (an invalidating HealthMonitor flag fired in-measurement). Absorbing.
    void markInvalid() noexcept;

    SessionPhase phase() const noexcept { return static_cast<SessionPhase> (phase_.load()); }
    bool sweepActive()   const noexcept { return phase() == SessionPhase::SweepActive; }   // D6 consumer
    // True once the measurement has started (armed or later) — the engine marks Invalid only here, so a
    // pre-sweep room event in Idle/Preflight is never scored, but a same-block silence->Complete plus a
    // late-latched dropout still invalidates.
    bool inMeasurement() const noexcept {
        const auto p = phase();
        return p == SessionPhase::SweepActive || p == SessionPhase::Complete || p == SessionPhase::Invalid;
    }

private:
    // Shared, sustained-rise re-arm helper for Preflight (first onset) and Complete (re-take/2nd sweep).
    // Returns true when the kArmSustainBlocks run completes this block.
    bool advanceArmRun (float peak) noexcept;

    std::atomic<int>  phase_        { (int) SessionPhase::Idle };
    std::atomic<bool> sweepStarted_ { false };   // edge, drained by consumeSweepStarted()
    int  armRun_            = 0;                  // CAPTURE-THREAD scratch: consecutive above-threshold blocks
    int  silenceRun_        = 0;                  // CAPTURE-THREAD scratch: consecutive below-floor blocks in-sweep
    bool sawSignal_         = false;              // CAPTURE-THREAD scratch: signal seen since SweepActive entry
    int  silenceBlocksNeeded_ = kDefaultSilenceBlocks;   // configured terminal-silence count
};

} // namespace eb
```

- [ ] Create `src/audio/MeasurementSession.cpp`:

```cpp
#include "audio/MeasurementSession.h"
#include <algorithm>

namespace eb {

void MeasurementSession::reset() noexcept {
    phase_.store ((int) SessionPhase::Idle);
    sweepStarted_.store (false);
    armRun_ = 0; silenceRun_ = 0; sawSignal_ = false;
}

void MeasurementSession::configure (int blockSize, double sampleRate) noexcept {
    if (blockSize > 0 && sampleRate > 0.0) {
        const long n = std::lround (kSilenceCompleteSeconds * sampleRate / (double) blockSize);
        silenceBlocksNeeded_ = (int) std::max (1L, n);
    }
}

bool MeasurementSession::advanceArmRun (float peak) noexcept {
    if (peak >= kSweepStartLinear) {
        if (++armRun_ >= kArmSustainBlocks) { armRun_ = 0; return true; }
    } else {
        armRun_ = 0;
    }
    return false;
}

void MeasurementSession::observeBlockPeak (float peak) noexcept {
    if (static_cast<SessionPhase> (phase_.load()) == SessionPhase::Invalid)
        return;   // absorbing

    if (static_cast<SessionPhase> (phase_.load()) == SessionPhase::Idle)
        phase_.store ((int) SessionPhase::Preflight);

    const auto cur = static_cast<SessionPhase> (phase_.load());

    if (cur == SessionPhase::Preflight) {
        // FIRST sweep onset: arm on a sustained rise and fire the one re-scope edge.
        if (advanceArmRun (peak)) {
            phase_.store ((int) SessionPhase::SweepActive);
            sweepStarted_.store (true);   // genuine onset -> the single latch clear
            silenceRun_ = 0; sawSignal_ = true;
        }
        return;
    }

    if (cur == SessionPhase::Complete) {
        // Re-arm for the next sweep of the SAME Dirac measurement (right earcup) or a re-take, WITHOUT
        // re-clearing latches: a clip on either earcup must accrue into the one validity window.
        if (advanceArmRun (peak)) {
            phase_.store ((int) SessionPhase::SweepActive);
            silenceRun_ = 0; sawSignal_ = true;
        }
        return;
    }

    // SweepActive: track the silence tail. A sustained below-floor run after signal -> provisional
    // Complete; any above-floor block resets the run so a brief inter-segment gap doesn't end it early.
    if (peak < kSilenceFloorLinear) {
        if (sawSignal_ && ++silenceRun_ >= silenceBlocksNeeded_)
            phase_.store ((int) SessionPhase::Complete);
    } else {
        silenceRun_ = 0; sawSignal_ = true;
    }
}

bool MeasurementSession::consumeSweepStarted() noexcept {
    return sweepStarted_.exchange (false);
}

void MeasurementSession::markInvalid() noexcept {
    phase_.store ((int) SessionPhase::Invalid);
}

} // namespace eb
```

- [ ] Add `src/audio/MeasurementSession.cpp` to the `eb_engine` target in `CMakeLists.txt`, alongside `src/audio/HealthMonitor.cpp` (grep `HealthMonitor.cpp` to find the list). If sources are globbed, no edit is needed — verify.
- [ ] Build: `./tools/dev.cmd cmake --build build --target eb_tests`
- [ ] Run the new tests: `./tools/dev.cmd ctest --test-dir build -R "MeasurementSession" --output-on-failure`
- [ ] Run the full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
- [ ] Commit: `MeasurementSession state machine (D5/R18): re-armable level-threshold sweep boundary` with the trailer.

---

### Task 2: `HealthMonitor::blockPeak` + `resetMeasurementLatches()`

Two additions: (1) a no-latch static `blockPeak` so the engine can feed the session the CURRENT block's peak before `analyzeInputBlock`; (2) `resetMeasurementLatches()` that clears ONLY the validity latches on the sweep-onset edge while PRESERVING the per-run `OsResampled` guidance flag, the config, and the live telemetry.

**Files:** `src/audio/HealthMonitor.h`, `src/audio/HealthMonitor.cpp`, `tests/test_healthmonitor.cpp`

**Interfaces:**
- `static float HealthMonitor::blockPeak (const float* l, const float* r, int n) noexcept` — max |sample| over both channels (skips non-finite). No latching, no state.
- `void HealthMonitor::resetMeasurementLatches() noexcept` — clears `clean`, all sticky flags EXCEPT `OsResampled`, `recentClip_`, the rail-run scratch + atomics, `prevL_/prevR_`, and `driftRun`; leaves `model_`/`capacity_`/`nominal_`, the level atomics, `reachedGood_`, `blockCount`, and the FIFO counters untouched.

Steps:

- [ ] Write the failing test, appended to `tests/test_healthmonitor.cpp`:

```cpp
TEST_CASE("HealthMonitor::blockPeak is the max |sample| over both channels, ignoring non-finite") {
    std::vector<float> l { 0.1f, -0.7f, 0.2f }, r { 0.3f, 0.05f, -0.4f };
    CHECK (eb::HealthMonitor::blockPeak (l.data(), r.data(), 3) == Catch::Approx (0.7f));
    std::vector<float> ln { 0.2f, std::numeric_limits<float>::quiet_NaN(), 0.1f }, rn (3, 0.0f);
    CHECK (eb::HealthMonitor::blockPeak (ln.data(), rn.data(), 3) == Catch::Approx (0.2f));  // NaN skipped
}

TEST_CASE("HealthMonitor: resetMeasurementLatches clears validity but keeps config + telemetry + OsResampled") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 16384, 2.0);   // model + nominal ratio 2.0

    h.observeRenderBlock (256, 256, 2.0, 0.5);                 // advances blockCount; sets ratio/fill
    h.reportInLevels (0.10f, 0.02f, false, false);             // latches reachedGoodLevel
    h.reportRawRail (false);                                   // D2: per-run OsResampled guidance
    std::vector<float> rail (4, 1.0f), sil (4, 0.0f);
    h.analyzeInputBlock (rail.data(), sil.data(), 4);          // confirmed clip -> invalidates
    REQUIRE_FALSE (h.cleanCapture());
    REQUIRE (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
    REQUIRE (eb::any (h.flags() & eb::HealthFlag::OsResampled));

    h.resetMeasurementLatches();   // the sweep-start edge: re-scope validity to the sweep

    // Validity is cleared...
    CHECK (h.cleanCapture());
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (h.clipConfirmed());
    CHECK (h.clipLongestRun() == 0);
    // ...but the per-run OS-SRC guidance + run config + telemetry survive.
    CHECK (eb::any (h.flags() & eb::HealthFlag::OsResampled));            // D2 guidance kept
    CHECK (h.reachedGoodLevel());                                        // kept
    CHECK (std::abs (h.snapshot().captureToRenderRatio - 2.0) < 1e-6);   // ratio telemetry kept

    // And a fresh in-sweep confirmed clip still invalidates after the re-scope.
    h.analyzeInputBlock (rail.data(), sil.data(), 4);
    CHECK_FALSE (h.cleanCapture());
    CHECK (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
    // The in-sweep clip message still carries the OS-resampled caveat (OsResampled survived).
    CHECK (eb::invalidMeasurementMessage (h.flags()).contains ("OS-resampled"));
}
```
(Ensure `tests/test_healthmonitor.cpp` includes `<limits>`, `<cmath>`, and `"gui/ClipStatus.h"` for `invalidMeasurementMessage`; add whichever is missing.)

- [ ] Run it to see it fail to compile (methods missing): `./tools/dev.cmd cmake --build build --target eb_tests`
- [ ] Add `blockPeak` to `src/audio/HealthMonitor.h` (public, static, near the other peak helpers; needs `<cmath>` + `juce_core`):

```cpp
    // No-latch max |sample| over both channels for the CURRENT block (skips non-finite). Lets the
    // engine feed MeasurementSession the current block's peak BEFORE analyzeInputBlock, so the sweep
    // arms with zero lag and a clip ON the onset block is re-detected after the validity re-scope.
    static float blockPeak (const float* l, const float* r, int n) noexcept {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (std::isfinite (l[i])) pk = juce::jmax (pk, std::abs (l[i]));
            if (std::isfinite (r[i])) pk = juce::jmax (pk, std::abs (r[i]));
        }
        return pk;
    }
```

- [ ] Declare `resetMeasurementLatches` in `src/audio/HealthMonitor.h`, after the `prepare(...)` declaration:

```cpp
    // D5: clear ONLY the measurement-validity latches (sticky flags EXCEPT the per-run OsResampled
    // guidance, cleanCapture, clip-run + flat-run scratch, drift run, edge clip) WITHOUT touching the
    // per-run config (model/capacity/nominal) or the level/ratio telemetry the GUI is rendering. Called
    // on the sweep-onset edge so a clip/dropout BEFORE the sweep doesn't brand the sweep invalid.
    void resetMeasurementLatches() noexcept;
```

- [ ] Implement it in `src/audio/HealthMonitor.cpp`, after the `reset()` body:

```cpp
void HealthMonitor::resetMeasurementLatches() noexcept {
    clean.store (true);
    // Clear every sticky flag EXCEPT the per-run OS-SRC guidance (OsResampled is raised once in start()
    // after prepare() and reflects the whole run's stream; it must survive a mid-run sweep re-scope so
    // the in-sweep clip caveat still fires).
    flagBits.fetch_and (static_cast<unsigned> (HealthFlag::OsResampled));
    recentClip_.store (false);
    railRunL_ = railRunR_ = longestRun_ = 0;
    prevL_ = prevR_ = 0.0f;                              // fix-slice flat-run scratch (mirror reset())
    railSamplesL_.store (0); railSamplesR_.store (0); longestRunA_.store (0);
    clipConfirmed_.store (false);
    driftRun.store (0);
    // Deliberately NOT cleared: model_/capacity_/nominal_ (config), inLm/inRm/outM + cL/cR/cO (live
    // meters), reachedGood_ (monotonic per run), blockCount (grace window), the FIFO counters
    // (xrunsA/droppedA), AND OsResampled (per-run guidance, preserved above).
}
```

- [ ] Run to pass: `./tools/dev.cmd ctest --test-dir build -R "resetMeasurementLatches|blockPeak" --output-on-failure`
- [ ] Run the full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
- [ ] Commit: `HealthMonitor: blockPeak + resetMeasurementLatches (preserve OsResampled + config + telemetry) (D5)` with the trailer.

---

### Task 3: Wire `MeasurementSession` into `AudioEngine` and scope latches to the sweep

Feed the session the CURRENT block's peak (via `HealthMonitor::blockPeak`) BEFORE `analyzeInputBlock`; on the sweep-onset edge call `hm.resetMeasurementLatches()` (clearing pre-sweep latches) THEN analyze the current block (re-latching any clip ON the onset block). Mark the session Invalid in-measurement when `cleanCapture()` is false. Snapshot the phase into `Health.session`. `configure()` + `reset()` the session at run start.

**CRITICAL — do NOT revert the merged NaN fix.** The current capture callback and `processCaptureBlockForTest` use `if (graph.process(...)) hm.reportNonFinite();` (process returns bool, sanitizes input+output internally) and have NO `scanAndFlagNonFinite(mono)`/`clear`. INSERT the session lines around that body; do not replace it.

**Files:** `src/audio/AudioEngine.h`, `src/audio/AudioEngine.cpp`, `tests/test_audioengine.cpp`

**Interfaces:**
- `SessionPhase AudioEngine::sessionPhase() const noexcept` ; `bool AudioEngine::sweepActive() const noexcept` (for D6) ; `Health.session` populated in `health()`.

Steps:

- [ ] Write the failing seam tests, appended to `tests/test_audioengine.cpp` (a clip block is loud, so `kArmSustainBlocks` clip blocks arm on the 3rd; the onset block's clip is retained, ramp blocks before arming are re-scoped away):

```cpp
TEST_CASE("AudioEngine seam: session starts Idle and arms after a sustained loud run") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::Idle);
    std::vector<float> loud (8, 0.5f), sil (8, 0.0f), mono (8, 0.0f);
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i)
        e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    CHECK (e.cleanCapture());            // a clean loud run stays valid
}

TEST_CASE("AudioEngine seam: a clip ON the sweep-onset block invalidates") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    // Two clean-loud ramp blocks, then a clip block that COMPLETES the arm: the onset block's clip is
    // analyzed AFTER the latch re-scope, so it stands.
    std::vector<float> loud (8, 0.5f), sil (8, 0.0f), mono (8, 0.0f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };
    e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
    e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
    e.processCaptureBlockForTest (clip.data(), sil.data(), mono.data(), (int) clip.size());  // arms + clips
    CHECK (e.sessionPhase() == eb::SessionPhase::Invalid);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}
```

- [ ] Add the session member + accessors to `src/audio/AudioEngine.h`. Add the include with the other audio includes:

```cpp
#include "audio/MeasurementSession.h"
```

  Add the public accessors near `cleanCapture()`/`healthFlags()`:

```cpp
    // D5: the measurement-session phase (Idle/Preflight/SweepActive/Complete/Invalid). The GUI gates
    // its clean/invalid wording on this so pre-/post-sweep room events aren't scored as the sweep.
    SessionPhase sessionPhase() const noexcept { return session_.phase(); }
    bool sweepActive()         const noexcept { return session_.sweepActive(); }   // D6 consumer
```

  Add the member alongside `HealthMonitor hm;`:

```cpp
    MeasurementSession session_;   // D5: re-armable level-threshold sweep-window state machine
```

- [ ] Wire the capture callback in `src/audio/AudioEngine.cpp`. INSERT the four session lines into the EXISTING body (current `CaptureCallback::audioDeviceIOCallbackWithContext`, AudioEngine.cpp ~31-48) — keep the null guard, the bool `graph.process`, and the dropped-frames delta block exactly as they are:

```cpp
        if (numIn < 2 || (int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        const float* l = in[0]; const float* r = in[1];
        if (l == nullptr || r == nullptr) { e.hm.reportXrun(); return; }   // KEEP: per-pointer guard

        // D5: feed the session the CURRENT block's peak; on the genuine sweep onset re-scope validity
        // BEFORE analyzing this block (so a pre-sweep room event is dropped and a clip ON the onset
        // block is re-detected fresh by analyzeInputBlock below).
        const float pk = eb::HealthMonitor::blockPeak (l, r, numSamples);
        e.session_.observeBlockPeak (pk);
        if (e.session_.consumeSweepStarted()) e.hm.resetMeasurementLatches();

        e.hm.analyzeInputBlock (l, r, numSamples);             // KEEP: detection (raw input)
        // In-measurement only: an invalidating flag (clip/NaN/dropout/drift) latches the session Invalid
        // so the GUI's phase-gated wording matches cleanCapture(). Single-writer: the capture thread is
        // the only writer of session phase_; a render-side dropout sets cleanCapture=false and is
        // re-derived here on the next capture block.
        if (e.session_.inMeasurement() && ! e.hm.cleanCapture()) e.session_.markInvalid();

        if (e.graph.process (l, r, mono.data(), numSamples))   // KEEP: bool return, sanitizes in+out
            e.hm.reportNonFinite();
        e.bridge.pushCapture (mono.data(), numSamples);
        // KEEP UNCHANGED below: the droppedCaptureFrames delta block.
```

  Do NOT touch `RenderCallback` — the render side keeps its existing `observeRenderBlock(...)` calls and does NOT call `markInvalid()` (single-writer model; a render dropout is re-derived on the next capture block).

- [ ] Snapshot the phase in `AudioEngine::health()` and reset+configure the session at run start. In `AudioEngine::health()` (after `auto h = hm.snapshot();`):

```cpp
    h.session = session_.phase();   // D5: surface the measurement-session phase to the GUI
```

  In `start()`, next to the existing `hm.prepare(...)` / `session`-adjacent reset (the same spot where `hm.prepare` and `bridge.reset()` run; `cap`/`capRate` are in scope there):

```cpp
    session_.configure (cap, capRate);   // D5: size the terminal-silence window for this block/rate
    session_.reset();                     // fresh measurement session each run (Idle -> Preflight -> ...)
```

  In `prepareForTest(...)`, configure + reset so the seam mirrors a run:

```cpp
    session_.configure (block, sampleRate);
    session_.reset();
```

  And make the seam drive the session, mirroring the capture-callback ordering. Update `processCaptureBlockForTest(...)` (current form is `hm.analyzeInputBlock(...); if (graph.process(...)) hm.reportNonFinite();`):

```cpp
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                              float* outMono, int numSamples) {
    const float pk = eb::HealthMonitor::blockPeak (inL, inR, numSamples);
    session_.observeBlockPeak (pk);
    if (session_.consumeSweepStarted()) hm.resetMeasurementLatches();
    hm.analyzeInputBlock (inL, inR, numSamples);
    if (session_.inMeasurement() && ! hm.cleanCapture()) session_.markInvalid();
    if (graph.process (inL, inR, outMono, numSamples)) hm.reportNonFinite();
}
```

- [ ] Build: `./tools/dev.cmd cmake --build build --target eb_tests`
- [ ] Run the seam tests: `./tools/dev.cmd ctest --test-dir build -R "AudioEngine seam" --output-on-failure`
- [ ] Run the full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
- [ ] Commit: `AudioEngine: drive MeasurementSession + re-scope clip/integrity latches to the sweep (D5)` with the trailer.

---

### Task 4: GUI status line gated on the session phase

Gate the Running-branch verdict on the session phase: before the sweep, a neutral "waiting" line; in-sweep, the existing clean/invalid/output-clip/low-level verdict; at Complete, an honest sweep-scoped all-clear (with the OS-resampled caveat when applicable). The device-loss/Error path and the cleanCapture-first ordering stay.

**Files:** `src/gui/MainComponent.cpp`

**Anchor:** the REAL current Running chain in `updateStatusLine()` has FOUR else-if rungs after the `!h.cleanCapture` rung, in order: (1) `!h.cleanCapture` -> `invalidMeasurementMessage(h.flags)` (danger), (2) `any(h.flags & HealthFlag::ClipOutput)` (warn), (3) `silentTicks_ >= kSilentHoldTicks` -> "no input signal" (warn), (4) `lowLevelTicks_ >= kLowLevelHoldTicks` -> "level low" (warn), else "Running - clean" (ok). Read the file and quote that exact block as the replace target. `invalidMeasurementMessage(h.flags)` returns `juce::String` and `Label::setText` takes `juce::String`, so the existing call is correct.

Steps:

- [ ] In `src/gui/MainComponent.cpp`, replace the Running chain with the phase-gated version (insert the Idle/Preflight rung ABOVE the ClipOutput/silent/low-level warnings — those are suppressed before the sweep arms, intended — and add the Complete rung):

```cpp
    if (st == EngineStatus::Running) {
        const auto h = engine.health();
        // An invalidating condition is reported the instant it latches, regardless of phase.
        if (! h.cleanCapture) {
            statusLine.setText (eb::invalidMeasurementMessage (h.flags), juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::danger());
        } else if (h.session == eb::SessionPhase::Idle || h.session == eb::SessionPhase::Preflight) {
            // Before the sweep: validity isn't scoped to anything yet, so don't claim "clean".
            statusLine.setText ("Running - waiting for the Dirac sweep...", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
        } else if (any (h.flags & HealthFlag::ClipOutput)) {
            statusLine.setText ("Output clipping - lower the level or avoid Sum", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else if (silentTicks_ >= kSilentHoldTicks) {
            statusLine.setText ("Running - no input signal (check the EARS)", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else if (lowLevelTicks_ >= kLowLevelHoldTicks) {
            statusLine.setText ("Running - level low: turn your amp up to the green band", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
        } else if (h.session == eb::SessionPhase::Complete) {
            juce::String msg = "Sweep captured - no clipping or dropouts detected";
            if (any (h.flags & HealthFlag::OsResampled)) msg += " (OS-resampled - approximate)";
            statusLine.setText (msg, juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::ok());
        } else {
            statusLine.setText ("Capturing the Dirac sweep - clean so far", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::ok());
        }
    } else if (st == EngineStatus::Error) {
```

  (Match the real surrounding code: keep the exact `Theme::` accessor names, the `any(...)`/`HealthFlag` qualification, and the `silentTicks_`/`lowLevelTicks_`/`kSilentHoldTicks`/`kLowLevelHoldTicks` names the file already uses. Adjust only if the real names differ.)

- [ ] Build (the test target links `eb_gui`): `./tools/dev.cmd cmake --build build --target eb_tests`
- [ ] Run the full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
- [ ] Commit: `MainComponent: gate the status line on the measurement-session phase (D5)` with the trailer.

---

### Task 5: Edge tests — pre/post-sweep events excluded, two-sweep scoping correct

Focused seam tests proving the scoping end-to-end (the load-bearing D5 guarantee), including the two-sweep re-arm.

**Files:** `tests/test_audioengine.cpp`

Steps:

- [ ] Append the scoping tests to `tests/test_audioengine.cpp`:

```cpp
TEST_CASE("AudioEngine seam: a clip DURING the sweep invalidates and latches the session Invalid") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    auto run = [&] (std::vector<float> l) {
        std::vector<float> r (l.size(), 0.0f), mono (l.size(), 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), (int) l.size());
    };
    std::vector<float> loud (8, 0.5f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (loud);  // arm
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    CHECK (e.cleanCapture());

    run (clip);                                                                      // mid-sweep clip
    CHECK_FALSE (e.cleanCapture());
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.health().session == eb::SessionPhase::Invalid);
}

TEST_CASE("AudioEngine seam: a clean sweep then sustained silence reaches Complete and stays clean") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    auto run = [&] (float v) {
        std::vector<float> l (8, v), r (8, 0.0f), mono (8, 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), 8);
    };
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (0.5f);   // arm
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    // prepareForTest configured the silence window for block=8 @ 48k (a large block count); drive enough
    // quiet blocks to cross it. Use the session's configured need via kSilenceCompleteSeconds.
    const int need = (int) std::lround (eb::MeasurementSession::kSilenceCompleteSeconds * 48000.0 / 8.0) + 2;
    for (int i = 0; i < need; ++i) run (0.0f);
    CHECK (e.sessionPhase() == eb::SessionPhase::Complete);
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine seam: the RIGHT-earcup sweep after a gap is still scored (no false-clean)") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    auto runL = [&] (std::vector<float> l) {
        std::vector<float> r (l.size(), 0.0f), mono (l.size(), 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), (int) l.size());
    };
    std::vector<float> loud (8, 0.5f), quiet (8, 0.0f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) runL (loud);  // LEFT sweep
    const int need = (int) std::lround (eb::MeasurementSession::kSilenceCompleteSeconds * 48000.0 / 8.0) + 2;
    for (int i = 0; i < need; ++i) runL (quiet);                                      // long inter-sweep gap
    CHECK (e.sessionPhase() == eb::SessionPhase::Complete);                           // provisional
    CHECK (e.cleanCapture());

    // RIGHT earcup sweep arms again; a clip on it must STILL invalidate (the bug D5 must not have).
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks - 1; ++i) runL (loud);
    runL (clip);
    CHECK_FALSE (e.cleanCapture());
    CHECK (e.health().session == eb::SessionPhase::Invalid);
}
```
(Ensure `tests/test_audioengine.cpp` includes `<cmath>` for `std::lround`.)

- [ ] Build: `./tools/dev.cmd cmake --build build --target eb_tests`
- [ ] Run the scoping tests: `./tools/dev.cmd ctest --test-dir build -R "AudioEngine seam" --output-on-failure`
- [ ] Run the full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
- [ ] Commit: `Tests: pre/post-sweep clip scoping + two-sweep re-arm (D5)` with the trailer.

---

## Final verification (run before finishing)

- [ ] Full suite green: `./tools/dev.cmd ctest --test-dir build --output-on-failure`.
- [ ] App builds: `./tools/dev.cmd cmake --build build --target EarsBridge`.
- [ ] **On-device ratification (not headless-testable, like the D2 smoke):** run a real Dirac measurement through the EARS and confirm (a) the status shows "waiting for the Dirac sweep" before Dirac starts, "Capturing the Dirac sweep" during, and "Sweep captured" after BOTH earcups; (b) a deliberately hot level shows the invalid clip message; (c) the session does NOT prematurely show Complete between the left and right sweeps (if it does, raise `kSilenceCompleteSeconds`); (d) a correctly-leveled sweep actually arms (if it sits on "waiting", lower `kSweepStartLinear`). Note the observed inter-sweep gap + sweep peak so the two constants can be ratified.

Then use **superpowers:finishing-a-development-branch**.

## Self-review

**Spec coverage (audit D5 / R18, §7 step 3, §10).** `MeasurementSession{Idle,Preflight,SweepActive,Complete,Invalid}` (Task 1); latch re-scope on the genuine sweep onset via `resetMeasurementLatches()` (Task 2) called on `consumeSweepStarted()` (Task 3); pre-sweep events cleared on the edge, in-sweep events latch `Invalid`, post-signal silence -> Complete; **the two-sweep Dirac reality is handled** (re-armable Complete keeps both earcups in one validity window — Task 1 + Task 5 prove the right earcup still scores). Level-threshold boundary with a sustained-arm gate; the "Arm measurement" button alternative remains addable. GUI stops claiming "clean" before the sweep (Task 4).

**Integration with merged work.** The capture callback + seam keep the fix-slice NaN form (`if (graph.process(...)) reportNonFinite()`, no `scanAndFlagNonFinite`); `resetMeasurementLatches` preserves D2's `OsResampled` and resets the fix-slice `prevL_/prevR_`; Task 4 anchors the real 4-rung `updateStatusLine` and uses the `juce::String` `invalidMeasurementMessage`.

**Placeholder scan.** No TBD / "add error handling" / "similar to Task N". Every task carries real enums, signatures, bodies, and tests grounded in current `main`.

**Type consistency.** `SessionPhase` in `EngineTypes.h`; `MeasurementSession` round-trips its phase through `std::atomic<int>` + `static_cast<SessionPhase>` like `AudioEngine::engineStatus`/`LrVerify`; thresholds are `float` linear constants consistent with `HealthMonitor::kClipLinear`; the silence floor matches the -50 dBFS level floor by intent.

**RT-safety.** `MeasurementSession` uses only `std::atomic<int>/<bool>` + plain capture-thread scratch, all `noexcept`, no alloc/lock/IO. `blockPeak` is a plain loop. The capture callback gains only atomic ops + a branch. `phase_` is single-writer (capture thread); the GUI reads it via `phase()`. The render callback is unchanged (no `markInvalid`), so there is no two-writer race.
