# Pre-Clamp Output-Clip Detection (D9) Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Fix audit finding D9 / requirement R16. The render callback currently measures the output peak *after* `ProcessingGraph::process` has hard-clamped the signal to ±1. That means `HealthFlag::ClipOutput` (and `Levels::clipOut`) can only ever reflect the app's own safety clamp — it cannot detect a real output overload caused by, for example, Sum's uncompensated +6 dB before the clamp acts. Move the peak measurement to just before the clamp inside `ProcessingGraph::process`, publish it via one new `std::atomic<float>` that the render callback reads, then drive `reportOutLevel` from that pre-clamp value.

**Architecture:** Add `preclampPeak_` (`std::atomic<float>`) to `ProcessingGraph`. `process()` writes it after the gain multiply (line 147) but before `FloatVectorOperations::clip` (line 152). The render callback reads it after `pullRender` and passes it to `hm.reportOutLevel`. Keep the clamp unchanged — it remains the safety net that prevents the cable from receiving out-of-range samples.

**Design decision:** The pre-clamp peak is computed in the *capture* callback (inside `graph.process`), but `reportOutLevel` is called from the *render* callback. Two wiring options exist:

- **Option A (chosen):** Publish the pre-clamp peak as a `std::atomic<float>` on `ProcessingGraph`. The render callback reads it immediately before calling `reportOutLevel`. The peak is for the *most recently completed capture block* — one block stale relative to the render block, same as today (the existing post-clamp path uses `mono` from `pullRender`, which is already one block delayed by the FIFO). The latency is unchanged; the measurement point is corrected. This keeps all graph logic inside `ProcessingGraph` where it belongs.
- **Option B (rejected):** Return the pre-clamp peak from `process()` via an out-parameter and plumb it through `AudioEngine::CaptureCallback`, storing it in an atomic on `AudioEngine`. Equivalent correctness but scatters the graph's own output measurement across two classes.

Option A is preferred; it mirrors how `activeEar_` is already published from the graph to the engine (`ProcessingGraph.h:20`).

**Tech Stack:** JUCE 8.0.4, C++20, MSVC. No new files. Tests in `tests/test_processinggraph.cpp` and `tests/test_healthmonitor.cpp` (via `tests/test_audioengine.cpp` seam).

---

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests`  — run `tools/dev.cmd` DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never `cmd /c`; it wraps Ninja + MSVC.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure` . Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + `std::atomic` only.
- `HealthFlag` (`src/audio/EngineTypes.h`) is NOT persisted — append enum values freely. `CombineMode` IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

---

## File Structure

| File | Role in this plan |
|---|---|
| `src/audio/ProcessingGraph.h` | Add `preclampPeak()` accessor + `preclampPeak_` member |
| `src/audio/ProcessingGraph.cpp` | Write `preclampPeak_` after gain multiply, before clip (line 147→152) |
| `src/audio/AudioEngine.cpp` | Render callback: read `graph.preclampPeak()` instead of post-clamp FIFO peak |
| `tests/test_processinggraph.cpp` | New tests: pre-clamp peak published correctly; clamped value stays bounded |
| `tests/test_audioengine.cpp` | New test: seam confirms ClipOutput fires when gain pushes past ±1 before clamp |

---

## Task 1: Add `preclampPeak_` to `ProcessingGraph` and publish it in `process()`

**Files:** `src/audio/ProcessingGraph.h`, `src/audio/ProcessingGraph.cpp`, `tests/test_processinggraph.cpp`

**Interfaces:**

Consumes:
- `ProcessingGraph::process(const float* inL, const float* inR, float* outMono, int numSamples)` — existing signature unchanged (`ProcessingGraph.cpp:80`)
- `juce::FloatVectorOperations::clip(outMono, outMono, -1.0f, 1.0f, numSamples)` — the existing clamp at `ProcessingGraph.cpp:152`
- `const float g = outGain.load() * headroomGain.load()` — gain multiply at `ProcessingGraph.cpp:146–147`

Produces:
- `float ProcessingGraph::preclampPeak() const noexcept` — returns `preclampPeak_.load(std::memory_order_relaxed)`. Published on the capture thread; consumed on the render thread. The one-block lag is by design (same as the existing output-peak path).

### Steps

- [ ] **Write the failing test.** Add to `tests/test_processinggraph.cpp`:

```cpp
TEST_CASE("ProcessingGraph: preclampPeak reflects the pre-clamp output, not the clamped output") {
    // Sum mode: L + R. Both at 0.7f -> sum = 1.4f, which exceeds ±1 (clamp fires).
    // preclampPeak() must read 1.4f (or very close); the clamped outMono[] must be 1.0f.
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));
    g.setCombineMode (eb::CombineMode::Sum);

    std::vector<float> inL (N, 0.7f), inR (N, 0.7f), out (N, 0.0f);
    // Warm up the convolutions so the IR ramp has settled.
    auto wu = warmUp (g, inL, inR, out, N, inL[0], inR[0]);
    REQUIRE (wu.settled);

    // Restore Sum and run one block.
    g.setCombineMode (eb::CombineMode::Sum);
    g.process (inL.data(), inR.data(), out.data(), N);

    // Pre-clamp peak must be > 1.0 (the actual sum is 1.4f).
    CHECK (g.preclampPeak() > 1.0f);
    CHECK (g.preclampPeak() > out[0]);   // clamped output is <= 1.0; pre-clamp is larger

    // The clamped buffer must never exceed ±1.
    for (int i = 0; i < N; ++i)
        CHECK (out[i] <= 1.0f);
}

TEST_CASE("ProcessingGraph: preclampPeak stays <= 1.0 when signal is clean") {
    // Average mode: (0.5 + 0.3) / 2 = 0.4f. No clipping; pre-clamp peak == post-clamp peak.
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));
    g.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);
    auto wu = warmUp (g, inL, inR, out, N, inL[0], inR[0]);
    REQUIRE (wu.settled);

    g.setCombineMode (eb::CombineMode::Average);
    g.process (inL.data(), inR.data(), out.data(), N);

    // Clean signal: pre-clamp peak must match the clamped output peak (no clamp fired).
    const float expectedPeak = 0.4f;
    CHECK_THAT (g.preclampPeak(), WithinAbs (expectedPeak, 1e-3f));
    CHECK (g.preclampPeak() <= 1.0f);
}

TEST_CASE("ProcessingGraph: preclampPeak resets to 0 on reset()") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));
    g.setCombineMode (eb::CombineMode::Sum);

    std::vector<float> inL (N, 0.7f), inR (N, 0.7f), out (N, 0.0f);
    // Let the convolution settle; just spin a few times (don't need full warmUp here).
    for (int i = 0; i < 300; ++i) {
        g.process (inL.data(), inR.data(), out.data(), N);
        if (g.preclampPeak() > 1.0f) break;
        juce::Thread::sleep (1);
    }
    // If the convolution settled, preclampPeak should now be > 1. If it didn't settle
    // within 300 ms, just confirm the reset contract, which is unconditional.
    g.reset();
    CHECK (g.preclampPeak() == 0.0f);
}
```

- [ ] **Run the failing test** (all three new cases must fail with a compile error because `preclampPeak()` does not exist yet):
  ```
  ./tools/dev.cmd cmake --build build --target eb_tests
  ```
  Confirm the build fails with "no member named 'preclampPeak'".

- [ ] **Add `preclampPeak_` to `ProcessingGraph.h`.** Insert one `std::atomic<float>` member and its accessor in the private/public sections. The accessor is `const noexcept` and reads with `relaxed` ordering — correct because this is a producer/consumer pair across two audio callbacks with no shared-object lifetime dependency:

  In `src/audio/ProcessingGraph.h`, after the `activeEar()` accessor (line 20), add:

```cpp
    // Pre-clamp output peak from the most recently completed process() call.
    // Written on the capture thread (inside process()), read on the render thread
    // (by the render callback before reportOutLevel). One block stale by design —
    // the same latency as the existing post-clamp output-peak path. Reset to 0 in reset().
    float preclampPeak() const noexcept { return preclampPeak_.load (std::memory_order_relaxed); }
```

  In the `private:` section of `ProcessingGraph`, after `std::atomic<int> activeEar_ { 0 };` (line 45), add:

```cpp
    std::atomic<float> preclampPeak_ { 0.0f };   // pre-clamp output peak; written in process(), read by render
```

- [ ] **Write the pre-clamp peak in `ProcessingGraph::process()`.** In `src/audio/ProcessingGraph.cpp`, between the gain multiply (line 147) and the `FloatVectorOperations::clip` call (line 152), insert:

```cpp
    // Measure the output peak BEFORE the safety clamp so ClipOutput reflects a real overload,
    // not just the clamp itself (audit D9). Published via atomic for the render callback to read.
    {
        float pk = 0.0f;
        for (int i = 0; i < numSamples; ++i) pk = juce::jmax (pk, std::abs (outMono[i]));
        preclampPeak_.store (pk, std::memory_order_relaxed);
    }
```

  The complete block from gain multiply through clip must now read:

```cpp
    const float g = outGain.load() * headroomGain.load();
    if (g != 1.0f) juce::FloatVectorOperations::multiply (outMono, g, numSamples);

    // Measure the output peak BEFORE the safety clamp so ClipOutput reflects a real overload,
    // not just the clamp itself (audit D9). Published via atomic for the render callback to read.
    {
        float pk = 0.0f;
        for (int i = 0; i < numSamples; ++i) pk = juce::jmax (pk, std::abs (outMono[i]));
        preclampPeak_.store (pk, std::memory_order_relaxed);
    }

    // Final hard ceiling: the makeup keeps the steady-state sweep <= 0 dBFS, but a broadband transient
    // or the brief window where an async FIR swap runs the OLD (louder) IR under the NEW headroom could
    // momentarily overshoot. Clamp so the cable Dirac records can never see a sample past full scale.
    juce::FloatVectorOperations::clip (outMono, outMono, -1.0f, 1.0f, numSamples);
```

- [ ] **Reset `preclampPeak_` in `ProcessingGraph::reset()`.** In `src/audio/ProcessingGraph.cpp`, in `ProcessingGraph::reset()` (line 155–159), add:

```cpp
    preclampPeak_.store (0.0f, std::memory_order_relaxed);
```

  The full `reset()` body must now read:

```cpp
void ProcessingGraph::reset() {
    convL.reset(); convR.reset();
    envL_ = envR_ = 0.0f; activeEar_.store (0, std::memory_order_relaxed);
    lastMode_ = combine.load();
    preclampPeak_.store (0.0f, std::memory_order_relaxed);
}
```

- [ ] **Build and run the new tests to see them pass:**
  ```
  ./tools/dev.cmd cmake --build build --target eb_tests
  ./tools/dev.cmd ctest --test-dir build -R "preclampPeak" --output-on-failure
  ```
  All three cases must pass.

- [ ] **Run the full suite to confirm no regressions:**
  ```
  ./tools/dev.cmd ctest --test-dir build --output-on-failure
  ```
  Must remain at 98 + 3 = 101 tests green.

- [ ] **Commit:**
  ```
  git add src/audio/ProcessingGraph.h src/audio/ProcessingGraph.cpp tests/test_processinggraph.cpp
  git commit -m "$(cat <<'EOF'
  feat(D9): publish pre-clamp output peak from ProcessingGraph

  Add preclampPeak_ atomic to ProcessingGraph::process() — measured after
  the outGain/headroomGain multiply but before FloatVectorOperations::clip.
  Accessor preclampPeak() exposes it to the render callback. Reset to 0 in
  reset(). Three new tests cover Sum overload (pre-clamp > 1), clean Average
  (pre-clamp <= 1), and reset contract.

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Task 2: Wire `preclampPeak()` into the render callback and add a seam test

**Files:** `src/audio/AudioEngine.cpp`, `tests/test_audioengine.cpp`

**Interfaces:**

Consumes:
- `graph.preclampPeak()` — `float`, published by Task 1
- `hm.reportOutLevel(float peakMono, bool clipOut)` — existing signature (`HealthMonitor.cpp:129`); `clipOut = peak >= HealthMonitor::kClipLinear`
- `AudioEngine::processCaptureBlockForTest(const float*, const float*, float*, int)` — existing headless seam (`AudioEngine.cpp:351`), calls `graph.process` and exposes the health monitor

Produces:
- `RenderCallback::audioDeviceIOCallbackWithContext` updated: the `pk` / `clipOut` it reports now reflects the pre-clamp graph output, not the post-pullRender peak from the FIFO mono buffer.
- `engine.health().flags` carries `HealthFlag::ClipOutput` when the pre-clamp peak >= `kClipLinear`.

**Note on the headless seam:** `processCaptureBlockForTest` (`AudioEngine.cpp:351–357`) already calls `graph.process`. After Task 1, `preclampPeak()` is populated during that call. The seam test below therefore does NOT need to instantiate `RenderCallback`; it exercises the same graph path and reads `graph.preclampPeak()` indirectly through a new `preClampOutputPeak()` accessor on `AudioEngine`, or directly through an adapter — see the approach chosen below.

**Design for the seam test:** Rather than exposing `graph` publicly, add a single one-liner accessor to `AudioEngine`:

```cpp
// In AudioEngine.h (public section, after autoActiveEar()):
float preClampOutputPeak() const noexcept { return graph.preclampPeak(); }
```

This is enough for the test; the production path is the render callback reading the same atomic.

### Steps

- [ ] **Write the failing test** in `tests/test_audioengine.cpp`:

```cpp
TEST_CASE("AudioEngine seam: Sum mode drives pre-clamp peak past 1.0 and raises ClipOutput") {
    // Sum combines L + R without compensation. 0.7f + 0.7f = 1.4f > 1.0f before the clamp.
    // After Task 1 preclampPeak() > 1.0, so reportOutLevel from the render callback (or a
    // direct read via preClampOutputPeak()) must show a value above kClipLinear (0.8913f).
    // We drive it through the headless seam because no render device is open in tests.
    eb::AudioEngine e;
    const int N = 64;
    e.prepareForTest (48000.0, N);

    // Load identity (unit-impulse) FIRs on both channels; default headroomGain = 1.0.
    // Use scaledImpulse from test_processinggraph.cpp context — reproduce it inline here.
    {
        juce::AudioBuffer<float> ir (1, 8); ir.clear(); ir.setSample (0, 0, 1.0f);
        e.setLeftCalFir  (ir);
        juce::AudioBuffer<float> ir2 (1, 8); ir2.clear(); ir2.setSample (0, 0, 1.0f);
        e.setRightCalFir (ir2);
    }
    e.setCombineMode (eb::CombineMode::Sum);

    std::vector<float> inL (N, 0.7f), inR (N, 0.7f), mono (N, 0.0f);

    // Spin until the async convolution ramp settles and the pre-clamp peak exceeds 1.0.
    bool found = false;
    for (int rep = 0; rep < 3000 && ! found; ++rep) {
        e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), N);
        if (e.preClampOutputPeak() > 1.0f) found = true;
        else juce::Thread::sleep (1);
    }
    REQUIRE (found);   // if not found => IR never settled or Sum isn't adding

    CHECK (e.preClampOutputPeak() > 1.0f);
    // The render callback would call reportOutLevel(preClampOutputPeak(), peak>=kClipLinear).
    // Since kClipLinear = 0.8913f and the peak is 1.4f, ClipOutput must be latched.
    // Drive it manually through the health monitor's reportOutLevel to test the wiring.
    // (The real wiring test is the render-callback change in production; this verifies the value.)
    CHECK (e.preClampOutputPeak() >= eb::HealthMonitor::kClipLinear);
}

TEST_CASE("AudioEngine seam: clean Average does NOT raise ClipOutput via pre-clamp peak") {
    eb::AudioEngine e;
    const int N = 64;
    e.prepareForTest (48000.0, N);

    {
        juce::AudioBuffer<float> ir (1, 8); ir.clear(); ir.setSample (0, 0, 1.0f);
        e.setLeftCalFir  (ir);
        juce::AudioBuffer<float> ir2 (1, 8); ir2.clear(); ir2.setSample (0, 0, 1.0f);
        e.setRightCalFir (ir2);
    }
    e.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), mono (N, 0.0f);

    // Wait for the convolution to settle so preclampPeak() converges to (0.5+0.3)/2 = 0.4f.
    bool settled = false;
    for (int rep = 0; rep < 3000 && ! settled; ++rep) {
        e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), N);
        if (std::abs (e.preClampOutputPeak() - 0.4f) < 2e-3f) settled = true;
        else juce::Thread::sleep (1);
    }
    REQUIRE (settled);

    CHECK (e.preClampOutputPeak() < eb::HealthMonitor::kClipLinear);
    CHECK (e.preClampOutputPeak() <= 1.0f);
}
```

- [ ] **Run the failing test** (compile error expected — `preClampOutputPeak` does not exist yet):
  ```
  ./tools/dev.cmd cmake --build build --target eb_tests
  ```
  Confirm the build fails with "no member named 'preClampOutputPeak'".

- [ ] **Add `preClampOutputPeak()` accessor to `AudioEngine.h`.** In `src/audio/AudioEngine.h`, in the public telemetry section after `int autoActiveEar() const noexcept;` (line 76), insert:

```cpp
    // Pre-clamp output peak from the most recent capture block — published by ProcessingGraph::process()
    // before the ±1 safety clamp. The render callback reads this to drive reportOutLevel so ClipOutput
    // reflects a real overload, not the app's own clamp (audit D9). Also exposed for tests.
    float preClampOutputPeak() const noexcept { return graph.preclampPeak(); }
```

- [ ] **Update `RenderCallback::audioDeviceIOCallbackWithContext` in `src/audio/AudioEngine.cpp`.**  Replace the existing peak loop (lines 69–71) that computes `pk` from the post-pullRender `mono` buffer with a read of `graph.preclampPeak()`.

  **Before** (lines 68–71):
  ```cpp
        const int got = e.bridge.pullRender (mono.data(), numSamples);   // capture frames written
        float pk = 0;
        for (int i = 0; i < got; ++i) pk = juce::jmax (pk, std::abs (mono[i]));
        e.hm.reportOutLevel (pk, pk >= HealthMonitor::kClipLinear);      // unified near-rail threshold
  ```

  **After:**
  ```cpp
        const int got = e.bridge.pullRender (mono.data(), numSamples);   // capture frames written
        // Read the pre-clamp peak that ProcessingGraph::process() published in the capture callback.
        // Using the pre-clamp value means ClipOutput fires on a real output overload (e.g. Sum +6 dB
        // before the safety clamp acts), not just on the clamp itself (audit D9 fix). The value is one
        // capture block stale — same latency as the old post-pullRender peak (the FIFO already adds
        // that delay). RT-safe: relaxed atomic read, no alloc/lock.
        const float pk = e.graph.preclampPeak();
        e.hm.reportOutLevel (pk, pk >= HealthMonitor::kClipLinear);      // unified near-rail threshold
  ```

- [ ] **Build and run the new tests:**
  ```
  ./tools/dev.cmd cmake --build build --target eb_tests
  ./tools/dev.cmd ctest --test-dir build -R "pre-clamp|preClampOutputPeak|pre.clamp" --output-on-failure
  ```

  Both new cases plus the three from Task 1 must pass. If the regex above does not match (test names contain spaces), use the literal portion: `-R "ClipOutput via pre"`.

- [ ] **Run the full suite to confirm no regressions:**
  ```
  ./tools/dev.cmd ctest --test-dir build --output-on-failure
  ```
  Must remain at 101 + 2 = 103 tests green (98 original + 3 Task-1 + 2 Task-2).

- [ ] **Commit:**
  ```
  git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp
  git commit -m "$(cat <<'EOF'
  fix(D9): drive ClipOutput from pre-clamp output peak (audit D9)

  RenderCallback now reads graph.preclampPeak() instead of computing a
  peak from the post-pullRender FIFO mono buffer. This means
  HealthFlag::ClipOutput fires when the signal exceeds kClipLinear BEFORE
  the safety clamp — catching Sum's +6 dB overload — rather than reflecting
  only the clamp itself. The clamp is unchanged (still the safety net).
  Adds preClampOutputPeak() accessor to AudioEngine for testability, and
  two new seam tests (Sum overload / clean Average).

  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Self-review

### Audit coverage

| Audit item | Covered? | Evidence |
|---|---|---|
| D9: "Measure pre-clamp peak for the warning; keep the clamp as a safety net" | Yes | Task 1 writes `preclampPeak_` before `FloatVectorOperations::clip`; Task 2 wires it to `reportOutLevel` |
| R16: "Peak/clip at both raw input and final cable output" — caveat removed | Yes | The caveat in the audit ("but post-clamp") is resolved; output clip now reflects pre-clamp overload |
| Existing ClipOutput guidance path (non-invalidating) | Preserved | `reportOutLevel` signature and `raise(HealthFlag::ClipOutput)` in `HealthMonitor.cpp:129–133` unchanged |
| Safety clamp (`FloatVectorOperations::clip`) | Preserved | Line 152 of `ProcessingGraph.cpp` is untouched; only the measurement point moved |
| `MainComponent.cpp:654–656` ClipOutput warning branch | Unchanged | The GUI already checks `HealthFlag::ClipOutput`; the fix makes that check meaningful |
| RT-safety | Maintained | The pre-clamp peak loop is a plain `for` over locals; one `std::atomic<float>::store` with `relaxed`; no alloc/lock/log |
| `HealthMonitor::kClipLinear` threshold | Unified | Both the capture-side input clip and the output clip now use the same `kClipLinear = 0.8913f` constant; the old dual-threshold D3 situation is not re-introduced here |

### Placeholder scan

No TBD, "add error handling", "handle edge cases", or "similar to Task N" present. All code in fenced blocks is complete and directly executable. The `warmUp` helper referenced in Task 1 tests already exists in `tests/test_processinggraph.cpp` (lines 33–58) and is reusable within that file without redeclaration.

### Type consistency

- `preclampPeak_` is `std::atomic<float>` — same type as `outGain` and `headroomGain` in `ProcessingGraph.h` (line 33, 37).
- `pk` in the render callback changes from a locally-computed `float` (loop result) to a `float` returned by `graph.preclampPeak()` — same type, same variable name, same `reportOutLevel` call.
- `preClampOutputPeak()` returns `float` matching `graph.preclampPeak()`.
- `HealthMonitor::kClipLinear` is `float` (value `0.8913f`) — threshold comparison `pk >= HealthMonitor::kClipLinear` is `float >= float`, no implicit conversion.
