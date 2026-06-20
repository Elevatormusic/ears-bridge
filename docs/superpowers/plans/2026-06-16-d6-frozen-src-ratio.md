# Frozen SRC Ratio During the Sweep — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Stop the `ClockBridge` PI fill-controller from continuously re-publishing the async-SRC ratio during a Dirac sweep. Snapshot the converged ratio at sweep onset, **freeze** it for the sweep's duration (absorbing short-term clock drift with the existing FIFO headroom instead of steering), recenter between sweeps / in silence, and **invalidate** the measurement if drift outruns the headroom and forces a real retiming mid-sweep. This closes audit finding **D6** / requirement **R19**.

**Architecture:** Extend the lock-free `ClockBridge` (no new threads/libraries, no FIR/combine/clock-design change — audit §7). A freeze mode driven by one `std::atomic<bool>`: while frozen, `pullRender` holds the snapshotted ratio and does **not** advance the PI integrator; if the **raw** FIFO fill crosses a hard near-empty/near-full bound (drift outran the headroom) it latches an *emergency-correction* edge. **The freeze is driven live by the merged D5 session** — the capture callback calls `bridge.setSweepActive(session_.sweepActive())` every block, so the freeze engages on `SweepActive` and releases during the inter-sweep `Complete` gap (the right earcup re-snapshots a fresh converged trim). The render callback drains the emergency edge and raises a new invalidating `SweepRetimed` flag.

**Tech Stack:** C++20, JUCE 8.0.4 (`juce::AbstractFifo` + `juce::LagrangeInterpolator`), Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

## Pre-build review applied (why this plan differs from the original D6 draft)

This plan was reviewed by a 4-angle workflow against CURRENT `main` (the clipping-review-fixes + D2 + **D5** slices all merged) before writing. The original D6 draft was authored when D5 did **not** exist and shipped an *inert* seam; that is now wrong. Baked-in corrections (do NOT undo):

1. **`SweepRetimed = 1u << 10`, NOT `1u << 9`.** D2's `OsResampled = 1u << 9` is the current highest `HealthFlag` bit. `1u<<9` would alias the two flags (an OS-resampled run would falsely invalidate; the latch-preserve mask in `resetMeasurementLatches` would keep a stale `SweepRetimed`).
2. **`invalidMeasurementMessage` is `juce::String` with four branches** (ClipConfirmed +OsResampled-caveat → NonFinite → ExcessDrift → Dropout). INSERT a `SweepRetimed` branch (after NonFinite, before ExcessDrift); do NOT revert to the old `const char*` three-branch form.
3. **D6 is LIVE, wired to D5 — not an inert seam.** `AudioEngine::sweepActive()` already exists (D5, `AudioEngine.h:70`, tagged "D6 consumer"). Do NOT redeclare it or add `AudioEngine::setSweepActive`. Drive the bridge with one capture-path line `bridge.setSweepActive(session_.sweepActive())`. Keep the NaN-safe capture body (`if (graph.process(...)) reportNonFinite()`, no `scanAndFlagNonFinite`).
4. **Emergency bound on the RAW fill, not `smoothedFill`.** The 1-pole `smoothedFill` (0.01 coeff, ~100-block time constant) lags far behind a draining FIFO — it would flag the imminent underrun tens of blocks too late (and fail the 40-block starve test). The hard near-empty/near-full check is an imminence detector → use the raw `fillFrac`.
5. **The freeze branch REUSES the existing `nominal` local** in `pullRender` (don't re-declare it — the replaced span already contains its declaration).

**On-device ratification (R22 / soak, not headless-testable):** confirm a real Dirac sweep's drift stays inside the freeze band (so the emergency only fires on a genuine fault), and that the inter-sweep gap is long enough for the PI to reconverge before the right earcup re-snapshots (the D5 smoke already measures this gap — if it's materially < the ~1.5 s `kSilenceCompleteSeconds`, consider a single-snapshot-across-both-sweeps variant). Also confirm the run halts/re-prompts on `SweepRetimed`.

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests` — run `tools/dev.cmd` DIRECTLY from Bash, never bare `cmake` (it loses the MSVC env) and never `cmd /c`; it wraps Ninja + MSVC.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; **125 tests today**). Note: `dev.cmd` may eat `|` in a `-R` alternation — run each name substring separately if so.
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + `std::atomic` only.
- HealthFlag (`src/audio/EngineTypes.h`) is NOT persisted — append enum values. Current highest bit is `OsResampled = 1u << 9`; `SweepRetimed` takes `1u << 10`. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

## Design decision

**Freeze = hold the last converged trimmed ratio, stop integrating, absorb drift with FIFO headroom; latch an emergency-correction edge only when the RAW FIFO fill crosses a hard bound.** (Audit §7 step 3: "freeze the ClockBridge ratio for its duration … recenter only in silence … invalidate if an emergency correction is forced mid-sweep.")

The PI loop has converged (post-prime to `kTargetFill = 0.5`, post-preflight streaming) by the time the sweep arms, so the pre-sweep estimate is exactly the value `ratioTrim` already holds — snapshot it, no separate estimation pass. While frozen we emit that ratio every block (constant `publishedRatio`, so `HealthMonitor`'s drift state machine sees a constant ratio and cannot accumulate the sub-0.5% creep D6 describes), and do not touch `integ`/`ratioTrim` — only the read-only `smoothedFill` observation stays current so `fifoFill()` is honest.

*Emergency-correction definition:* while frozen, the held ratio lets the FIFO fill drift. If the **raw** fill reaches a hard near-empty bound (`fillFrac < kFreezeFloor`) the interpolator is about to starve, or a hard near-full bound (`fillFrac > kFreezeCeil`) the producer is about to overrun — either means a real drop/insert is imminent that nonuniformly retimes the sweep. We latch an atomic `emergencyCorrection_` edge; we do **not** auto-unfreeze or auto-steer (steering through it is exactly the creep D6 forbids — the measurement is already ruined, so flag it). The engine drains the edge per block and invalidates with `SweepRetimed`.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/ClockBridge.h` | bridge surface + freeze state | declare `setSweepActive(bool)`, `sweepActive()`, `consumeEmergencyCorrection()`, freeze constants (tied to `kTargetFill`) + atomics/scratch |
| `src/audio/ClockBridge.cpp` | PI loop + SRC pull | freeze branch in `pullRender` (raw-fill emergency); snapshot trim on entry; reset freeze state in `reset()`/`prepare()` |
| `src/audio/EngineTypes.h` | `HealthFlag` enum | add invalidating `SweepRetimed = 1u << 10` |
| `src/audio/HealthMonitor.cpp` | invalidating mask + report | add `SweepRetimed` to the `raise()` invalidating set; add `reportSweepRetimed()` |
| `src/audio/HealthMonitor.h` | telemetry surface | declare `reportSweepRetimed()` |
| `src/audio/AudioEngine.cpp` | capture + render callbacks | capture: `bridge.setSweepActive(session_.sweepActive())`; render: drain `consumeEmergencyCorrection()` → `reportSweepRetimed()`; optional belt-and-braces clear in `start()` |
| `src/audio/AudioEngine.h` | test seam | add `bridgeSweepFrozen()` test accessor (NO new `setSweepActive`/`sweepActive` — D5 has `sweepActive()`) |
| `src/gui/ClipStatus.h` | flag→message helper | insert the `SweepRetimed` branch into the `juce::String` function |
| `tests/test_clockbridge.cpp` | bridge unit tests | freeze-holds-ratio, freeze-absorbs-small-drift, freeze-emergency-on-large-drift, unfreeze-recenters |
| `tests/test_healthmonitor.cpp` | monitor unit tests | `reportSweepRetimed` invalidates + flags |
| `tests/test_clipstatus.cpp` | message helper tests | `SweepRetimed` message (juce::String) |
| `tests/test_audioengine.cpp` | seam test | a sustained loud sweep freezes the bridge; the gap releases it |

---

## Task 1: Freeze API on ClockBridge — hold the converged ratio during a sweep

**Files:** Modify `src/audio/ClockBridge.h`, `src/audio/ClockBridge.cpp`; Test `tests/test_clockbridge.cpp`

**Interfaces:**
- `void ClockBridge::setSweepActive(bool active) noexcept` — flips the freeze flag (any thread; the consumer reads it). `true`: the next `pullRender` snapshots the converged trim and holds it; `false`: resumes PI steering and re-arms for the next sweep.
- `bool ClockBridge::sweepActive() const noexcept`.
- `bool ClockBridge::consumeEmergencyCorrection() noexcept` — read-and-clear edge: true once if the raw FIFO fill crossed a hard bound while frozen.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_clockbridge.cpp`:

```cpp
TEST_CASE("ClockBridge: frozen ratio is held constant during a sweep") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.25f), out (256, 0.0f);
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256);   // first frozen block snapshots
    const double frozen = cb.currentRatio();
    REQUIRE (cb.sweepActive());
    for (int i = 0; i < 200; ++i) {
        cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256);
        CHECK (cb.currentRatio() == frozen);                 // ratio frozen: zero creep
    }
    CHECK_FALSE (cb.consumeEmergencyCorrection());           // balanced feed => no forced correction
}

TEST_CASE("ClockBridge: freeze absorbs small drift with FIFO headroom (no emergency, no creep)") {
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
    CHECK (raised);                                          // raw near-empty crossing forced a correction
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
    cb.setSweepActive (false);                              // recenter only between sweeps / in silence
    REQUIRE_FALSE (cb.sweepActive());
    bool moved = false;
    for (int i = 0; i < 50; ++i) {
        cb.pushCapture (blk.data(), 256); cb.pushCapture (blk.data(), 256);   // overfeed -> fill rises
        cb.pullRender (out.data(), 256);
        if (cb.currentRatio() != frozen) { moved = true; break; }
    }
    CHECK (moved);                                          // steering resumed
}
```

- [ ] **Step 2: Run the tests to see them fail** — `setSweepActive`/`sweepActive`/`consumeEmergencyCorrection` undeclared.

- [ ] **Step 3: Declare the API + freeze constants + state in `ClockBridge.h`**

Add the public methods after `prime`/`primeToTarget`:
```cpp
    // ---- D6: freeze the SRC ratio during a Dirac sweep --------------------------------------
    // The PI fill-controller normally re-trims the resample ratio every render block; during a sweep
    // that continuous sub-0.5% creep nonuniformly retimes Dirac's log sweep (smears the recovered IR)
    // while staying under the drift latch -> reads "clean". While the sweep is active we HOLD the
    // converged trim (publishedRatio stops moving), absorb short-term clock drift with the FIFO
    // headroom, and recenter only between sweeps. If drift outruns the headroom (raw FIFO fill crosses a
    // hard near-empty/near-full bound) a real retiming is forced; we latch that as an emergency edge so
    // the engine can invalidate, rather than silently steer through it. Driven live by D5's session via
    // AudioEngine (setSweepActive(session_.sweepActive()) each capture block).
    void setSweepActive (bool active) noexcept;   // any thread; consumer reads it
    bool sweepActive() const noexcept;
    bool consumeEmergencyCorrection() noexcept;   // read-and-clear: true once if a hard bound crossed while frozen
```

Add the freeze constants after `kTargetFill`:
```cpp
    // Hard FIFO-fill band tolerated WHILE FROZEN (raw fill, not the steering smoother). Tied to
    // kTargetFill so a future setpoint change keeps the band balanced. Outside it the held ratio can no
    // longer hold the FIFO and a real drop/insert is imminent -> emergency correction.
    static constexpr double kFreezeBand  = 0.40;                      // ± headroom around the setpoint
    static constexpr double kFreezeFloor = kTargetFill - kFreezeBand; // 0.10: near-empty (interpolator starve)
    static constexpr double kFreezeCeil  = kTargetFill + kFreezeBand; // 0.90: near-full (producer overrun)
```

Add the freeze state in the `private` section (near the existing PI scratch + atomics):
```cpp
    // D6 freeze state. sweepActive_/emergencyCorrection_ are cross-thread atomics; frozenRatio_/
    // freezeArmed_ are CONSUMER-THREAD-ONLY scratch (written/read only inside pullRender).
    std::atomic<bool>   sweepActive_ { false };
    std::atomic<bool>   emergencyCorrection_ { false };   // edge, drained by consumeEmergencyCorrection()
    bool   freezeArmed_  = false;   // false until the first frozen block snapshots the trim
    double frozenRatio_  = 1.0;     // the held capture:render ratio while frozen
```

- [ ] **Step 4: Implement the freeze branch in `pullRender` + reset the state**

In `src/audio/ClockBridge.cpp`, replace the **entire PI block** at the top of `pullRender` — from the `// --- PI fill-control` comment through `publishedRatio.store (ratio);` (this span INCLUDES the existing `const double nominal = captureRate / renderRate;` and `const double ratio = nominal * ratioTrim;` lines; they are DELETED and re-declared once below) — with:
```cpp
    // --- Fill observation (always current, even while frozen, so fifoFill() stays honest). ---
    const double fillFrac = (double) fifo.getNumReady() / (double) capacity;
    smoothedFill += 0.01 * (fillFrac - smoothedFill);        // 1-pole smoother (steering only)
    publishedFill.store (smoothedFill);

    const double nominal = captureRate / renderRate;          // input samples per output sample
    double ratio;
    if (sweepActive_.load()) {
        // --- D6 FROZEN: hold the converged trim; do NOT advance the PI integrator (no creep). ---
        if (! freezeArmed_) { frozenRatio_ = nominal * ratioTrim; freezeArmed_ = true; }   // snapshot once
        ratio = frozenRatio_;
        // RAW fill (not the lagged smoother) for the imminence check: the held ratio can no longer keep
        // the FIFO and a real drop/insert is imminent -> flag NOW. Do NOT steer (that is the creep D6 forbids).
        if (fillFrac < kFreezeFloor || fillFrac > kFreezeCeil)
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
Leave the rest of `pullRender` (from the `// Input samples the interpolator MAY need` comment onward) unchanged — it already uses the local `ratio`.

**Checklist after editing:** grep `pullRender` for `nominal` and `ratio` — `nominal` must have exactly ONE declaration (the shared prologue above), and `ratio` must be declared once (`double ratio;`) then assigned in each branch. If you see a second `const double nominal` or `const double ratio`, you didn't delete the original PI-block declarations — remove them.

In `prepare` and `reset`, clear the freeze state right after the existing `smoothedFill = kTargetFill; ratioTrim = 1.0; integ = 0.0;` line (BOTH blocks):
```cpp
    sweepActive_.store (false); emergencyCorrection_.store (false);
    freezeArmed_ = false; frozenRatio_ = captureRate / juce::jmax (1.0, renderRate);
```

Add the method bodies near the other getters (after `currentRatio()`):
```cpp
void ClockBridge::setSweepActive (bool active) noexcept { sweepActive_.store (active); }
bool ClockBridge::sweepActive() const noexcept { return sweepActive_.load(); }
bool ClockBridge::consumeEmergencyCorrection() noexcept { return emergencyCorrection_.exchange (false); }
```

- [ ] **Step 5: Run the tests** — `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "ClockBridge" --output-on-failure`. All four new freeze tests pass; existing ClockBridge tests (equal-rates, 96k->48k, drift-absorbed, priming) still pass (they never call `setSweepActive` → unchanged FREE branch).

- [ ] **Step 6: Commit**
```bash
git add src/audio/ClockBridge.h src/audio/ClockBridge.cpp tests/test_clockbridge.cpp
git commit -m "ClockBridge: freeze the SRC ratio during a sweep (D6/R19)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `SweepRetimed` invalidating flag + HealthMonitor report path

**Files:** `src/audio/EngineTypes.h`, `src/audio/HealthMonitor.h`, `src/audio/HealthMonitor.cpp`; Test `tests/test_healthmonitor.cpp`

**Interfaces:**
- `HealthFlag::SweepRetimed = 1u << 10` — INVALIDATING (the next free bit; `OsResampled` holds `1u << 9`). Distinct from `Dropout` per the audit.
- `void HealthMonitor::reportSweepRetimed() noexcept` — raises `SweepRetimed` (clears `cleanCapture`).

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
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::OsResampled));  // and NOT aliased onto OsResampled (1u<<9)
}
```

- [ ] **Step 2: Run the test to see it fail** — `HealthFlag::SweepRetimed`/`reportSweepRetimed` undeclared.

- [ ] **Step 3: Add the enum value**

In `src/audio/EngineTypes.h`, the enum currently ends `OsResampled = 1u << 9` (no trailing comma). Give it a comma and append:
```cpp
    OsResampled   = 1u << 9, // guidance: OS SRC resampled the INPUT (D2) -> clip detection approximate; NOT invalidating
    SweepRetimed  = 1u << 10 // INVALIDATING: a forced mid-sweep SRC correction retimed the sweep
```

- [ ] **Step 4: Declare + implement the report method and extend the invalidating mask**

In `src/audio/HealthMonitor.h`, declare it next to `reportDroppedFrames`:
```cpp
    void reportSweepRetimed() noexcept;      // a forced mid-sweep SRC correction (D6): invalidating
```

In `src/audio/HealthMonitor.cpp`, add `SweepRetimed` to the invalidating mask in `raise()` (the six-flag mask currently ends with `NonFinite` and no trailing pipe):
```cpp
        static_cast<unsigned> (HealthFlag::NonFinite)     |
        static_cast<unsigned> (HealthFlag::SweepRetimed);
```

Add the body after `reportDroppedFrames`:
```cpp
void HealthMonitor::reportSweepRetimed() noexcept {
    // A held (frozen) SRC ratio could no longer absorb the clock drift, so a real drop/insert was
    // forced mid-sweep -> the sweep was nonuniformly retimed and the measurement is invalid. Kept as
    // its own flag (NOT Dropout) so the GUI can name the cause honestly (see ClipStatus.h).
    raise (HealthFlag::SweepRetimed);
}
```

- [ ] **Step 5: Run the test** — `./tools/dev.cmd ctest --test-dir build -R "reportSweepRetimed" --output-on-failure`. Passes; existing HealthMonitor tests stay green (the mask only gained a flag).

- [ ] **Step 6: Commit**
```bash
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "HealthMonitor: SweepRetimed invalidating flag (1u<<10) for forced mid-sweep correction (D6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Honest status message for `SweepRetimed`

**Files:** `src/gui/ClipStatus.h`; Test `tests/test_clipstatus.cpp`

**Interfaces:** `invalidMeasurementMessage(HealthFlag)` (which RETURNS `juce::String`) gains a `SweepRetimed` branch, inserted **after `NonFinite` and before `ExcessDrift`** (clock-retiming is a more specific cause than generic drift). The four existing branches (ClipConfirmed +OsResampled-caveat, NonFinite, ExcessDrift, Dropout) are PRESERVED.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_clipstatus.cpp`:
```cpp
TEST_CASE("invalidMeasurementMessage: SweepRetimed names the clock-retiming cause") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (invalidMeasurementMessage (HealthFlag::SweepRetimed).containsIgnoreCase ("retim"));
    // ClipConfirmed and NonFinite still take precedence.
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::SweepRetimed)
               .containsIgnoreCase ("full scale"));
    // SweepRetimed outranks the generic ExcessDrift wording.
    CHECK (invalidMeasurementMessage (HealthFlag::SweepRetimed | HealthFlag::ExcessDrift).toStdString()
               == "Clock drift retimed the sweep - this measurement is invalid. Repeat the measurement.");
}
```

- [ ] **Step 2: Run the test to see it fail** — the `SweepRetimed` case falls through to "Dropouts detected".

- [ ] **Step 3: Insert the branch**

In `src/gui/ClipStatus.h`, INSERT the `SweepRetimed` branch into the existing `juce::String` function, after the `NonFinite` branch and before the `ExcessDrift` branch (do NOT change the return type or the other branches):
```cpp
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    if (any (flags & HealthFlag::SweepRetimed))
        return "Clock drift retimed the sweep - this measurement is invalid. Repeat the measurement.";
    if (any (flags & HealthFlag::ExcessDrift))            // preserved from the clipping-review-fixes slice
        return "Sample-clock drift detected - this measurement is invalid.";
    return "Dropouts detected - this measurement is invalid.";
```

- [ ] **Step 4: Run the test** — `./tools/dev.cmd ctest --test-dir build -R "invalidMeasurementMessage" --output-on-failure`. Passes; the existing ClipStatus tests (ExcessDrift / OsResampled / most-specific-cause) stay green.

- [ ] **Step 5: Commit**
```bash
git add src/gui/ClipStatus.h tests/test_clipstatus.cpp
git commit -m "ClipStatus: honest message for a sweep retimed by clock drift (D6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Wire the freeze to D5's session + invalidate on a forced correction

**Files:** `src/audio/AudioEngine.h` (test accessor only), `src/audio/AudioEngine.cpp` (capture + render callbacks, start); Test `tests/test_audioengine.cpp`

**Interfaces:** NO new `AudioEngine::setSweepActive`/`sweepActive` (D5 already provides `sweepActive()` at `AudioEngine.h:70`). The freeze is driven by syncing the ClockBridge to the session each capture block; the render callback drains the emergency edge.
- `bool AudioEngine::bridgeSweepFrozen() const noexcept` — test accessor returning `bridge.sweepActive()`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_audioengine.cpp`:
```cpp
TEST_CASE("AudioEngine: a sustained loud sweep freezes the ClockBridge ratio, the gap releases it") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);
    std::vector<float> loud (512, 0.3f), mono (512, 0.0f), quiet (512, 0.0f);  // 0.3 > kSweepStartLinear (-24 dBFS)
    CHECK_FALSE (e.bridgeSweepFrozen());
    // kArmSustainBlocks (3) sustained loud blocks arm SweepActive -> the capture sync freezes the bridge.
    for (int b = 0; b < 4; ++b) e.processCaptureBlockForTest (loud.data(), loud.data(), mono.data(), 512);
    CHECK (e.bridgeSweepFrozen());
    CHECK (e.sweepActive());
    // Sustained silence completes the segment -> session leaves SweepActive -> the bridge releases.
    for (int b = 0; b < 200; ++b) e.processCaptureBlockForTest (quiet.data(), quiet.data(), mono.data(), 512);
    CHECK_FALSE (e.bridgeSweepFrozen());
}
```
(The full callback-level emergency-drain → `reportSweepRetimed` → `cleanCapture==false` chain is covered at the ClockBridge + HealthMonitor unit boundaries in Tasks 1-2; the end-to-end soak/integration test belongs to R22.)

- [ ] **Step 2: Run the test to see it fail** — `AudioEngine::bridgeSweepFrozen` undeclared.

- [ ] **Step 3: Add the test accessor in `AudioEngine.h`**

In the `// ---- Headless test seam ----` block (next to `prepareForTest`/`processCaptureBlockForTest`):
```cpp
    bool bridgeSweepFrozen() const noexcept { return bridge.sweepActive(); }   // D6 test accessor
```

- [ ] **Step 4: Sync the bridge from the session + drain the emergency edge**

In `src/audio/AudioEngine.cpp` `CaptureCallback::audioDeviceIOCallbackWithContext`, immediately AFTER `if (e.session_.consumeSweepStarted()) e.hm.resetMeasurementLatches();` (and before `e.hm.analyzeInputBlock`), add ONE line:
```cpp
        e.bridge.setSweepActive (e.session_.sweepActive());   // D6: freeze the SRC ratio during the sweep, release between sweeps
```
**KEEP the NaN-safe capture body** (`if (e.graph.process (l, r, mono.data(), numSamples)) e.hm.reportNonFinite();`, no `scanAndFlagNonFinite`) — add only this one line.

Mirror it in `processCaptureBlockForTest`, immediately after `if (session_.consumeSweepStarted()) hm.resetMeasurementLatches();`:
```cpp
    bridge.setSweepActive (session_.sweepActive());
```

In `RenderCallback::audioDeviceIOCallbackWithContext`, after the `observeRenderBlock` if/else pair (the `if (e.usingAggregate_) e.hm.observeRenderBlock(...) else e.hm.observeRenderBlock(...)`) and BEFORE the `for (int ch = 0; ch < numOut; ++ch)` channel-duplication loop, add:
```cpp
        // D6: while frozen the held SRC ratio absorbs short-term drift with FIFO headroom; if drift
        // outran the headroom the bridge flagged a forced retiming -> invalidate. Drain the edge every
        // block. Skip the macOS aggregate path: there the FIFO is clock-locked 1:1, the freeze is inert,
        // and the aggregate's own drift wobble must not be mistaken for a forced sweep correction.
        if (! e.usingAggregate_ && e.bridge.consumeEmergencyCorrection())
            e.hm.reportSweepRetimed();
```

In `start()`, after the existing `bridge.primeToTarget();`, add a belt-and-braces clear (optional but tidy — `bridge.reset()` already cleared the freeze state, and `session_.reset()` puts the session Idle so the capture-sync sets the bridge inactive on block 1 anyway):
```cpp
    bridge.setSweepActive (false);   // D6: a fresh run starts free-running; the session arms the sweep later
```
`stop()` needs no change (`bridge.reset()` clears the freeze state; `session_.reset()` returns the session to Idle).

- [ ] **Step 5: Run the seam test** — `./tools/dev.cmd ctest --test-dir build -R "AudioEngine" --output-on-failure`. The new test passes; existing AudioEngine tests stay green.

- [ ] **Step 6: Run the FULL suite, build the app, commit**

`./tools/dev.cmd ctest --test-dir build --output-on-failure` (all green) then `./tools/dev.cmd cmake --build build --target EarsBridge`.
```bash
git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "AudioEngine: drive the sweep-freeze from D5's session + invalidate on a forced correction (D6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (run before finishing)

- [ ] Full suite green: `./tools/dev.cmd ctest --test-dir build --output-on-failure`.
- [ ] App builds: `./tools/dev.cmd cmake --build build --target EarsBridge`.
- [ ] **On-device / R22 ratification (not headless-testable):** during a real Dirac sweep, the SRC ratio is held constant (no creep) and the emergency only fires on a genuine fault; the inter-sweep gap is long enough for the PI to reconverge before the right earcup re-snapshots (folds into the D5 smoke that measures the gap — if the gap is materially < ~1.5 s, consider holding one snapshot across both sweeps); and a `SweepRetimed` event halts/re-prompts the run rather than limping into the right earcup with a corrupted FIFO.

Then use **superpowers:finishing-a-development-branch**.

## Self-review

**Spec coverage (audit D6 / R19, §7 step 3, §9):** snapshot the converged trim at sweep onset (Task 1, `frozenRatio_ = nominal * ratioTrim`); FREEZE during `SweepActive`, absorbing drift with FIFO headroom and not steering (Task 1 frozen branch; tests "held constant"/"absorbs small drift"); recenter between sweeps (Task 4 wiring releases the freeze during D5's inter-sweep `Complete` gap → the PI reconverges → the right earcup re-snapshots; Task 1 "unfreeze recenters"); INVALIDATE on a forced mid-sweep correction (raw-fill emergency → render drain → `reportSweepRetimed` → `cleanCapture` false; tests "emergency"/"reportSweepRetimed invalidates"; honest GUI string Task 3). The `SweepRetimed` flag is distinct from `Dropout`.

**Integration with merged work.** `SweepRetimed = 1u << 10` (not `1u << 9`, which is D2's `OsResampled`); `invalidMeasurementMessage` stays `juce::String` with the preserved ExcessDrift branch + OsResampled caveat (SweepRetimed inserted between NonFinite and ExcessDrift); D6 is LIVE, driven by D5's `session_.sweepActive()` (no duplicate `AudioEngine::sweepActive()`); the NaN-safe capture body is untouched (only one bridge-sync line added).

**Type consistency.** `setSweepActive(bool)`/`sweepActive()`/`consumeEmergencyCorrection()` on `ClockBridge`; `reportSweepRetimed()` follows the `reportDroppedFrames()` void-no-arg style through `raise()`; `kFreezeFloor`/`kFreezeCeil` are `static constexpr double` tied to `kTargetFill`; `bridgeSweepFrozen()` is a const test accessor like the existing seam.

**RT-safety.** The frozen branch is plain locals + atomic load/store; `freezeArmed_`/`frozenRatio_` are consumer-thread-only scratch (touched solely in `pullRender`). The capture-thread `bridge.setSweepActive(session_.sweepActive())` is one atomic store read lock-free by the render thread (eventual consistency — the freeze engages at most a block late, which is fine). The render drain is one atomic exchange + a rare `raise()`. No alloc/lock/syscall/log/exception added to any callback.
