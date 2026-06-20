# Frozen SRC Ratio During the Sweep — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Stop the `ClockBridge` PI fill-controller from continuously re-publishing the async-SRC ratio during a Dirac sweep. Estimate a stable ratio just before the sweep, **freeze** it for the sweep's duration (absorbing short-term clock drift with the existing FIFO headroom rather than steering), recenter only in silence / between sweeps, and **invalidate** the measurement if an emergency drop/insert or a large correction is forced mid-sweep. This closes audit finding **D6** / requirement **R19**.

**Architecture:** Extend the existing lock-free `ClockBridge` (no new threads, no new libraries, no change to the FIR/combine/clock design — audit §7). Add a freeze mode driven by one `std::atomic<bool>`: on entry, snapshot the current converged trimmed ratio and hold it; while frozen, `pullRender` keeps `publishedRatio` constant and does **not** advance the PI integrator; if the FIFO crosses a hard near-empty / near-full bound while frozen (drift outran the headroom), latch an *emergency-correction* event the engine can read. `AudioEngine` gains a `setSweepActive(bool)` seam that fans the freeze out to the bridge and, on each render block during a sweep, forwards a forced emergency correction into `HealthMonitor` as a new invalidating `SweepRetimed` flag (kept distinct from the dropout class per the audit).

**Tech Stack:** C++20, JUCE 8.0.4 (`juce::AbstractFifo` + `juce::LagrangeInterpolator`), Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

**Depends on D5 (sweep-session state machine).** The audit's D5 plan introduces a `MeasurementSession{Idle,Preflight,SweepActive,Complete,Invalid}` state machine but it is **not yet in the tree** (verified: no `SweepActive` / `MeasurementSession` symbol exists anywhere in `src/`). This plan therefore **defines the freeze API as the seam D5 will drive** — `ClockBridge::setSweepActive(bool)` and `AudioEngine::setSweepActive(bool)` — and exercises it directly in tests by toggling that boolean. When D5 lands, its state machine calls `AudioEngine::setSweepActive(true)` on `SweepActive` entry and `false` on exit; no change to the bridge is needed. Until then the seam is callable but no production caller toggles it (so behavior is byte-for-byte unchanged for shipped runs, which is intentional — D6 is inert until D5 wires the boundary).

This plan implements audit finding **D6** and requirement **R19** from `docs/EARS_DIRAC_CLIPPING_AUDIT.md` (§4 D6, §7 step 3, §8 phase 3, §9 clock/transport). It builds on the already-merged D1/D3/D4 slice (`HealthMonitor::analyzeInputBlock` / `scanAndFlagNonFinite`, `HealthFlag::ClipConfirmed`/`NonFinite`, `src/gui/ClipStatus.h`). **Out of scope** (named follow-ups): D5 session state machine + the sweep boundary itself (session plan), D2 native-rate pin, D8 per-block format revalidation, the golden sample-accurate cross-domain transport soak test (a later integration-test plan; this plan asserts ratio constancy + emergency-correction invalidation at the unit level).

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests` — run `tools/dev.cmd` DIRECTLY from Bash, never bare `cmake` (it loses the MSVC env) and never `cmd /c`; it wraps Ninja + MSVC.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + `std::atomic` only.
- HealthFlag (`src/audio/EngineTypes.h`) is NOT persisted — append enum values freely. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

## Design decision

**Freeze = hold the last converged trimmed ratio, stop integrating, absorb drift with FIFO headroom; latch an emergency-correction event only when the FIFO crosses a hard bound.** (Audit §7 step 3: "freeze the ClockBridge ratio for its duration … recenter only in silence … invalidate if an emergency correction is forced mid-sweep.")

The bridge already converges `ratioTrim` to equilibrium via its PI loop (`ClockBridge.cpp:69-80`) and primes to the `kTargetFill = 0.5` setpoint before the run (`AudioEngine.cpp:303`). So the pre-sweep estimate is simply the value `ratioTrim` already holds at the moment the sweep is armed — no separate estimation pass is needed; we snapshot it. While frozen we keep emitting that exact `ratio` every block (constant `publishedRatio`, so `HealthMonitor`'s drift state machine at `HealthMonitor.cpp:175-184` sees a constant ratio and cannot accumulate the sub-0.5% creep that D6 describes), and we **do not** touch `integ` / `ratioTrim` / `smoothedFill`'s steering — only the `smoothedFill` *observation* is kept current (read-only telemetry) so `fifoFill()` stays honest.

*Alternative considered and rejected:* re-running a fresh dedicated ratio-estimation window at freeze time (average fill slope over N blocks). Rejected because the PI loop has already converged by the time a sweep is armed (the run has been streaming through preflight), so a second estimator adds RT-path state and a warm-up window for no accuracy gain; snapshotting the converged trim is both simpler and exactly the equilibrium value.

*Emergency-correction definition:* while frozen, a held ratio means the FIFO fill drifts slowly. If it reaches a hard near-empty bound (`fill < kFreezeFloor`) the interpolator would starve, or a hard near-full bound (`fill > kFreezeCeil`) the producer would overrun — either forces a real sample drop/insert that nonuniformly retimes the sweep. We detect that bound crossing in `pullRender` (frozen branch only) and set an atomic `emergencyCorrection_`; the engine drains it per block and invalidates. We do **not** auto-unfreeze or auto-steer on the crossing — the measurement is already ruined, so the honest action is to flag it, not to hide it by steering (which is exactly the creep D6 forbids).

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/ClockBridge.h` | bridge surface + freeze state | declare `setSweepActive(bool)`, `sweepActive()`, `consumeEmergencyCorrection()`, freeze constants + atomics/scratch |
| `src/audio/ClockBridge.cpp` | PI loop + SRC pull | freeze branch in `pullRender`; snapshot trim on entry; reset freeze state in `reset()`/`prepare()` |
| `src/audio/EngineTypes.h` | `HealthFlag` enum | add invalidating `SweepRetimed` |
| `src/audio/HealthMonitor.cpp` | invalidating-flag mask + message | add `SweepRetimed` to the `raise()` invalidating set; add `reportSweepRetimed()` |
| `src/audio/HealthMonitor.h` | telemetry surface | declare `reportSweepRetimed()` |
| `src/audio/AudioEngine.h` | engine surface + sweep seam | declare `setSweepActive(bool)`, `sweepActive()` |
| `src/audio/AudioEngine.cpp` | render callback + lifecycle | implement `setSweepActive`; drain `consumeEmergencyCorrection()` in the render callback during a sweep; clear sweep state in `start()`/`stop()` |
| `src/gui/ClipStatus.h` | flag→message helper | add the `SweepRetimed` branch |
| `tests/test_clockbridge.cpp` | bridge unit tests | freeze-holds-ratio, freeze-absorbs-small-drift, freeze-emergency-on-large-drift, unfreeze-recenters |
| `tests/test_healthmonitor.cpp` | monitor unit tests | `reportSweepRetimed` invalidates + flags |
| `tests/test_clipstatus.cpp` | message helper tests | `SweepRetimed` message |

---

## Task 1: Freeze API on ClockBridge — hold the converged ratio during a sweep

**Files:**
- Modify: `src/audio/ClockBridge.h:13-61` (declare API + constants + state)
- Modify: `src/audio/ClockBridge.cpp:6-31` (`prepare`/`reset` clear freeze state), `:68-103` (`pullRender` freeze branch)
- Test: `tests/test_clockbridge.cpp`

**Interfaces:**
- Consumes: existing `ClockBridge::pullRender(float*, int)` (consumer thread), `currentRatio()`, `fifoFill()`, the PI state `ratioTrim`/`integ`/`smoothedFill` (consumer-thread-only).
- Produces:
  - `void ClockBridge::setSweepActive(bool active) noexcept` — flips the freeze flag (any thread; the consumer reads it). `true` arms the freeze (the *next* `pullRender` snapshots the converged trim); `false` resumes PI steering from the held trim.
  - `bool ClockBridge::sweepActive() const noexcept` — current freeze state.
  - `bool ClockBridge::consumeEmergencyCorrection() noexcept` — read-and-clear edge: true once if a hard FIFO-bound crossing forced a retiming correction while frozen.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_clockbridge.cpp` (the `runBridge` harness at `:26-85` already drives push/pull at a chosen feed ratio; these new tests drive the bridge directly so they can toggle the freeze mid-run):

```cpp
TEST_CASE("ClockBridge: frozen ratio is held constant during a sweep") {
    // With the PI loop free-running the ratio wanders every block (the D6 creep). Once the sweep is
    // armed the published ratio must stop moving: snapshot the converged trim and hold it.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();

    std::vector<float> blk (256, 0.25f), out (256, 0.0f);
    // Run a few blocks free so the PI loop is at equilibrium, then arm the sweep.
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256);   // first frozen block snapshots
    const double frozen = cb.currentRatio();
    REQUIRE (cb.sweepActive());

    // Over many balanced blocks the held ratio must not move at all (bit-exact constant).
    for (int i = 0; i < 200; ++i) {
        cb.pushCapture (blk.data(), 256);
        cb.pullRender (out.data(), 256);
        CHECK (cb.currentRatio() == frozen);                 // ratio frozen: zero creep
    }
    CHECK_FALSE (cb.consumeEmergencyCorrection());           // balanced feed => no forced correction
}

TEST_CASE("ClockBridge: freeze absorbs small drift with FIFO headroom (no emergency, no creep)") {
    // Fed ~0.05% fast: with the loop frozen the FIFO slowly fills, but stays within the freeze band,
    // so it is absorbed by headroom -- ratio stays constant and NO emergency correction is raised.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.1f), out (256, 0.0f);
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);

    const double feed = 48024.0 / 48000.0;   // +0.05% fast producer
    double credit = 0.0; double held = 0.0;
    for (int b = 0; b < 300; ++b) {
        credit += 256.0 * feed;
        while (credit >= 256.0) { cb.pushCapture (blk.data(), 256); credit -= 256.0; }
        cb.pullRender (out.data(), 256);
        if (b == 0) held = cb.currentRatio();
        CHECK (cb.currentRatio() == held);                   // still frozen across the whole sweep
    }
    CHECK_FALSE (cb.consumeEmergencyCorrection());           // small drift absorbed by headroom
}

TEST_CASE("ClockBridge: freeze raises an emergency correction when drift outruns the headroom") {
    // Starve the consumer hard while frozen: pull without feeding so the FIFO drains past the freeze
    // floor. The bridge cannot steer (it is frozen), so it must flag the forced retiming, not hide it.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.1f), out (256, 0.0f);
    for (int i = 0; i < 4; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);

    bool raised = false;
    for (int i = 0; i < 40 && ! raised; ++i) {               // pull with no new capture -> FIFO drains
        cb.pullRender (out.data(), 256);
        raised = cb.consumeEmergencyCorrection();
    }
    CHECK (raised);                                          // hard bound crossing forced a correction
}

TEST_CASE("ClockBridge: unfreezing resumes PI steering and recenters") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.1f), out (256, 0.0f);
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    const double frozen = cb.currentRatio();
    for (int i = 0; i < 20; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    CHECK (cb.currentRatio() == frozen);

    cb.setSweepActive (false);                              // recenter only in silence/between sweeps
    REQUIRE_FALSE (cb.sweepActive());
    // After unfreeze the loop is free again: push an imbalance and confirm the ratio is allowed to move.
    bool moved = false;
    for (int i = 0; i < 50; ++i) {
        cb.pushCapture (blk.data(), 256); cb.pushCapture (blk.data(), 256);   // overfeed -> fill rises
        cb.pullRender (out.data(), 256);
        if (cb.currentRatio() != frozen) { moved = true; break; }
    }
    CHECK (moved);                                          // steering resumed
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `setSweepActive`, `sweepActive`, `consumeEmergencyCorrection` are undeclared on `ClockBridge`.

- [ ] **Step 3: Declare the API + freeze constants + state in `ClockBridge.h`**

In `src/audio/ClockBridge.h`, add the public methods after `prime`/`primeToTarget` (after line 32) and before the telemetry getters:
```cpp
    // ---- D6: freeze the SRC ratio during a Dirac sweep --------------------------------------
    // The PI fill-controller normally re-trims the resample ratio every render block. During a
    // measurement sweep that continuous sub-0.5% creep nonuniformly retimes Dirac's log sweep
    // (smears the recovered IR) while staying under the drift latch -> reads "clean". So: while the
    // sweep is active we HOLD the converged trim constant (publishedRatio stops moving), absorbing
    // short-term clock drift with the existing FIFO headroom instead of steering, and recenter only
    // when the sweep ends. If drift outruns the headroom (FIFO crosses a hard near-empty/near-full
    // bound), a real retiming correction is forced; we latch that as an emergency-correction edge so
    // the engine can invalidate the measurement rather than silently steer through it.
    void setSweepActive (bool active) noexcept;   // any thread; consumer reads it
    bool sweepActive() const noexcept;
    // Read-and-clear: true once if a hard FIFO-bound crossing forced a correction while frozen.
    bool consumeEmergencyCorrection() noexcept;
```

Add the freeze constants in the public block (after `kTargetFill` at line 19):
```cpp
    // Hard FIFO-fill band tolerated WHILE FROZEN. Outside this band the held ratio can no longer hold
    // the FIFO and a real drop/insert is forced -> emergency correction. Wider than the PI band so the
    // freeze genuinely absorbs short-term drift with headroom; a crossing means the freeze failed.
    static constexpr double kFreezeFloor = 0.10;   // near-empty: interpolator about to starve
    static constexpr double kFreezeCeil  = 0.90;   // near-full: producer about to overrun
```

Add the freeze state in the `private` section (after `integ` at line 54, and after the existing atomics near line 60):
```cpp
    // D6 freeze state. sweepActive_ is cross-thread (toggled off the consumer thread). frozenRatio /
    // freezeArmed_ are CONSUMER-THREAD-ONLY scratch (written/read only inside pullRender).
    std::atomic<bool>   sweepActive_ { false };
    std::atomic<bool>   emergencyCorrection_ { false };   // edge, drained by consumeEmergencyCorrection()
    bool   freezeArmed_  = false;   // false until the first frozen block snapshots the trim
    double frozenRatio_  = 1.0;     // the held capture:render ratio while frozen
```

- [ ] **Step 4: Implement the freeze branch in `pullRender` + reset the state**

In `src/audio/ClockBridge.cpp`, replace the PI block at the top of `pullRender` (lines 68-80, from the `// --- PI fill-control` comment through `publishedRatio.store (ratio);`) with:
```cpp
int ClockBridge::pullRender (float* out, int numFrames) {
    // --- Fill observation (always current, even while frozen, so fifoFill() stays honest). ---
    const double fillFrac = (double) fifo.getNumReady() / (double) capacity;
    smoothedFill += 0.01 * (fillFrac - smoothedFill);        // 1-pole smoother
    publishedFill.store (smoothedFill);

    const double nominal = captureRate / renderRate;          // input samples per output sample
    double ratio;
    if (sweepActive_.load()) {
        // --- FROZEN: hold the converged trim; do NOT advance the PI integrator (no creep). ---
        if (! freezeArmed_) { frozenRatio_ = nominal * ratioTrim; freezeArmed_ = true; }  // snapshot once
        ratio = frozenRatio_;
        // Drift the held ratio can no longer absorb -> a real retiming correction is forced. Flag it
        // (latch the edge); do NOT steer (steering through it is exactly the creep D6 forbids).
        if (smoothedFill < kFreezeFloor || smoothedFill > kFreezeCeil)
            emergencyCorrection_.store (true);
    } else {
        // --- FREE: normal PI fill-control, steering fill toward kTargetFill. ---
        freezeArmed_ = false;                                 // re-arm for the next sweep
        const double errFill = smoothedFill - kTargetFill;
        integ = juce::jlimit (-0.02, 0.02, integ + 1.0e-4 * errFill);
        ratioTrim = juce::jlimit (0.97, 1.03, 1.0 + (2.0e-2 * errFill + integ));
        ratio = nominal * ratioTrim;
    }
    publishedRatio.store (ratio);                             // expose to currentRatio() (lock-free)
```

Leave the rest of `pullRender` (from `// Input samples the interpolator MAY need` at line 82 onward) unchanged — it already uses the local `ratio`.

In `prepare` (`ClockBridge.cpp:15-18`) and `reset` (`:27-30`), clear the freeze state alongside the existing PI reset. Add to **both** blocks, right after `smoothedFill = kTargetFill; ratioTrim = 1.0; integ = 0.0;`:
```cpp
    sweepActive_.store (false); emergencyCorrection_.store (false);
    freezeArmed_ = false; frozenRatio_ = captureRate / juce::jmax (1.0, renderRate);
```

Add the method bodies near the other getters (after `currentRatio()` at `ClockBridge.cpp:109`):
```cpp
void ClockBridge::setSweepActive (bool active) noexcept { sweepActive_.store (active); }
bool ClockBridge::sweepActive() const noexcept { return sweepActive_.load(); }
bool ClockBridge::consumeEmergencyCorrection() noexcept { return emergencyCorrection_.exchange (false); }
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Then: `./tools/dev.cmd ctest --test-dir build -R "ClockBridge" --output-on-failure`
Expected: all four new freeze tests pass, and the existing `ClockBridge` tests (equal-rates, 96k->48k, drift-absorbed, priming) still pass — they never call `setSweepActive`, so they exercise the unchanged FREE branch.

- [ ] **Step 6: Commit**

```
git add src/audio/ClockBridge.h src/audio/ClockBridge.cpp tests/test_clockbridge.cpp
git commit -m "ClockBridge: freeze the SRC ratio during a sweep (D6/R19)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `SweepRetimed` invalidating flag + HealthMonitor report path

**Files:**
- Modify: `src/audio/EngineTypes.h:15-26` (HealthFlag enum)
- Modify: `src/audio/HealthMonitor.h:21` area (declare `reportSweepRetimed`)
- Modify: `src/audio/HealthMonitor.cpp:8-20` (invalidating mask), add `reportSweepRetimed` body
- Test: `tests/test_healthmonitor.cpp`

**Interfaces:**
- Consumes: `ClockBridge::consumeEmergencyCorrection()` (Task 1), `HealthMonitor::raise(HealthFlag)`.
- Produces:
  - `HealthFlag::SweepRetimed = 1u << 9` — INVALIDATING; a forced mid-sweep SRC correction nonuniformly retimed the sweep. Kept distinct from `Dropout` per the audit ("keep the classes separate").
  - `void HealthMonitor::reportSweepRetimed() noexcept` — raises `SweepRetimed` (which clears `cleanCapture`). RT-safe (one `raise`).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("HealthMonitor: reportSweepRetimed invalidates and flags, distinct from Dropout") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    REQUIRE (h.cleanCapture());
    REQUIRE_FALSE (eb::any (h.flags() & eb::HealthFlag::SweepRetimed));

    h.reportSweepRetimed();

    CHECK_FALSE (h.cleanCapture());                                   // invalidating
    CHECK (eb::any (h.flags() & eb::HealthFlag::SweepRetimed));       // its own flag
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::Dropout));      // NOT folded into the dropout class
}
```

- [ ] **Step 2: Run the test to see it fail**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `HealthFlag::SweepRetimed` and `reportSweepRetimed` are undeclared.

- [ ] **Step 3: Add the enum value**

In `src/audio/EngineTypes.h`, append to the `HealthFlag` enum (currently ending at `NonFinite = 1u << 8`):
```cpp
    NonFinite     = 1u << 8, // INVALIDATING: a NaN/Inf sample reached the path
    SweepRetimed  = 1u << 9  // INVALIDATING: a forced mid-sweep SRC correction retimed the sweep
```

- [ ] **Step 4: Declare + implement the report method and extend the invalidating mask**

In `src/audio/HealthMonitor.h`, declare it next to `reportDroppedFrames` (after line 17):
```cpp
    void reportSweepRetimed();               // a forced mid-sweep SRC correction (D6): invalidating
```

In `src/audio/HealthMonitor.cpp`, add `SweepRetimed` to the invalidating mask in `raise()` (lines 11-17):
```cpp
    const unsigned invalidating =
        static_cast<unsigned> (HealthFlag::Xrun)          |
        static_cast<unsigned> (HealthFlag::Dropout)       |
        static_cast<unsigned> (HealthFlag::ExcessDrift)   |
        static_cast<unsigned> (HealthFlag::FifoStarved)   |
        static_cast<unsigned> (HealthFlag::ClipConfirmed) |
        static_cast<unsigned> (HealthFlag::NonFinite)     |
        static_cast<unsigned> (HealthFlag::SweepRetimed);
```

Add the body after `reportDroppedFrames` (after line 60):
```cpp
void HealthMonitor::reportSweepRetimed() {
    // A held (frozen) SRC ratio could no longer absorb the clock drift, so a real drop/insert was
    // forced mid-sweep -> the sweep was nonuniformly retimed and the measurement is invalid. Kept as
    // its own flag (NOT Dropout) so the GUI can name the cause honestly (see ClipStatus.h).
    raise (HealthFlag::SweepRetimed);
}
```

- [ ] **Step 5: Run the test to see it pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Then: `./tools/dev.cmd ctest --test-dir build -R "HealthMonitor" --output-on-failure`
Expected: the new test passes; existing HealthMonitor tests stay green (the mask only gained a flag; no existing flag's behavior changed).

- [ ] **Step 6: Commit**

```
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "HealthMonitor: SweepRetimed invalidating flag for forced mid-sweep correction (D6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Honest status message for `SweepRetimed`

**Files:**
- Modify: `src/gui/ClipStatus.h:8-14`
- Test: `tests/test_clipstatus.cpp`

**Interfaces:**
- Consumes: `HealthFlag` (now carrying `SweepRetimed`).
- Produces: `invalidMeasurementMessage(HealthFlag)` returns the `SweepRetimed` string when that bit is set. Ordered after `ClipConfirmed`/`NonFinite` (those name a more direct user-actionable cause) and before the generic dropout fallback.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_clipstatus.cpp` (matching the file's existing style — confirm the exact expected string matches Step 3):
```cpp
TEST_CASE("invalidMeasurementMessage: SweepRetimed names the clock-retiming cause") {
    using namespace eb;
    const char* msg = invalidMeasurementMessage (HealthFlag::SweepRetimed);
    CHECK (juce::String (msg).containsIgnoreCase ("retim"));   // names the clock-retiming cause
    // ClipConfirmed and NonFinite still take precedence when both are set.
    CHECK (juce::String (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::SweepRetimed))
               .containsIgnoreCase ("full scale"));
}
```

- [ ] **Step 2: Run the test to see it fail**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Then: `./tools/dev.cmd ctest --test-dir build -R "ClipStatus|clipstatus|invalidMeasurement" --output-on-failure`
Expected: FAIL — the `SweepRetimed` case currently falls through to the generic "Dropouts detected" string, so `containsIgnoreCase("retim")` is false.

- [ ] **Step 3: Add the branch**

In `src/gui/ClipStatus.h`, insert the `SweepRetimed` branch after the `NonFinite` branch and before the dropout fallback:
```cpp
[[nodiscard]] inline const char* invalidMeasurementMessage (HealthFlag flags) noexcept {
    if (any (flags & HealthFlag::ClipConfirmed))
        return "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.";
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    if (any (flags & HealthFlag::SweepRetimed))
        return "Clock drift retimed the sweep - this measurement is invalid. Repeat the measurement.";
    return "Dropouts detected - this measurement is invalid.";
}
```

- [ ] **Step 4: Run the test to see it pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Then: `./tools/dev.cmd ctest --test-dir build -R "ClipStatus|clipstatus|invalidMeasurement" --output-on-failure`
Expected: pass.

- [ ] **Step 5: Commit**

```
git add src/gui/ClipStatus.h tests/test_clipstatus.cpp
git commit -m "ClipStatus: honest message for a sweep retimed by clock drift (D6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: AudioEngine sweep seam — fan the freeze out + invalidate on forced correction

**Files:**
- Modify: `src/audio/AudioEngine.h:54-58` (declare `setSweepActive`/`sweepActive`)
- Modify: `src/audio/AudioEngine.cpp:63-90` (render callback drains the emergency edge), `:208-343` (`start`/`stop` clear sweep state), add method bodies
- Test: `tests/test_audioengine.cpp`

**Interfaces:**
- Consumes: `ClockBridge::setSweepActive(bool)`, `ClockBridge::consumeEmergencyCorrection()` (Task 1); `HealthMonitor::reportSweepRetimed()` (Task 2).
- Produces:
  - `void AudioEngine::setSweepActive(bool active) noexcept` — the seam D5's session machine will call on `SweepActive` entry/exit; fans straight to `bridge.setSweepActive`.
  - `bool AudioEngine::sweepActive() const noexcept` — current freeze state (for the GUI / D5).
  - Render-callback wiring: each block, if a forced emergency correction occurred while frozen, call `hm.reportSweepRetimed()` (invalidates). On the macOS aggregate path (`usingAggregate_`) the FIFO runs clock-locked 1:1 and the freeze is inert, so skip the drain there to avoid a spurious flag from the aggregate's own drift wobble.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_audioengine.cpp`. This uses the public engine surface only (no device I/O) — it asserts the seam exists and forwards to the bridge, which is what D5 will rely on:
```cpp
TEST_CASE("AudioEngine: setSweepActive fans the freeze out to the clock bridge") {
    eb::AudioEngine e;
    CHECK_FALSE (e.sweepActive());
    e.setSweepActive (true);
    CHECK (e.sweepActive());          // seam D5 drives on SweepActive entry
    e.setSweepActive (false);
    CHECK_FALSE (e.sweepActive());    // and clears on exit / between sweeps
}
```

(The render-callback invalidation path — emergency edge -> `reportSweepRetimed` -> `cleanCapture==false` — is unit-tested at the `ClockBridge` + `HealthMonitor` boundaries in Tasks 1-2; a full callback-level integration test belongs to the separate callback-harness plan, R22.)

- [ ] **Step 2: Run the test to see it fail**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `AudioEngine::setSweepActive` / `sweepActive` are undeclared.

- [ ] **Step 3: Declare the seam in `AudioEngine.h`**

In `src/audio/AudioEngine.h`, add after `EngineStatus status() const;` (line 54):
```cpp
    // D6 sweep seam (driven by D5's session state machine on SweepActive entry/exit). Freezes the
    // ClockBridge SRC ratio for the sweep's duration so continuous sub-0.5% ratio creep can't
    // nonuniformly retime Dirac's log sweep. Inert on the macOS aggregate path (clock-locked 1:1).
    void setSweepActive (bool active) noexcept;
    bool sweepActive() const noexcept;
```

- [ ] **Step 4: Implement the seam + drain the emergency edge in the render callback**

In `src/audio/AudioEngine.cpp`, add the method bodies near `status()` (after line 161):
```cpp
void AudioEngine::setSweepActive (bool active) noexcept { bridge.setSweepActive (active); }
bool AudioEngine::sweepActive() const noexcept { return bridge.sweepActive(); }
```

In the `RenderCallback::audioDeviceIOCallbackWithContext` body, after the `observeRenderBlock` call (currently lines 82-83, the `if (e.usingAggregate_) ... else ...` pair) and before the channel-duplication loop, add:
```cpp
        // D6: while a sweep is frozen the held SRC ratio absorbs short-term drift with FIFO headroom;
        // if drift outran the headroom the bridge was forced to drop/insert (nonuniform retiming) ->
        // invalidate this measurement. Drain the edge every block (read-and-clear). Skip on the
        // aggregate path: there the FIFO is clock-locked 1:1, the freeze is inert, and the aggregate's
        // own drift-correction wobble must not be mistaken for a forced sweep correction.
        if (! e.usingAggregate_ && e.bridge.consumeEmergencyCorrection())
            e.hm.reportSweepRetimed();
```

In `start()` and `stop()`, clear the sweep freeze so a fresh run never inherits a stale frozen ratio. `start()` already calls `bridge.reset()` at line 294 (which now clears `sweepActive_` per Task 1, Step 4), so add an explicit belt-and-braces clear right after the existing `bridge.primeToTarget();` at line 303:
```cpp
    bridge.setSweepActive (false);   // D6: a fresh run starts free-running; D5 arms the sweep later
```
`stop()` already calls `bridge.reset()` at line 342, which clears the freeze state — no change needed there.

- [ ] **Step 5: Run the test to see it pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Then: `./tools/dev.cmd ctest --test-dir build -R "AudioEngine" --output-on-failure`
Expected: the new seam test passes; existing AudioEngine tests stay green.

- [ ] **Step 6: Run the FULL suite + commit**

Run: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
Expected: all tests pass (98 prior + the new freeze/flag/message/seam tests).

```
git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "AudioEngine: sweep-freeze seam + invalidate on forced mid-sweep correction (D6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (audit D6 / R19, §7 step 3, §9 clock/transport):**
- *"Estimate a stable ratio before the sweep"* — Task 1 snapshots the already-converged trimmed ratio (`frozenRatio_ = nominal * ratioTrim`) on the first frozen block. The PI loop has converged by sweep time (post-prime, post-preflight streaming), so the snapshot **is** the equilibrium estimate (Design decision; alternative re-estimation pass named and rejected).
- *"FREEZE it during SweepActive, absorb short-term drift with FIFO headroom, do NOT keep steering"* — Task 1's frozen branch holds `frozenRatio_` constant, does not advance `integ`/`ratioTrim`, and keeps only the read-only `smoothedFill` observation current. `currentRatio()` is bit-exact constant across the sweep (test: "frozen ratio is held constant", "absorbs small drift"). This removes the continuous re-publish at `ClockBridge.cpp:71-80` that D6 cites, so `HealthMonitor`'s drift state machine (`HealthMonitor.cpp:175-184`) sees a constant ratio and the sub-0.5% creep cannot accumulate.
- *"recenter only in silence / between sweeps"* — Task 1's FREE branch resumes PI steering on `setSweepActive(false)` and re-arms (`freezeArmed_ = false`) for the next sweep (test: "unfreezing resumes PI steering and recenters").
- *"INVALIDATE the measurement if an emergency drop/insert/large correction is forced mid-sweep"* — Task 1 latches `emergencyCorrection_` on a hard FIFO-bound crossing while frozen (`kFreezeFloor`/`kFreezeCeil`); Task 4's render callback drains it and calls `HealthMonitor::reportSweepRetimed()` (Task 2), which clears `cleanCapture` (test: "emergency correction when drift outruns the headroom"; "reportSweepRetimed invalidates"). Kept a **distinct** `SweepRetimed` flag, not folded into `Dropout`, per the audit; honest GUI string in Task 3.
- *D5 dependency* — stated up front; the freeze API (`AudioEngine::setSweepActive`) is the seam D5 will drive; verified `SweepActive`/`MeasurementSession` do not yet exist in `src/`, so D6 ships inert (no production toggle) until D5 wires the boundary — behavior for current shipped runs is unchanged.

**Type consistency:** `setSweepActive(bool)` / `sweepActive()` / `consumeEmergencyCorrection()` signatures match across `ClockBridge` → `AudioEngine`; `HealthFlag::SweepRetimed = 1u << 9` appends after `NonFinite = 1u << 8` (HealthFlag is non-persisted — append is allowed; `CombineMode` untouched); `reportSweepRetimed()` follows the existing `reportXrun()`/`reportDroppedFrames()` void-no-arg style and routes through `raise()` exactly like the other invalidating flags; `kFreezeFloor`/`kFreezeCeil` are `static constexpr double` like `kTargetFill`.

**RT-safety:** the frozen branch in `pullRender` is plain locals + `std::atomic` loads/stores (`sweepActive_.load`, `emergencyCorrection_.store`) — no alloc/lock/syscall/log/exception; `freezeArmed_`/`frozenRatio_` are consumer-thread-only scratch (written and read solely inside `pullRender`, mirroring the existing `smoothedFill`/`ratioTrim`/`integ` pattern). `setSweepActive`/`consumeEmergencyCorrection` are single atomic ops. The render-callback drain is one atomic exchange + (rarely) one `raise()` (atomic `fetch_or` + `store`), all RT-safe and matching the existing `observeRenderBlock` wiring.

**Placeholder scan:** no "TBD", no "add error handling", no "handle edge cases", no "similar to Task N" — every test and every implementation edit is shown as concrete code grounded in the real files (`ClockBridge.{h,cpp}`, `HealthMonitor.{h,cpp}`, `EngineTypes.h`, `AudioEngine.{h,cpp}`, `ClipStatus.h`) with file:line anchors. Each task is TDD: failing test → run-to-fail → minimal implementation → run-to-pass → commit.
