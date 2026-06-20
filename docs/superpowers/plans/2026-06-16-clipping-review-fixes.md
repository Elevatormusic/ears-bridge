# Confirmed-Clipping Review Fixes Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Fix the real correctness findings an xhigh `/code-review` surfaced in the merged confirmed-clipping (D1/D3/D4) slice: (1) a NaN in the raw input permanently poisons the FIR convolution and the SSE output safety-net is dead; (2) the D3 threshold unification made the input *and* output meter "CLIP" LED fire ~1 dB early; (3) `invalidMeasurementMessage` mislabels an Xrun/clock-drift invalidation as "Dropouts detected"; (4) the run-based confirmed-clip false-positives on a clean full-scale low-frequency sine.

**Architecture:** Surgical fixes in the existing surfaces. The NaN fix moves sanitization to `ProcessingGraph::process` (sanitize the input scratch *before* the stateful convolution, and the output *before* the `±1` clamp) and has it return whether it sanitized anything so the callback raises `NonFinite`. The meter fix points the visual "CLIP" latch at the near-full-scale `kRailCeiling` while the guidance flags stay at `kClipLinear` (−1 dBFS). The message fix adds an `ExcessDrift` branch. The false-positive fix requires the rail run to be **flat** (a clipped flat-top has consecutive equal samples; a sine peak varies), so a smooth full-scale sine no longer trips it.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

This implements the verified findings from the confirmed-clipping `/code-review` (NaN/convolution: A/C/D/Efficiency/Altitude angles, CONFIRMED; meter thresholds: A/B/C/Reuse/Simplification, CONFIRMED; message: Reuse/Altitude, CONFIRMED; false-positive: D/Efficiency/Altitude, PLAUSIBLE; the capture null-check: sweep). **Deferred minors** (noted, not fixed here): `railSamples` int overflow on a >12 h continuous-rail run (test-only stat), the redundant `clipConfirmed_` atomic, the triple peak/abs pass, and an all-NaN block tripping guidance `LowLevel`.

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests` — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (loses the MSVC env), never cmd /c.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; 98 tests today).
- RT-safety: `ProcessingGraph::process`, `analyzeInputBlock`, and the capture/render callbacks run on the audio thread — NO heap alloc, lock, syscall, logging, or exception; plain locals + std::atomic only. The new sanitize loop is a plain `std::isfinite` pass.
- HealthFlag is NOT persisted; do not change its values. Do not add OsResampled (that's the separate D2 plan).
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/ProcessingGraph.h` | process signature | `process(...)` returns `bool` (non-finite encountered) |
| `src/audio/ProcessingGraph.cpp` | DSP | sanitize input scratch before convolution; sanitize output before the clamp; return the flag |
| `src/audio/HealthMonitor.h` | telemetry surface | declare `reportNonFinite()`; constants for the flat-run epsilon |
| `src/audio/HealthMonitor.cpp` | analyzeInputBlock + report | meter bool → `kRailCeiling`; flat-run confirmed-clip; `reportNonFinite()` body |
| `src/audio/AudioEngine.cpp` | capture callback + test seam | null-check `in[0]/in[1]`; use `process()`'s bool to raise NonFinite; render meter bool → `kRailCeiling`; mirror in `processCaptureBlockForTest` |
| `src/gui/ClipStatus.h` | invalid-measurement message | add an `ExcessDrift` branch before the dropout fallthrough |
| `tests/test_processinggraph.cpp` | DSP tests | NaN-poison-prevention + return value |
| `tests/test_healthmonitor.cpp` | health tests | meter-vs-flag thresholds; flat-run confirmed; smooth-peak NOT confirmed |
| `tests/test_audioengine.cpp` | seam tests | NaN through the seam invalidates + no poison; null-input guard |
| `tests/test_clipstatus.cpp` | message tests | ExcessDrift message |

---

## Task 1: NaN sanitization — keep non-finite out of the FIR and the cable

**Files:**
- Modify: `src/audio/ProcessingGraph.h` (process signature), `src/audio/ProcessingGraph.cpp:80-153` (process body)
- Modify: `src/audio/HealthMonitor.h` (declare `reportNonFinite`), `src/audio/HealthMonitor.cpp` (body)
- Modify: `src/audio/AudioEngine.cpp` (CaptureCallback body + `processCaptureBlockForTest`)
- Test: `tests/test_processinggraph.cpp`, `tests/test_audioengine.cpp`

**Interfaces:**
- `bool eb::ProcessingGraph::process(const float* inL, const float* inR, float* outMono, int numSamples)` — now returns **true** if it replaced any non-finite sample (in the input scratch before convolution, or the output before the clamp). The convolution never sees a NaN, so its overlap-add state can't be poisoned.
- `void eb::HealthMonitor::reportNonFinite() noexcept` — raises `HealthFlag::NonFinite` (invalidating). For the callback to flag a FIR-produced non-finite that `analyzeInputBlock` (input-only) didn't see.

- [ ] **Step 1: Write the failing test (poison-prevention through the engine seam)**

Append to `tests/test_audioengine.cpp`:
```cpp
TEST_CASE("AudioEngine seam: a NaN input is sanitized and does NOT poison the FIR for later blocks") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> bad (8, 0.3f); bad[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> r0  (8, 0.0f), mono (8, 0.0f);
    e.processCaptureBlockForTest (bad.data(), r0.data(), mono.data(), 8);
    for (float v : mono) CHECK (std::isfinite (v));               // output never non-finite
    CHECK (eb::any (e.health().flags & eb::HealthFlag::NonFinite));
    CHECK_FALSE (e.cleanCapture());

    // A subsequent CLEAN block must produce finite output (proves the convolution wasn't poisoned).
    std::vector<float> clean (8, 0.4f), r1 (8, 0.0f), mono2 (8, 0.0f);
    e.processCaptureBlockForTest (clean.data(), r1.data(), mono2.data(), 8);
    for (float v : mono2) CHECK (std::isfinite (v));
}
```
(Ensure `<limits>` and `<cmath>` are included in the test file.)

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "does NOT poison" --output-on-failure`
Expected: FAIL — the NaN is convolved (poison) and/or the output isn't finite on the second block (on a scalar build) OR compile error once `process` is `bool` and `reportNonFinite` is referenced below.

- [ ] **Step 3: Sanitize in ProcessingGraph::process (and return the flag)**

In `src/audio/ProcessingGraph.h`, change the `process` declaration to return `bool`:
```cpp
    bool process (const float* inL, const float* inR, float* outMono, int numSamples);
```
In `src/audio/ProcessingGraph.cpp`, add a file-static helper above `process` and rework the body (keep the convolution/combine/gain unchanged):
```cpp
// Replace any non-finite sample with 0 in place; return whether any were found. RT-safe (plain loop).
static bool sanitizeNonFinite (float* buf, int n) noexcept {
    bool bad = false;
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (buf[i])) { buf[i] = 0.0f; bad = true; }
    return bad;
}

bool ProcessingGraph::process (const float* inL, const float* inR,
                               float* outMono, int numSamples) {
    auto* l = scratch.getWritePointer (0);
    auto* r = scratch.getWritePointer (1);
    juce::FloatVectorOperations::copy (l, inL, numSamples);
    juce::FloatVectorOperations::copy (r, inR, numSamples);
    // Keep non-finite OUT of the stateful FFT convolution: a single NaN would smear through the
    // overlap-add and poison every later block (permanent silence) until reset.
    bool bad = sanitizeNonFinite (l, numSamples);
    bad = sanitizeNonFinite (r, numSamples) || bad;
```
(Leave the convolution, combine switch, and `g = outGain * headroomGain` multiply exactly as they are.) Then, **before** the final clamp at line 152, sanitize the output and fold it into `bad`, and return it:
```cpp
    // Catch a FIR-produced non-finite BEFORE the clamp: on x86/SSE FloatVectorOperations::clip turns a
    // NaN into 1.0, so a post-clamp scan would miss it and a garbage full-scale sample would reach Dirac.
    bad = sanitizeNonFinite (outMono, numSamples) || bad;
    juce::FloatVectorOperations::clip (outMono, outMono, -1.0f, 1.0f, numSamples);
    return bad;
}
```
Ensure `#include <cmath>` is present in `ProcessingGraph.cpp` (for `std::isfinite`).

- [ ] **Step 4: HealthMonitor::reportNonFinite**

In `src/audio/HealthMonitor.h` (near `scanAndFlagNonFinite`):
```cpp
    void reportNonFinite() noexcept;   // raise HealthFlag::NonFinite (for a FIR-produced non-finite)
```
In `src/audio/HealthMonitor.cpp`:
```cpp
void HealthMonitor::reportNonFinite() noexcept { raise (HealthFlag::NonFinite); }
```

- [ ] **Step 5: Wire the capture callback + test seam (and null-check)**

In `src/audio/AudioEngine.cpp` CaptureCallback, replace the input/process/scan block. The current code is:
```cpp
        const float* l = in[0]; const float* r = in[1];
        e.hm.analyzeInputBlock (l, r, numSamples);
        e.graph.process (l, r, mono.data(), numSamples);
        if (e.hm.scanAndFlagNonFinite (mono.data(), numSamples))
            juce::FloatVectorOperations::clear (mono.data(), numSamples);
        e.bridge.pushCapture (mono.data(), numSamples);
```
Replace with (add the per-pointer null guard the sibling render callback already has; use `process()`'s return to flag a FIR-produced non-finite; drop the now-redundant post-clamp scan since process sanitizes the output before the clamp):
```cpp
        const float* l = in[0]; const float* r = in[1];
        if (l == nullptr || r == nullptr) { e.hm.reportXrun(); return; }
        e.hm.analyzeInputBlock (l, r, numSamples);             // detection (raw input): peak, run, NaN
        if (e.graph.process (l, r, mono.data(), numSamples))   // sanitizes in+out; true => non-finite seen
            e.hm.reportNonFinite();                            // flag a FIR-produced non-finite too
        e.bridge.pushCapture (mono.data(), numSamples);
```
In `processCaptureBlockForTest`, mirror it:
```cpp
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                              float* outMono, int numSamples) {
    hm.analyzeInputBlock (inL, inR, numSamples);
    if (graph.process (inL, inR, outMono, numSamples)) hm.reportNonFinite();
}
```

- [ ] **Step 6: Add a ProcessingGraph-level test + run**

Append to `tests/test_processinggraph.cpp` (reuse the file's existing `warmUp`/identity-FIR helper if present; otherwise prepare with an identity FIR):
```cpp
TEST_CASE("ProcessingGraph::process sanitizes non-finite input and reports it") {
    eb::ProcessingGraph g; g.prepare (48000.0, 8);
    std::vector<float> l (8, 0.2f), r (8, 0.0f), out (8, 0.0f);
    l[2] = std::numeric_limits<float>::infinity();
    const bool bad = g.process (l.data(), r.data(), out.data(), 8);
    CHECK (bad);
    for (float v : out) CHECK (std::isfinite (v));
    // A following clean block stays finite (convolution not poisoned).
    std::vector<float> l2 (8, 0.2f), out2 (8, 0.0f);
    CHECK_FALSE (g.process (l2.data(), r.data(), out2.data(), 8));
    for (float v : out2) CHECK (std::isfinite (v));
}
```
Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "sanitizes non-finite|does NOT poison" --output-on-failure` → PASS; then full suite.

- [ ] **Step 7: Commit**
```bash
git add src/audio/ProcessingGraph.h src/audio/ProcessingGraph.cpp src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp src/audio/AudioEngine.cpp tests/test_processinggraph.cpp tests/test_audioengine.cpp
git commit -m "fix(dsp): sanitize non-finite before the FIR and before the clamp; null-guard capture

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Meter "CLIP" latch at near-full-scale, not the −1 dBFS guidance threshold

**Files:** `src/audio/HealthMonitor.cpp` (analyzeInputBlock), `src/audio/AudioEngine.cpp` (render callback); Test: `tests/test_healthmonitor.cpp`

**Interfaces:** no signature change. The visual meter bool (`Levels.clipL/clipR/clipOut`) now uses `kRailCeiling` (≈ full scale); the guidance `ClipInput`/`ClipOutput` flags continue to fire at `kClipLinear` (−1 dBFS) via `reportInLevels`/`reportOutLevel`'s internal `|| peak >= kClipLinear` check.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("Input meter CLIP latches at the rail, not at the -1 dBFS guidance threshold") {
    using eb::HealthFlag; using eb::any;
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    // -0.4 dBFS (0.95): above the guidance threshold (-1 dBFS) but NOT at the rail.
    std::vector<float> l (16, 0.95f), r (16, 0.0f);
    h.analyzeInputBlock (l.data(), r.data(), 16);
    CHECK_FALSE (h.levels().clipL);                              // meter LED off below the rail
    CHECK (any (h.flags() & HealthFlag::ClipInput));             // guidance still fires at -1 dBFS
    // At the rail (0.9999): meter LED on.
    eb::HealthMonitor h2; h2.prepare (eb::EarsModel::Ears, 4096);
    std::vector<float> l2 (16, 0.9999f), r2 (16, 0.0f);
    h2.analyzeInputBlock (l2.data(), r2.data(), 16);
    CHECK (h2.levels().clipL);
}
```

- [ ] **Step 2: Run test to verify it fails** — `clipL` is currently true at 0.95 (it uses `kClipLinear`).

- [ ] **Step 3: Point the meter bool at kRailCeiling**

In `src/audio/HealthMonitor.cpp` `analyzeInputBlock`, change the final `reportInLevels` call (line 103) from `kClipLinear` to `kRailCeiling`:
```cpp
    reportInLevels (pkL, pkR, pkL >= kRailCeiling, pkR >= kRailCeiling);
```
(`reportInLevels` still raises `ClipInput` at `kClipLinear` via its internal `|| peakL >= kClipLinear || peakR >= kClipLinear`, so guidance is unchanged — only the meter latch moves to the rail.)

In `src/audio/AudioEngine.cpp` RenderCallback, change the output meter bool from `kClipLinear` to `kRailCeiling`:
```cpp
        e.hm.reportOutLevel (pk, pk >= HealthMonitor::kRailCeiling);
```
(`reportOutLevel` still raises `ClipOutput` at `kClipLinear` internally; only the output meter latch moves to the rail.)

- [ ] **Step 4: Run tests** — focused, then full suite. Watch the merged `test_healthmonitor` clip-flag cases (they assert on `ClipInput`, which is unchanged). Expected: green.

- [ ] **Step 5: Commit**
```bash
git add src/audio/HealthMonitor.cpp src/audio/AudioEngine.cpp tests/test_healthmonitor.cpp
git commit -m "fix(meters): latch CLIP at the rail (kRailCeiling), keep guidance at -1 dBFS

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: invalidMeasurementMessage names clock drift distinctly from dropouts

**Files:** `src/gui/ClipStatus.h`; Test: `tests/test_clipstatus.cpp`

**Interfaces:** `invalidMeasurementMessage(HealthFlag)` gains an `ExcessDrift` branch (between `NonFinite` and the dropout fallthrough). An Xrun/Dropout/FifoStarved invalidation still reads "Dropouts detected"; an `ExcessDrift` invalidation now reads "Sample-clock drift…".

- [ ] **Step 1: Write the failing test**

Append to `tests/test_clipstatus.cpp` (match the file's existing wrapper style — `const char*` compares directly to a string literal via `std::string(...)`):
```cpp
TEST_CASE("invalidMeasurementMessage names clock drift distinctly from dropouts") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ExcessDrift))
           == "Sample-clock drift detected - this measurement is invalid.");
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::Xrun))
           == "Dropouts detected - this measurement is invalid.");
    // Clip still outranks drift.
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::ExcessDrift))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
}
```

- [ ] **Step 2: Run test to verify it fails** — ExcessDrift currently falls through to "Dropouts detected".

- [ ] **Step 3: Add the ExcessDrift branch**

In `src/gui/ClipStatus.h` `invalidMeasurementMessage`, add the branch before the final `return "Dropouts detected..."`:
```cpp
    if (any (flags & HealthFlag::ExcessDrift))
        return "Sample-clock drift detected - this measurement is invalid.";
    return "Dropouts detected - this measurement is invalid.";
```
(Keep the existing `ClipConfirmed` then `NonFinite` branches above it — order: ClipConfirmed > NonFinite > ExcessDrift > Dropout/Xrun/FifoStarved.)

- [ ] **Step 4: Run tests** — focused, then full suite. PASS.

- [ ] **Step 5: Commit**
```bash
git add src/gui/ClipStatus.h tests/test_clipstatus.cpp
git commit -m "fix(ui): name sample-clock drift distinctly from dropouts in the invalid-measurement message

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Confirmed clip requires a FLAT rail run (no false-positive on a clean full-scale sine)

**Files:** `src/audio/HealthMonitor.h` (a flat-run epsilon constant + per-channel previous-sample members), `src/audio/HealthMonitor.cpp` (analyzeInputBlock run logic + reset); Test: `tests/test_healthmonitor.cpp`

**Interfaces:** no public signature change. A confirmed clip now requires `kRailRunMin` consecutive samples that are both `>= kRailCeiling` AND **flat** (`|x[i] - x[i-1]| <= kFlatRunEps`). A real digital clip flat-tops at the exact rail code (`delta == 0`); a smooth sine peak varies sample-to-sample (`delta > kFlatRunEps`), so it no longer confirms.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("Confirmed clip requires a FLAT rail run, not a smooth full-scale peak") {
    using eb::HealthFlag; using eb::any;
    SECTION ("flat-topped clip (equal rail samples) -> confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l { 0.5f, 1.0f, 1.0f, 1.0f, 0.5f };   // flat top, delta == 0
        std::vector<float> r (l.size(), 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
        CHECK (h.clipConfirmed());
        CHECK_FALSE (h.cleanCapture());
    }
    SECTION ("smooth full-scale sine peak (varying rail samples) -> NOT confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        // Three consecutive samples above kRailCeiling but each differs by ~1.5e-4 (a sine peak).
        std::vector<float> l { 0.99980f, 0.99993f, 1.00000f, 0.99993f, 0.99980f };
        std::vector<float> r (l.size(), 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());
    }
}
```
(Note: values in the second section are >= kRailCeiling=0.9999 but their pairwise deltas ~1.3e-4 exceed kFlatRunEps, so the run never reaches a flat length of 3.)

- [ ] **Step 2: Run test to verify it fails** — the smooth-peak section currently confirms (the run only checks `>= kRailCeiling`).

- [ ] **Step 3: Constant + previous-sample members (header)**

In `src/audio/HealthMonitor.h` thresholds block:
```cpp
    // A real digital clip flat-tops at the exact rail code (consecutive samples equal); a smooth
    // full-scale sine peak varies sample-to-sample. Require the rail run to be FLAT within this epsilon
    // so a clean loud low-frequency tone doesn't false-positive as a confirmed clip.
    static constexpr float kFlatRunEps = 1.0e-5f;
```
Private members (next to `railRunL_`/`railRunR_`):
```cpp
    float prevL_ = 0.0f, prevR_ = 0.0f;   // previous raw sample per channel (capture-thread scratch)
```

- [ ] **Step 4: Flat-run logic (cpp) + reset**

In `src/audio/HealthMonitor.cpp` `analyzeInputBlock`, replace the per-channel run updates. For channel L, the current:
```cpp
        if (fa) {
            const float ma = std::abs (a);
            pkL = juce::jmax (pkL, ma);
            railRunL_ = (ma >= kRailCeiling) ? railRunL_ + 1 : 0;
            if (ma >= kRailCeiling) ++railL;
        } else {
            railRunL_ = 0;                 // a non-finite sample breaks THIS channel's run only
        }
```
becomes:
```cpp
        if (fa) {
            const float ma = std::abs (a);
            pkL = juce::jmax (pkL, ma);
            const bool atRail = ma >= kRailCeiling;
            const bool flat   = std::abs (a - prevL_) <= kFlatRunEps;   // a true clip is flat; a sine peak isn't
            railRunL_ = atRail ? (flat ? railRunL_ + 1 : 1) : 0;        // not-flat starts a fresh length-1 run
            if (atRail) ++railL;
            prevL_ = a;
        } else {
            railRunL_ = 0; prevL_ = 0.0f;  // a non-finite sample breaks THIS channel's run only
        }
```
Apply the mirror change for channel R (`prevR_`, `railRunR_`, `mb`, `b`). In `reset()` (which already zeroes `railRunL_`/`railRunR_`/`longestRun_`), add:
```cpp
    prevL_ = prevR_ = 0.0f;
```

- [ ] **Step 5: Run tests** — focused `"FLAT rail run"`, then the existing `"consecutive near-rail"` cases (the flat `{1,1,1}`/`{-1,-1,-1}` runs in those still confirm because their deltas are 0), then the full suite. Expected: all green.

- [ ] **Step 6: Commit**
```bash
git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "fix(health): confirmed clip requires a FLAT rail run (no false-positive on a clean full-scale sine)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (run before finishing)

- [ ] Full suite green: `./tools/dev.cmd ctest --test-dir build --output-on-failure`.
- [ ] App builds: `./tools/dev.cmd cmake --build build --target EarsBridge`.

Then use **superpowers:finishing-a-development-branch**.

## Self-review

**Finding coverage:** NaN-poison + SSE-dead-safety-net → Task 1 (sanitize input before the convolution, output before the clamp, raise NonFinite on either). Meter over-warn (D3 side-effect) → Task 2 (meter at `kRailCeiling`, guidance unchanged at `kClipLinear`). Xrun/drift mislabeled "Dropouts" → Task 3 (ExcessDrift branch). False-positive on a clean full-scale sine → Task 4 (flat-run requirement). Capture null-check (sweep) → folded into Task 1. **Deferred minors** (noted): railSamples int overflow (test-only stat), redundant clipConfirmed_ atomic, triple peak/abs pass, all-NaN-block LowLevel.

**Placeholder scan:** every step has complete code grounded in the actual `ProcessingGraph.cpp:80-153`, `HealthMonitor.cpp:70-104`, the capture callback, and `ClipStatus.h`. No TBD/"add error handling"/"similar to".

**Type consistency:** `process(...)->bool`, `reportNonFinite()`, `kRailCeiling`/`kClipLinear`/`kFlatRunEps`, `prevL_`/`prevR_` used identically across tasks.

**RT-safety:** the sanitize loop and the flat-run logic are plain loops over locals; `reportNonFinite` is one atomic `raise()`. No alloc/lock/log added to the audio thread.
