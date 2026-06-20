# Real Audio-Path Test Harness Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Close audit finding **R22** — today no test ever instantiates or drives the *production* `AudioEngine::CaptureCallback` / `RenderCallback`; only the headless `processCaptureBlockForTest` seam (which runs `graph.process` + `hm` analysis directly, never the callbacks) is exercised, and RT-safety is asserted by comment only. This plan adds a minimal public test seam on `AudioEngine` that constructs and DRIVES the real `AudioIODeviceCallback` objects with synthetic buffers (no device I/O), plus a GOLDEN sample-accurate cross-clock transport test through `ClockBridge`, and the audit §9 device-reconnection / format-mismatch / clock-drift / long-soak cases.

**Architecture:** The production capture/render callbacks are private nested structs (`AudioEngine.cpp:13-90`) that hold an `AudioEngine&` and touch its private members (`hm`, `graph`, `bridge`, `lastDroppedCapture_`, `lastUnderruns_`, `usingAggregate_`). They are already constructed in the `AudioEngine` ctor (`captureCb`/`renderCb`, `AudioEngine.cpp:115-116`). The new seam re-prepares the same engine state `start()` builds (`graph.prepare` + `bridge.prepare`/`primeToTarget` + `hm.prepare` + baseline reset, `AudioEngine.cpp:278-309`) and then invokes the *real* callbacks' `audioDeviceIOCallbackWithContext` directly with caller-supplied buffers — exercising the exact shipped control flow (raw-input analysis → `graph.process` → NaN scan → `bridge.pushCapture` → dropped-frame diff on capture; `bridge.pullRender` → out-level → underrun-edge → `observeRenderBlock` → mono-duplicate on render). No new threads, no device, no production-behavior change.

**Tech Stack:** C++17, JUCE 8.0.4, Catch2 (`Catch2WithMain`), Ninja + MSVC via `tools/dev.cmd`.

## Global Constraints

- Build (tests): ./tools/dev.cmd cmake --build build --target eb_tests  — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never cmd /c; it wraps Ninja + MSVC.
- Run a test: ./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure . Full suite: ./tools/dev.cmd ctest --test-dir build --output-on-failure (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + std::atomic only.
- HealthFlag (src/audio/EngineTypes.h) is NOT persisted — append enum values freely. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
- Work on a feature branch, not main.

## Design decision

**Drive the real callbacks through a thin public seam on `AudioEngine`, not by friending the test or mocking a `juce::AudioIODevice`.**

The audit §8 Phase 0 says: *"expose callbacks for test"* and *"drive the real `CaptureCallback`/`RenderCallback` with synthetic buffers."* Three approaches are possible:

1. **A `juce::AudioIODevice` mock** that returns synthetic in-buffers and starts the real callbacks. *Rejected:* JUCE's device-start path is heavyweight and the callbacks are already constructed and wired in the ctor — a mock device adds a large surface for no extra coverage of the bridge/health wiring we care about.
2. **Friend the test translation unit.** *Rejected:* the private callback structs are defined in `AudioEngine.cpp` and are not visible to a test TU even as a friend; it would force moving the struct definitions into the header.
3. **A public test seam that prepares engine state and forwards to the already-constructed `captureCb`/`renderCb`** (chosen). It exercises the *exact* shipped `audioDeviceIOCallbackWithContext` bodies and the full `hm`/`graph`/`bridge` wiring, with zero production-path change. This matches the audit's "extend, not rewrite" stance (§7).

The seam adds one prepare entry point (`prepareCallbacksForTest`) that mirrors `start()`'s state setup, and two drive methods (`driveCaptureCallback`, `driveRenderCallback`) that call the real callbacks. A `firstRenderUnderrunWasStartupGap`-style helper is **not** added; the seam primes the FIFO exactly as `start()` does so startup behavior matches production.

## File Structure

| File | Create/Modify | Responsibility |
|---|---|---|
| `src/audio/AudioEngine.h` | Modify | Add public test seam: `prepareCallbacksForTest(double,int,int)`, `driveCaptureCallback(const float*,const float*,int)`, `driveRenderCallback(float*,float*,int)` + `Health`/`Levels` already exposed |
| `src/audio/AudioEngine.cpp` | Modify | Implement the seam: prepare `graph`/`bridge`/`hm` like `start()`, size the callbacks' `mono`, forward to `captureCb`/`renderCb` `audioDeviceIOCallbackWithContext` |
| `tests/test_audioengine_callbacks.cpp` | Create | Drive the real capture/render callbacks with synthetic buffers: happy path, input-clean/output-clamp, ring overflow, ring underflow, NaN/Inf → invalidate, channel-drop → Xrun, allocation-free assertion |
| `tests/test_clockbridge_transport.cpp` | Create | GOLDEN sample-accurate cross-clock transport: equal-rate impulse-train index check, 96k→48k count conservation, injected drift held within ASRC tolerance, long soak (no creep-induced retiming reads clean) |
| `tests/CMakeLists.txt` | Modify | Register the two new test sources in the `eb_tests` target |

---

### Task 1: Public test seam to construct & drive the real callbacks

Add a public seam on `AudioEngine` that prepares the same engine state `start()` builds and forwards synthetic buffers into the already-constructed production callbacks. No device, no new thread.

**Files:**
- `src/audio/AudioEngine.h`
- `src/audio/AudioEngine.cpp`
- `tests/test_audioengine_callbacks.cpp` (new)
- `tests/CMakeLists.txt`

**Interfaces:**
- Consumes (existing, from the files read):
  - `void ProcessingGraph::prepare (double sampleRate, int maxBlockSize)`
  - `void ClockBridge::prepare (double captureRate, double renderRate, int channels, int capacityFrames)`; `void primeToTarget()`; `void reset()`
  - `void HealthMonitor::prepare (EarsModel, int fifoCapacityFrames, double nominalRatio = 1.0) noexcept`
  - `struct AudioEngine::CaptureCallback`, `struct AudioEngine::RenderCallback` (private; `std::unique_ptr<>` members `captureCb`, `renderCb` already built in the ctor) with `void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int numSamples, const juce::AudioIODeviceCallbackContext&)`
  - members `std::vector<float> CaptureCallback::mono`, `std::vector<float> RenderCallback::mono`
  - `Health AudioEngine::health() const`; `Levels AudioEngine::levels() const`; `bool cleanCapture() const noexcept`
- Produces (new public API on `AudioEngine`):
  - `void prepareCallbacksForTest (double sampleRate, int blockSize, int fifoCapacity)`
  - `void driveCaptureCallback (const float* inL, const float* inR, int numSamples)`
  - `void driveRenderCallback (float* outL, float* outR, int numSamples)`

Steps:

- [ ] Register the new test source. In `tests/CMakeLists.txt`, add `test_audioengine_callbacks.cpp` to the `eb_tests` source list (immediately after `test_audioengine.cpp`):

```cmake
    test_audioengine.cpp
    test_audioengine_callbacks.cpp
    test_settings.cpp
```

- [ ] Write the failing test `tests/test_audioengine_callbacks.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include <vector>
#include <cmath>

using Catch::Matchers::WithinAbs;

// A unit-impulse FIR makes the per-ear convolution a passthrough so the test can reason about
// the forwarded mono numerically. Settles the async IR load by spinning the capture callback.
static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("AudioEngine callbacks: capture forwards a clean block; render pulls it back") {
    eb::AudioEngine e;
    const int N = 64, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);
    e.setLeftCalFir  (unitImpulse (8));
    e.setRightCalFir (unitImpulse (8));
    e.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f);
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    // Spin the capture callback (async Convolution IR load + gain ramp settle) then pull.
    bool settled = false;
    for (int rep = 0; rep < 3000 && ! settled; ++rep) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
        if (std::abs (outL[N-1] - 0.4f) < 2e-3f) settled = true;   // (0.5+0.3)/2 through the FIFO
        else juce::Thread::sleep (1);
    }
    REQUIRE (settled);
    CHECK (e.cleanCapture());                          // a clean run stays valid
    CHECK (e.health().droppedFrames == 0);
    CHECK_THAT (outR[N-1], WithinAbs (outL[N-1], 1e-6));   // render duplicates mono to L == R
}
```

- [ ] Run it to see it fail to COMPILE (the seam does not exist yet): `./tools/dev.cmd cmake --build build --target eb_tests`. Confirm the error names `prepareCallbacksForTest` / `driveCaptureCallback` / `driveRenderCallback`.

- [ ] Declare the seam in `src/audio/AudioEngine.h`. After the existing headless seam block (the `processCaptureBlockForTest` declaration, ~line 107-108), add:

```cpp
    // ---- R22 callback-level test seam ----
    // Prepares the SAME engine state start() builds (graph + ClockBridge primed to target + HealthMonitor),
    // sizes the production callbacks' scratch, and lets a test drive the REAL CaptureCallback/RenderCallback
    // bodies with synthetic buffers. No device I/O, no new thread. nominalRatio is sampleRate:sampleRate (1.0).
    void prepareCallbacksForTest (double sampleRate, int blockSize, int fifoCapacity);
    void driveCaptureCallback (const float* inL, const float* inR, int numSamples);
    void driveRenderCallback  (float* outL, float* outR, int numSamples);
```

- [ ] Implement the seam in `src/audio/AudioEngine.cpp`. Append after `processCaptureBlockForTest` (after line 357, before the closing `} // namespace eb`):

```cpp
// ---- R22 callback-level test seam -----------------------------------------------------
// Build the same state start() does (without devices), then drive the production callbacks.
void AudioEngine::prepareCallbacksForTest (double sampleRate, int block, int fifoCapacity) {
    activeRate = sampleRate; blockSize = block;
    const int cap = juce::jmax (1024, fifoCapacity);

    graph.prepare (sampleRate, block);
    bridge.prepare (sampleRate, sampleRate, 1, cap);
    hm.prepare (eb::EarsModel::Ears, cap, 1.0);   // nominal capture:render ratio == 1.0 (equal rates)
    bridge.reset();
    bridge.primeToTarget();                       // mirror start(): prime to the PI setpoint, no startup underrun

    // Reset the per-callback diff baselines exactly as start() does (a stale value would suppress
    // underrun detection or inject a spurious dropped-frame delta on the first driven block).
    lastUnderruns_      = 0;
    lastDroppedCapture_ = 0;
    usingAggregate_     = false;                   // exercise the Windows two-device path (real observeRenderBlock)

    // Size the production callbacks' scratch (audioDeviceAboutToStart normally does this from the device).
    captureCb->mono.assign ((size_t) juce::jmax (1, block), 0.0f);
    renderCb->mono.assign  ((size_t) juce::jmax (1, block), 0.0f);

    engineStatus.store ((int) EngineStatus::Running);   // so audioDeviceStopped()-style guards behave as in a run
}

void AudioEngine::driveCaptureCallback (const float* inL, const float* inR, int numSamples) {
    const float* in[2] = { inL, inR };
    juce::AudioIODeviceCallbackContext ctx;
    captureCb->audioDeviceIOCallbackWithContext (in, 2, nullptr, 0, numSamples, ctx);
}

void AudioEngine::driveRenderCallback (float* outL, float* outR, int numSamples) {
    float* out[2] = { outL, outR };
    juce::AudioIODeviceCallbackContext ctx;
    renderCb->audioDeviceIOCallbackWithContext (nullptr, 0, out, 2, numSamples, ctx);
}
```

- [ ] Run it to pass: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "AudioEngine callbacks" --output-on-failure`. Confirm the new test passes and the full suite is unaffected.

- [ ] Commit: `git add -A && git commit` with message `R22: add callback-level test seam to drive the real Capture/Render callbacks` and the required trailer.

---

### Task 2: Confirmed-clip & NaN/Inf invalidation through the REAL capture callback

Prove the shipped capture-callback path (not the helper seam) latches an invalid measurement on a confirmed rail-run and on a non-finite sample — this is the coverage gap the audit flags as R22 "the `0.999`/confirmed-clip path is never run through the callback."

**Files:**
- `tests/test_audioengine_callbacks.cpp`

**Interfaces:**
- Consumes:
  - `void AudioEngine::driveCaptureCallback (const float*, const float*, int)` (Task 1)
  - `Health AudioEngine::health() const` → `Health{ ..., HealthFlag flags, bool cleanCapture }`
  - `bool AudioEngine::cleanCapture() const noexcept`
  - `bool eb::any (HealthFlag) noexcept`; `HealthFlag operator& (HealthFlag,HealthFlag)`
  - `HealthFlag::ClipConfirmed` (rail-run, invalidating); `HealthFlag::NonFinite` (NaN/Inf, invalidating); `HealthMonitor::kRailRunMin == 3`, `kRailCeiling == 0.9999f`
- Produces: test cases only.

Steps:

- [ ] Write the failing tests. Append to `tests/test_audioengine_callbacks.cpp`:

```cpp
TEST_CASE("AudioEngine callbacks: a confirmed rail-run through the real capture callback invalidates") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    // 3 consecutive samples at/above kRailCeiling on L == kRailRunMin -> ClipConfirmed (invalidating).
    std::vector<float> inL { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };
    std::vector<float> inR (inL.size(), 0.0f);
    e.driveCaptureCallback (inL.data(), inR.data(), (int) inL.size());

    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: an isolated single full-scale sample does NOT confirm a clip") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    // A clean full-scale sine touches the rail for ONE sample -> no run -> not a confirmed clip.
    std::vector<float> inL { 0.2f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::vector<float> inR (inL.size(), 0.0f);
    e.driveCaptureCallback (inL.data(), inR.data(), (int) inL.size());

    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a NaN in the raw input invalidates and is not forwarded") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    std::vector<float> inL (N, 0.1f), inR (N, 0.1f);
    inL[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    e.driveCaptureCallback (inL.data(), inR.data(), N);
    e.driveRenderCallback  (outL.data(), outR.data(), N);

    CHECK (eb::any (e.health().flags & eb::HealthFlag::NonFinite));
    CHECK_FALSE (e.cleanCapture());
    for (int i = 0; i < N; ++i) CHECK (std::isfinite (outL[i]));   // no NaN reaches the cable
}
```

- [ ] Add `#include <limits>` at the top of `tests/test_audioengine_callbacks.cpp` (for `quiet_NaN`).

- [ ] Run it to see it fail/pass: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "AudioEngine callbacks" --output-on-failure`. These exercise existing `analyzeInputBlock`/`scanAndFlagNonFinite` behavior through the real callback; if any case fails it indicates the callback wiring differs from the headless seam — investigate before proceeding (do NOT change production behavior in this plan).

- [ ] Commit: `git add -A && git commit` with message `R22: confirmed-clip + NaN/Inf invalidation tests through the real capture callback` and the required trailer.

---

### Task 3: Ring overflow & underflow + channel-drop through the REAL callbacks

Drive the producer/consumer FIFO edges through the shipped callbacks: an over-fed capture (FIFO overrun → `reportDroppedFrames` → `Dropout` → invalidate) and a starved render (underrun edge → `effGot==0` → `observeRenderBlock` → `FifoStarved`+`Dropout` → invalidate), plus the `numIn < 2` channel-drop guard that reports an Xrun. Audit §9 "ring-buffer overflow & underflow; channel-drop → invalidate."

**Files:**
- `tests/test_audioengine_callbacks.cpp`

**Interfaces:**
- Consumes:
  - `void AudioEngine::driveCaptureCallback (const float*, const float*, int)`; `void driveRenderCallback (float*, float*, int)`
  - capture-callback internals exercised: `bridge.pushCapture` overrun → `bridge.droppedCaptureFrames()` delta → `hm.reportDroppedFrames` → `HealthFlag::Dropout`
  - render-callback internals exercised: `bridge.underruns()` edge → `effGot=0` → `hm.observeRenderBlock(numSamples, 0, …)` → `HealthFlag::FifoStarved | Dropout`
  - capture-callback guard `if (numIn < 2 …) { hm.reportXrun(); return; }` → `HealthFlag::Xrun`
  - `Health::droppedFrames` (long long), `bool cleanCapture()`
- Produces: test cases + one tiny private helper to drive the capture callback with `numIn == 1`.

Steps:

- [ ] Add a one-channel drive helper so the test can hit the `numIn < 2` guard without changing the public seam shape. In `src/audio/AudioEngine.h`, extend the seam block with:

```cpp
    // Drive the capture callback with a single input channel to exercise the numIn<2 Xrun guard.
    void driveCaptureCallbackMono (const float* in0, int numSamples);
```

- [ ] Implement it in `src/audio/AudioEngine.cpp`, next to `driveCaptureCallback`:

```cpp
void AudioEngine::driveCaptureCallbackMono (const float* in0, int numSamples) {
    const float* in[1] = { in0 };
    juce::AudioIODeviceCallbackContext ctx;
    captureCb->audioDeviceIOCallbackWithContext (in, 1, nullptr, 0, numSamples, ctx);
}
```

- [ ] Write the failing tests. Append to `tests/test_audioengine_callbacks.cpp`:

```cpp
TEST_CASE("AudioEngine callbacks: over-feeding capture overruns the FIFO and invalidates") {
    eb::AudioEngine e;
    const int N = 1024, cap = 1024;          // each capture block is the whole FIFO; render never drains here
    e.prepareCallbacksForTest (48000.0, N, cap);

    std::vector<float> inL (N, 0.2f), inR (N, 0.2f);
    for (int b = 0; b < 8; ++b) e.driveCaptureCallback (inL.data(), inR.data(), N);   // push, never pull

    CHECK (e.health().droppedFrames > 0);                 // bridge overrun frames surfaced into Health
    CHECK (eb::any (e.health().flags & eb::HealthFlag::Dropout));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a starved render underruns and invalidates") {
    eb::AudioEngine e;
    const int N = 512, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);

    // Drain the primed FIFO without ever feeding capture: ~cap/2 primed / N pulls then it starves.
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);
    bool starved = false;
    for (int b = 0; b < 64 && ! starved; ++b) {
        e.driveRenderCallback (outL.data(), outR.data(), N);
        if (eb::any (e.health().flags & eb::HealthFlag::FifoStarved)) starved = true;
    }
    CHECK (starved);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::Dropout));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a single-channel capture block reports an Xrun") {
    eb::AudioEngine e;
    const int N = 64;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    std::vector<float> in0 (N, 0.1f);

    e.driveCaptureCallbackMono (in0.data(), N);     // numIn == 1 -> the numIn<2 guard fires

    CHECK (eb::any (e.health().flags & eb::HealthFlag::Xrun));
    CHECK_FALSE (e.cleanCapture());
    CHECK (e.health().xruns >= 1);
}
```

- [ ] Run it to pass: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "AudioEngine callbacks" --output-on-failure`.

- [ ] Commit: `git add -A && git commit` with message `R22: FIFO overrun/underrun + channel-drop coverage through the real callbacks` and the required trailer.

---

### Task 4: Allocation/lock-free assertion harness over the steady-state callbacks

Replace the audit's "RT-safety asserted by comment" with a runtime check: install a global `operator new` counter, drive the steady-state capture+render callbacks for many blocks, and assert ZERO heap allocations occur after the prepare/warm-up phase. (Locks/syscalls aren't directly observable in-process, so we assert the strongest cheap invariant — no allocation — which is the one the audit calls out as untested.)

**Files:**
- `tests/test_audioengine_callbacks.cpp`

**Interfaces:**
- Consumes:
  - `void AudioEngine::driveCaptureCallback (const float*, const float*, int)`; `void driveRenderCallback (float*, float*, int)`
  - the steady-state callback bodies (must be alloc-free per the RT-safety constraint)
- Produces:
  - a translation-unit-local global-`new`/`delete` override guarded by an `std::atomic<bool> g_countAllocs` so it only counts inside the measured region (other test TUs are unaffected — Catch2 links one binary, so the override is process-wide; keep it inert unless armed).

Steps:

- [ ] Write the failing test. Append to `tests/test_audioengine_callbacks.cpp` (the global override MUST sit at file scope, before the test cases that use it):

```cpp
// --- Allocation counter: armed only inside the measured region, so it never perturbs other TUs. ---
namespace {
    std::atomic<bool>     g_countAllocs { false };
    std::atomic<long long> g_allocCount { 0 };
}
void* operator new (std::size_t sz) {
    if (g_countAllocs.load (std::memory_order_relaxed)) g_allocCount.fetch_add (1, std::memory_order_relaxed);
    if (auto* p = std::malloc (sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

TEST_CASE("AudioEngine callbacks: steady-state capture+render are allocation-free") {
    eb::AudioEngine e;
    const int N = 256, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);
    e.setLeftCalFir  (unitImpulse (8));
    e.setRightCalFir (unitImpulse (8));
    e.setCombineMode (eb::CombineMode::AutoPerEar);

    std::vector<float> inL (N, 0.25f), inR (N, 0.25f);
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    // Warm-up OUTSIDE the measured region: settle the async Convolution IR load / gain ramp.
    for (int b = 0; b < 256; ++b) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
        juce::Thread::sleep (1);
    }

    g_allocCount.store (0);
    g_countAllocs.store (true);
    for (int b = 0; b < 500; ++b) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
    }
    g_countAllocs.store (false);

    INFO ("alloc count in 500 capture+render blocks = " << g_allocCount.load());
    CHECK (g_allocCount.load() == 0);
}
```

- [ ] Add `#include <cstdlib>` and `#include <new>` to the includes at the top of the file (for `std::malloc`/`std::free`/`std::bad_alloc`).

- [ ] Run it to pass: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "allocation-free" --output-on-failure`. If a nonzero count appears, the INFO line localizes it; an allocation in the steady-state callbacks is a real RT-safety defect — file it, do not silence the test.

- [ ] Run the full suite once to confirm the global `operator new` override hasn't disturbed other tests: `./tools/dev.cmd ctest --test-dir build --output-on-failure`.

- [ ] Commit: `git add -A && git commit` with message `R22: assert the steady-state capture+render callbacks are allocation-free` and the required trailer.

---

### Task 5: GOLDEN sample-accurate cross-clock transport through ClockBridge

Prove the ClockBridge moves samples across the two clock domains with no dropped/duplicated/reordered content within the async-SRC tolerance — the audit's "golden sample-accurate transport test across the two clock domains" (§9 Clock/transport). Uses a marker-impulse train (not a sine) so a dropped/duplicated/reordered sample shows as a shifted or missing impulse, and an equal-rate count-conservation check.

**Files:**
- `tests/test_clockbridge_transport.cpp` (new)
- `tests/CMakeLists.txt`

**Interfaces:**
- Consumes:
  - `void ClockBridge::prepare (double captureRate, double renderRate, int channels, int capacityFrames)`; `void primeToTarget()`; `void pushCapture (const float*, int)`; `int pullRender (float*, int)`; `int underruns()`; `int overruns()`; `double currentRatio()`
  - `HealthMonitor::kDriftRatioTol == 0.005`
- Produces: test cases + one local balanced-feed driver (capture pushed to match consumption so the FIFO neither overruns nor starves).

Steps:

- [ ] Register the new test source. In `tests/CMakeLists.txt`, add `test_clockbridge_transport.cpp` to the `eb_tests` source list (immediately after `test_clockbridge.cpp`):

```cmake
    test_clockbridge.cpp
    test_clockbridge_transport.cpp
    test_healthmonitor.cpp
```

- [ ] Write the failing test `tests/test_clockbridge_transport.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/ClockBridge.h"
#include "audio/HealthMonitor.h"
#include <vector>
#include <cmath>

// Balanced driver: push capture to track consumption so the FIFO hovers near its primed fill
// (no overrun, no starvation), then collect the rendered output. capSampleFn(i) supplies capture
// sample i; the renderer reads renderBlocks blocks of renderBlock frames. Returns the rendered
// stream plus the bridge's under/overrun tallies.
static std::vector<float> runTransport (eb::ClockBridge& cb, double capRate, double renRate,
                                        int capBlock, int renderBlock, int renderBlocks,
                                        const std::function<float(long long)>& capSampleFn,
                                        int& underruns, int& overruns) {
    const double capPerRender = capRate / renRate;
    cb.primeToTarget();

    long long capPushed = 0;
    std::vector<float> capBuf ((size_t) capBlock);
    auto pushOne = [&]() {
        for (int i = 0; i < capBlock; ++i) capBuf[(size_t) i] = capSampleFn (capPushed + i);
        cb.pushCapture (capBuf.data(), capBlock);
        capPushed += capBlock;
    };
    // Prime the producer to the same depth primeToTarget() seeded (so indices line up downstream).
    const long long primed = (long long) std::llround (eb::ClockBridge::kTargetFill * 8192.0);
    for (long long pushed = 0; pushed < primed; ) { pushOne(); pushed += capBlock; }

    std::vector<float> out ((size_t) renderBlock, 0.0f), rendered;
    rendered.reserve ((size_t) renderBlocks * renderBlock);
    long long renderDone = 0;
    for (int b = 0; b < renderBlocks; ++b) {
        const long long target = (long long) ((double) (renderDone + renderBlock) * capPerRender) + primed;
        while (capPushed < target) pushOne();
        cb.pullRender (out.data(), renderBlock);
        renderDone += renderBlock;
        for (int i = 0; i < renderBlock; ++i) rendered.push_back (out[(size_t) i]);
    }
    underruns = cb.underruns(); overruns = cb.overruns();
    return rendered;
}

TEST_CASE("ClockBridge transport: equal-rate impulse train arrives in order with no drop/dup") {
    // Marker impulses every `period` capture samples. At 1:1 the rendered stream must contain the
    // SAME number of impulses in monotonically increasing positions (no reorder), spaced ~period
    // apart (no drop = no gap of ~2*period; no dup = no two impulses < period/2 apart).
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    const int period = 480;   // 100 impulses/sec at 48k
    auto capSample = [period] (long long i) { return (i % period == 0) ? 1.0f : 0.0f; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 48000.0, 48000.0, 256, 256, 400, capSample, u, o);
    REQUIRE (u == 0); REQUIRE (o == 0);

    // Collect impulse positions (Lagrange smears one capture impulse to a small local cluster; take
    // the local-max index as the marker, with a refractory gap so the smear is not double-counted).
    std::vector<int> pos;
    const int n = (int) r.size();
    for (int i = 1; i < n - 1; ++i)
        if (r[(size_t) i] > 0.5f && r[(size_t) i] >= r[(size_t) i-1] && r[(size_t) i] > r[(size_t) i+1])
            if (pos.empty() || (i - pos.back()) > period / 2) pos.push_back (i);

    REQUIRE (pos.size() >= 4);
    for (size_t k = 1; k < pos.size(); ++k) {
        const int gap = pos[k] - pos[k-1];
        CHECK (gap > period * 3 / 4);       // no dropped impulse (would ~double the gap)
        CHECK (gap < period * 5 / 4);       // no duplicated impulse (would ~halve the gap)
    }
}

TEST_CASE("ClockBridge transport: 96k->48k conserves the impulse count (2:1 decimation)") {
    // Every `period` capture samples carries an impulse; at 96k->48k the rendered (48k) stream is
    // half as long, so it must carry ~half as many impulses (count conservation across the ratio).
    eb::ClockBridge cb; cb.prepare (96000.0, 48000.0, 1, 16384);
    const int capPeriod = 960;   // 100 impulses/sec at 96k
    auto capSample = [capPeriod] (long long i) { return (i % capPeriod == 0) ? 1.0f : 0.0f; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 96000.0, 48000.0, 256, 256, 400, capSample, u, o);
    REQUIRE (u == 0); REQUIRE (o == 0);

    int impulses = 0; const int n = (int) r.size();
    for (int i = 1; i < n - 1; ++i)
        if (r[(size_t) i] > 0.5f && r[(size_t) i] >= r[(size_t) i-1] && r[(size_t) i] > r[(size_t) i+1])
            ++impulses;
    // 400 render blocks * 256 = 102400 render samples / (48000/100) = ~213 impulses expected.
    const int rendered = 400 * 256;
    const int expected = rendered / (48000 / 100);
    INFO ("impulses=" << impulses << " expected~=" << expected);
    CHECK (std::abs (impulses - expected) <= 3);   // count conserved within rounding of the warm-up edge
}
```

- [ ] Add `#include <functional>` to the includes (for `std::function`).

- [ ] Run it to pass: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "ClockBridge transport" --output-on-failure`.

- [ ] Commit: `git add -A && git commit` with message `R22: golden sample-accurate cross-clock transport tests for ClockBridge` and the required trailer.

---

### Task 6: Injected clock drift held within ASRC tolerance + long-soak no-creep

Cover the audit's "injected ±0.5–3% drift … long soak (no creep-induced retiming passes as clean)" (§9 Clock/transport). Feed the bridge at a rate offset from its nominal and assert (a) the PI loop holds the FIFO bounded with no under/overrun, (b) the published ratio stays within `kDriftRatioTol` for a small offset, and (c) over a long soak the transport stays transparent (steady amplitude, no clicks) — i.e. continuous sub-tolerance creep does not corrupt the stream.

**Files:**
- `tests/test_clockbridge_transport.cpp`

**Interfaces:**
- Consumes:
  - `runTransport(...)` (Task 5) and the balanced-feed pattern
  - `int ClockBridge::underruns()`; `int overruns()`; `double currentRatio()`; `double fifoFill()`
  - `HealthMonitor::kDriftRatioTol == 0.005`
- Produces: test cases.

Steps:

- [ ] Write the failing tests. Append to `tests/test_clockbridge_transport.cpp`:

```cpp
// Transparency metric reused from the transport runs: frequency/phase-invariant resonator-residual
// (a clean unit-amplitude tone obeys r[i+1]+r[i-1] == 2cos(w)*r[i]); returns max(|peak-1|, residual/peak).
static double toneCleanliness (const std::vector<float>& r, int warm) {
    const int n = (int) r.size();
    double peak = 0.0;
    for (int i = warm; i < n; ++i) peak = std::max (peak, std::abs ((double) r[(size_t) i]));
    double num = 0.0, den = 0.0;
    for (int i = warm + 1; i < n - 1; ++i) {
        const double ri = (double) r[(size_t) i];
        num += ((double) r[(size_t) i+1] + (double) r[(size_t) i-1]) * ri;
        den += ri * ri;
    }
    const double k = (den > 1.0e-12) ? num / den : 0.0;
    double resid = 0.0;
    for (int i = warm + 1; i < n - 1; ++i)
        resid = std::max (resid, std::abs ((double) r[(size_t) i+1] + (double) r[(size_t) i-1] - k * (double) r[(size_t) i]));
    return std::max (std::abs (peak - 1.0), resid / std::max (peak, 1.0e-6));
}

TEST_CASE("ClockBridge transport: a small injected drift stays within the ASRC drift tolerance") {
    // Bridge nominal = 1:1, but the producer is FED at 48000/48010 (~0.02% slow). The PI loop must
    // trim to hold the FIFO; over a long run the published ratio stays within kDriftRatioTol and the
    // FIFO never runs away (no under/overrun) -- a real sub-tolerance drift the loop absorbs cleanly.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    const double f0 = 997.0, fed = 48010.0;
    double ph = 0.0; const double d = 2.0 * juce::MathConstants<double>::pi * f0 / fed;
    auto capSample = [&] (long long) { const float s = (float) std::sin (ph); ph += d; return s; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 48010.0, 48000.0, 256, 256, 800, capSample, u, o);
    INFO ("ratio=" << cb.currentRatio() << " fill=" << cb.fifoFill() << " u=" << u << " o=" << o);
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (cb.fifoFill() > 0.05);
    CHECK (cb.fifoFill() < 0.95);
    CHECK (std::abs (cb.currentRatio() - 1.0) < eb::HealthMonitor::kDriftRatioTol);
}

TEST_CASE("ClockBridge transport: long soak -- sub-tolerance creep does not corrupt the stream") {
    // 4000 render blocks of 256 = ~21 s at 48k. With a ~0.02% feed offset the PI loop continuously
    // micro-trims the ratio; the audit's failure mode is that this continuous retiming smears the
    // stream while staying under the drift latch. Assert the rendered tone stays transparent
    // (steady amplitude, no clicks) over the whole soak -- i.e. creep does NOT silently corrupt it.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    const double f0 = 997.0, fed = 48008.0;
    double ph = 0.0; const double d = 2.0 * juce::MathConstants<double>::pi * f0 / fed;
    auto capSample = [&] (long long) { const float s = (float) std::sin (ph); ph += d; return s; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 48008.0, 48000.0, 256, 256, 4000, capSample, u, o);
    const double clean = toneCleanliness (r, juce::jmin ((int) r.size() / 2, 24 * 256));
    INFO ("soak cleanliness=" << clean << " u=" << u << " o=" << o << " fill=" << cb.fifoFill());
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (clean < 0.05);     // transparent across the full soak: no creep-induced retiming corruption
}
```

- [ ] Run it to pass (the soak is longer — allow the default 2-min build/test budget): `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build -R "ClockBridge transport" --output-on-failure`.

- [ ] Run the FULL suite to confirm everything stays green and the count grew by the new cases: `./tools/dev.cmd ctest --test-dir build --output-on-failure`.

- [ ] Commit: `git add -A && git commit` with message `R22: injected-drift tolerance + long-soak no-creep transport tests` and the required trailer.

---

## Self-review

**Spec coverage (maps to audit R22 + §9 test plan):**
- *"add a minimal test seam to construct and DRIVE the real CaptureCallback/RenderCallback with synthetic buffers"* → Task 1 (`prepareCallbacksForTest` + `driveCaptureCallback`/`driveRenderCallback` forwarding into the already-constructed `captureCb`/`renderCb`), exercising the exact `audioDeviceIOCallbackWithContext` bodies at `AudioEngine.cpp:27-49` and `:63-90`.
- *"assert no-alloc/no-lock where feasible"* → Task 4 (armed global-`new` counter over 500 steady-state blocks; locks/syscalls aren't in-process observable, so allocation — the invariant the audit names as untested — is asserted, with the limitation stated honestly).
- *"a GOLDEN sample-accurate cross-clock transport test through ClockBridge … no dropped/duplicated/reordered samples within ASRC tolerance"* → Task 5 (impulse-train order + gap check at 1:1; impulse-count conservation at 96k→48k).
- *"device-reconnection / format-mismatch / clock-drift / long-soak"* → channel-drop/format-class via the `numIn<2` Xrun guard and FIFO overrun/underrun (Task 3); clock-drift within tolerance and long-soak no-creep (Task 6). Note: live OS device re-enumeration (`audioDeviceStopped` → `deviceDied_`) is a device-thread event with no headless seam; it is covered at the unit level by the existing device-loss wiring and is out of scope for a no-device harness — called out rather than faked.
- *"Each test case is its own task or step."* → every case is its own checkbox step within a task; 6 tasks total.

**Placeholder scan:** no `TBD`, no "add error handling", no "handle edge cases", no "similar to Task N". Every test body and every production-side seam method is shown in full with real types and real constants (`kRailRunMin`, `kRailCeiling`, `kDriftRatioTol`, `kTargetFill`).

**Type consistency (against the files read):**
- `HealthFlag` values used (`ClipConfirmed`, `NonFinite`, `Dropout`, `FifoStarved`, `Xrun`) all exist in `EngineTypes.h:15-26`; `eb::any` / `operator&` are the real free functions (`:30-33`).
- `Health{ int xruns; long long droppedFrames; double fifoFill; bool cleanCapture; double captureToRenderRatio; HealthFlag flags }` matches `EngineTypes.h:35-42`; the tests read `.flags`, `.droppedFrames`, `.xruns` exactly.
- `ClockBridge::prepare/primeToTarget/pushCapture/pullRender/underruns/overruns/currentRatio/fifoFill` and `kTargetFill` match `ClockBridge.h:19-39`; `HealthMonitor::kDriftRatioTol` matches `HealthMonitor.h:32`.
- The seam mirrors `start()`'s real setup order (`graph.prepare` → `bridge.prepare`/`reset`/`primeToTarget` → `hm.prepare` → baseline reset, `AudioEngine.cpp:278-309`) and sizes the callbacks' `mono` the way `audioDeviceAboutToStart` does (`AudioEngine.cpp:19,58`), so the driven path is behavior-identical to a real run minus the device.

**RT-safety:** the plan adds NO code reachable from a real audio callback — the seam methods run only from tests; the production callback bodies are unchanged. The allocation test enforces the existing RT-safety contract rather than relaxing it.
