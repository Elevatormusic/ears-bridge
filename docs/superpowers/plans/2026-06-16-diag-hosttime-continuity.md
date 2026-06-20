# Callback-Timing (hostTime) Continuity Diagnostic Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Track the `juce::AudioIODeviceCallbackContext::hostTimeNs` pointer (a `const uint64_t*`, nullable) across consecutive callback invocations on both the capture and render callbacks. When the pointer is present, compute the block-to-block delta and compare it against the expected interval (`numSamples / sampleRate` in nanoseconds). Flag a discontinuity — a jump that deviates from the expected interval by more than a configurable tolerance — with `HealthFlag::HostTimeDiscontinuity`. The flag is **invalidating** (see Design decision below) and surfaced through `ClipStatus.h::invalidMeasurementMessage`.

**Architecture:** One new `HealthFlag` value, one small per-callback method on `HealthMonitor` (`observeHostTime`), two call-sites (one in each of `CaptureCallback::audioDeviceIOCallbackWithContext` and `RenderCallback::audioDeviceIOCallbackWithContext`), one new message branch in `invalidMeasurementMessage`, and one new test file added to the `eb_tests` CMake target. No new threads, no allocations, no locks.

**Design decision — invalidating vs guidance:** The audit (§6 optional diagnostic) leaves this open; the choice made here is **invalidating**. A hard host-clock jump during a Dirac log-sweep is indistinguishable from a dropped or duplicated block from the perspective of the FIFO-drift path: the ASRC sees a sudden fill-level discontinuity, adjusts its ratio, and potentially smears or retimes the swept impulse. This is the same class of integrity failure as `ExcessDrift` and `FifoStarved`, both of which already invalidate. The alternative (guidance-only) would require proving that every host-time jump leaves the audio path unaffected — impossible to assert for a WASAPI shared-mode path where the host time is the OS scheduling timestamp. The per-callback nature means a single erroneous flag from an unusual first callback is possible; the implementation therefore skips the very first block (no `prev` value yet) and requires a deviation to exceed `kHostTimeTolFrac` (5 %) before flagging, which filters scheduling jitter while catching real discontinuities.

**Tech Stack:** C++17, JUCE 8.0.4, Catch2 (existing suite at `tests/`), MSVC + Ninja via `tools/dev.cmd`.

---

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests`  — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never cmd /c; it wraps Ninja + MSVC.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure` . Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + std::atomic only.
- HealthFlag (src/audio/EngineTypes.h) is NOT persisted — append enum values freely. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `src/audio/EngineTypes.h` | Modify | Add `HostTimeDiscontinuity = 1u << 9` to `HealthFlag` |
| `src/audio/HealthMonitor.h` | Modify | Declare `observeHostTime(const uint64_t* hostTimeNs, int numSamples, double sampleRate, bool isCapture)` + per-callback scratch members |
| `src/audio/HealthMonitor.cpp` | Modify | Implement `observeHostTime` (RT-safe; nullable pointer guard; first-block skip; tolerance check; call `raise`) |
| `src/audio/AudioEngine.cpp` | Modify | Forward `context.hostTimeNs` to `hm.observeHostTime(...)` at the top of `CaptureCallback` and `RenderCallback` |
| `src/gui/ClipStatus.h` | Modify | Add a `HostTimeDiscontinuity` branch in `invalidMeasurementMessage` |
| `tests/test_hosttime.cpp` | Create | Catch2 tests: null pointer is ignored, nominal delta is not flagged, large jump flags and invalidates, first-block skipped, tolerance boundary |
| `tests/CMakeLists.txt` | Modify | Add `test_hosttime.cpp` to the `eb_tests` source list |

---

## Task 1: Add `HostTimeDiscontinuity` to `HealthFlag` and wire `ClipStatus.h`

**Files:**
- `src/audio/EngineTypes.h`
- `src/gui/ClipStatus.h`
- `tests/test_clipstatus.cpp`

**Interfaces:**
- Consumes: existing `HealthFlag` enum (currently highest bit = `NonFinite = 1u << 8`, `EngineTypes.h:26`)
- Produces: `HealthFlag::HostTimeDiscontinuity = 1u << 9`; `invalidMeasurementMessage` returns a new string for that flag

### Steps

- [ ] Write the failing test (add a new `TEST_CASE` to the **existing** `tests/test_clipstatus.cpp` — do not create a new file for this step):

```cpp
// Append to tests/test_clipstatus.cpp

TEST_CASE("invalidMeasurementMessage names HostTimeDiscontinuity") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::HostTimeDiscontinuity))
           == "A host-clock discontinuity was detected - this measurement is invalid.");
    // ClipConfirmed still takes precedence over HostTimeDiscontinuity.
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::HostTimeDiscontinuity))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
}
```

- [ ] Run `./tools/dev.cmd ctest --test-dir build -R "invalidMeasurementMessage" --output-on-failure` and confirm it fails to compile (undefined `HostTimeDiscontinuity`).

- [ ] Add the enum value to `src/audio/EngineTypes.h` immediately after line 26 (`NonFinite = 1u << 8`):

```cpp
    NonFinite               = 1u << 8,  // INVALIDATING: a NaN/Inf sample reached the path
    HostTimeDiscontinuity   = 1u << 9   // INVALIDATING: host-clock jump exceeds expected block interval
```

- [ ] Add the branch to `src/gui/ClipStatus.h` — insert it between the `NonFinite` and the fallback `return` (after line 12, before line 13):

```cpp
    if (any (flags & HealthFlag::HostTimeDiscontinuity))
        return "A host-clock discontinuity was detected - this measurement is invalid.";
```

  The full updated function body becomes:

```cpp
[[nodiscard]] inline const char* invalidMeasurementMessage (HealthFlag flags) noexcept {
    if (any (flags & HealthFlag::ClipConfirmed))
        return "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.";
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    if (any (flags & HealthFlag::HostTimeDiscontinuity))
        return "A host-clock discontinuity was detected - this measurement is invalid.";
    return "Dropouts detected - this measurement is invalid.";
}
```

- [ ] Run `./tools/dev.cmd cmake --build build --target eb_tests` and confirm it compiles.
- [ ] Run `./tools/dev.cmd ctest --test-dir build -R "invalidMeasurementMessage" --output-on-failure` and confirm it passes.
- [ ] Run the full suite `./tools/dev.cmd ctest --test-dir build --output-on-failure` and confirm all 98+ tests pass.
- [ ] Commit on the feature branch:

```
git checkout -b diag-hosttime-continuity
git add src/audio/EngineTypes.h src/gui/ClipStatus.h tests/test_clipstatus.cpp
git commit -m "$(cat <<'EOF'
feat: add HostTimeDiscontinuity HealthFlag and invalidMeasurementMessage branch

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Implement `HealthMonitor::observeHostTime`

**Files:**
- `src/audio/HealthMonitor.h`
- `src/audio/HealthMonitor.cpp`

**Interfaces:**

Consumes:
- `HealthFlag::HostTimeDiscontinuity` (Task 1)
- `HealthMonitor::raise(HealthFlag)` (existing, `HealthMonitor.cpp:8`)
- `HealthMonitor::reset()` (existing, must clear new scratch members)

Produces:
```cpp
// In HealthMonitor (public):
static constexpr double kHostTimeTolFrac = 0.05;  // 5 % of expected block interval

// RT-safe; called from both capture and render callbacks.
// hostTimeNs: pointer from juce::AudioIODeviceCallbackContext::hostTimeNs (may be nullptr).
// numSamples: callback block size.
// sampleRate: the device's current sample rate (passed by caller; no virtual call on the RT thread).
// isCapture: true for the capture callback, false for the render callback (keeps the two
//            callbacks' prev-timestamp tracking independent — they run on different threads).
void observeHostTime (const uint64_t* hostTimeNs, int numSamples,
                      double sampleRate, bool isCapture) noexcept;
```

Private members added to `HealthMonitor` (RT-safe: written only from one thread per `isCapture` value, never shared):
```cpp
// Per-callback "previous hostTimeNs" scratch — written only on the audio thread for that
// callback (capture or render); not atomic because each is single-writer. The "valid" bool
// is also single-writer-per-thread and used to skip the very first block.
uint64_t prevHostNsCapture_ = 0;  bool prevCapValid_ = false;
uint64_t prevHostNsRender_  = 0;  bool prevRendValid_ = false;
```

### Steps

- [ ] Create `tests/test_hosttime.cpp` with all test cases (this is the new file; do NOT add it to CMakeLists yet — that is Task 3):

```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/HealthMonitor.h"
#include <cstdint>

// Helper: call observeHostTime with a real pointer value for convenience.
static void callWithTime (eb::HealthMonitor& hm, uint64_t ns, int numSamples,
                          double sr, bool isCap) {
    hm.observeHostTime (&ns, numSamples, sr, isCap);
}

TEST_CASE ("HostTime: null hostTimeNs is ignored; no flag raised") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    // A null context pointer (backend does not supply hostTime) must be a no-op.
    hm.observeHostTime (nullptr, 512, 48000.0, true);
    hm.observeHostTime (nullptr, 512, 48000.0, true);
    CHECK_FALSE (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK (hm.cleanCapture());
}

TEST_CASE ("HostTime: first block with valid pointer is stored but not evaluated") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    // First block: no previous value -> skip; no flag.
    callWithTime (hm, 1'000'000'000ULL, 512, 48000.0, true);
    CHECK_FALSE (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK (hm.cleanCapture());
}

TEST_CASE ("HostTime: a nominal back-to-back block interval does NOT raise a flag") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    // At 48 kHz, 512 samples = 10 666 667 ns (10.667 ms).
    const int    N  = 512;
    const double SR = 48000.0;
    const uint64_t expectedNs = static_cast<uint64_t> (N / SR * 1e9);   // 10 666 666 ns
    uint64_t t = 1'000'000'000ULL;
    callWithTime (hm, t, N, SR, true);            // first block (skip)
    t += expectedNs;
    callWithTime (hm, t, N, SR, true);            // second block: delta == expected
    CHECK_FALSE (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK (hm.cleanCapture());
}

TEST_CASE ("HostTime: a large forward jump (2x expected interval) flags and invalidates") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    const int    N  = 512;
    const double SR = 48000.0;
    const uint64_t expectedNs = static_cast<uint64_t> (N / SR * 1e9);
    uint64_t t = 1'000'000'000ULL;
    callWithTime (hm, t, N, SR, true);            // first block (skip)
    t += expectedNs * 2;                          // 2x gap: a missed callback's worth
    callWithTime (hm, t, N, SR, true);
    CHECK (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK_FALSE (hm.cleanCapture());
}

TEST_CASE ("HostTime: a large backward jump (clock reset) flags and invalidates") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    const int    N  = 512;
    const double SR = 48000.0;
    const uint64_t expectedNs = static_cast<uint64_t> (N / SR * 1e9);
    uint64_t t = 1'000'000'000ULL;
    callWithTime (hm, t, N, SR, true);            // first block (skip)
    // Subtract more than one expected interval -> wrap -> huge unsigned delta.
    // Saturate: if delta > 10 * expected we treat that as a discontinuity.
    uint64_t tBack = t - expectedNs * 2;          // underflows to a huge uint64 value
    callWithTime (hm, tBack, N, SR, true);
    CHECK (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK_FALSE (hm.cleanCapture());
}

TEST_CASE ("HostTime: a delta within the 5% tolerance band does NOT flag") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    const int    N  = 512;
    const double SR = 48000.0;
    const uint64_t expectedNs = static_cast<uint64_t> (N / SR * 1e9);
    // Add 4% jitter (< kHostTimeTolFrac = 5%) -> must stay clean.
    const uint64_t jitter = static_cast<uint64_t> (expectedNs * 0.04);
    uint64_t t = 1'000'000'000ULL;
    callWithTime (hm, t, N, SR, true);
    t += expectedNs + jitter;
    callWithTime (hm, t, N, SR, true);
    CHECK_FALSE (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK (hm.cleanCapture());
}

TEST_CASE ("HostTime: capture and render callbacks track independently") {
    // A discontinuity on the capture thread must NOT affect the render prev-ts state,
    // and vice versa.
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    const int    N  = 256;
    const double SR = 48000.0;
    const uint64_t expectedNs = static_cast<uint64_t> (N / SR * 1e9);

    // Establish render-thread baseline (two nominal blocks on isCap=false).
    uint64_t tr = 2'000'000'000ULL;
    callWithTime (hm, tr, N, SR, false);           // first render block (skip)
    tr += expectedNs;
    callWithTime (hm, tr, N, SR, false);           // nominal -> no flag

    // Inject a capture-thread discontinuity.
    uint64_t tc = 1'000'000'000ULL;
    callWithTime (hm, tc, N, SR, true);            // first capture block (skip)
    tc += expectedNs * 3;                          // 3x gap -> flag
    callWithTime (hm, tc, N, SR, true);
    CHECK (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));

    // Reset and confirm the render prev-ts is gone (a new first render block skips check).
    hm.reset();
    tr += expectedNs * 10;                         // big gap, but first block post-reset -> skip
    callWithTime (hm, tr, N, SR, false);
    CHECK_FALSE (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK (hm.cleanCapture());
}

TEST_CASE ("HostTime: reset clears the prev-timestamp state so the next first block skips") {
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 4096);
    const int    N  = 512;
    const double SR = 48000.0;
    const uint64_t expectedNs = static_cast<uint64_t> (N / SR * 1e9);

    uint64_t t = 5'000'000'000ULL;
    callWithTime (hm, t, N, SR, true);             // first block: stored
    t += expectedNs;
    callWithTime (hm, t, N, SR, true);             // nominal: no flag

    hm.reset();                                    // clears prevCapValid_

    // After reset the next block is treated as "first" again, regardless of the gap.
    t += expectedNs * 100;
    callWithTime (hm, t, N, SR, true);             // first block post-reset: skip
    CHECK_FALSE (eb::any (hm.flags() & eb::HealthFlag::HostTimeDiscontinuity));
    CHECK (hm.cleanCapture());
}
```

- [ ] Run `./tools/dev.cmd ctest --test-dir build -R "HostTime" --output-on-failure` and confirm it fails to compile (missing `observeHostTime` declaration).

- [ ] Add the public declaration and the private scratch members to `src/audio/HealthMonitor.h`.

  In the public section, after the `bool reachedGoodLevel() const noexcept` line (line 79), add:

```cpp
    // ---- Callback-timing (hostTime) continuity diagnostic (audit §6 optional) ----
    // Called at the top of each audio callback. RT-safe: plain locals + two separate
    // non-atomic scalar members (each written only by the one callback thread that owns it).
    // hostTimeNs: the context pointer — may be nullptr when the backend does not supply it.
    // sampleRate: the device rate, forwarded from audioDeviceAboutToStart (avoids a virtual
    //             call on the audio thread).
    // isCapture: true for CaptureCallback, false for RenderCallback (each has its own prev-ts).
    static constexpr double kHostTimeTolFrac = 0.05;  // 5 % of expected block interval

    void observeHostTime (const uint64_t* hostTimeNs, int numSamples,
                          double sampleRate, bool isCapture) noexcept;
```

  In the private section, after the `std::atomic<int> driftRun` line (line 108), add:

```cpp
    // Per-callback host-time scratch (single-writer per thread; not atomic by design).
    uint64_t prevHostNsCapture_ = 0;  bool prevCapValid_  = false;
    uint64_t prevHostNsRender_  = 0;  bool prevRendValid_ = false;
    // Sample-rate snapshot per callback — set when observeHostTime is first called after
    // the callback's audioDeviceAboutToStart fires. Stored as uint64_t to allow arithmetic
    // with the nanosecond timestamps without casts on the hot path.
    // We store the expected interval in ns directly for efficiency.
    uint64_t capExpectedIntervalNs_  = 0;
    uint64_t rendExpectedIntervalNs_ = 0;
```

  **Note:** The expected interval is calculated once per block from `numSamples` and `sampleRate` rather than cached, because the block size can vary in theory; the computation is a single divide and multiply — no allocation.

- [ ] Implement `observeHostTime` in `src/audio/HealthMonitor.cpp`. Add it after `HealthMonitor::cleanCapture()` (after line 188):

```cpp
void HealthMonitor::observeHostTime (const uint64_t* hostTimeNs,
                                     int numSamples,
                                     double sampleRate,
                                     bool isCapture) noexcept {
    // nullptr means the backend does not supply host time; treat as absent, not zero.
    if (hostTimeNs == nullptr) return;
    if (sampleRate <= 0.0 || numSamples <= 0) return;

    const uint64_t now = *hostTimeNs;
    // Expected block interval in nanoseconds (integer; the truncation is < 1 ns, negligible).
    const uint64_t expected = static_cast<uint64_t> (
        static_cast<double> (numSamples) / sampleRate * 1.0e9);

    uint64_t& prevNs   = isCapture ? prevHostNsCapture_ : prevHostNsRender_;
    bool&     prevValid = isCapture ? prevCapValid_      : prevRendValid_;

    if (! prevValid) {
        // First block on this callback since the last reset() — store and skip the check.
        prevNs    = now;
        prevValid = true;
        return;
    }

    // Unsigned subtraction: if the clock wrapped/jumped backward, delta will be huge.
    const uint64_t delta = now - prevNs;
    prevNs = now;

    // Compute deviation as a fraction of the expected interval.
    // Cast to double to avoid integer overflow in intermediate multiplication.
    // |delta - expected| / expected — if expected is 0 (pathological rate), skip.
    if (expected == 0) return;

    const double deviation = (delta >= expected)
        ? static_cast<double> (delta - expected) / static_cast<double> (expected)
        : static_cast<double> (expected - delta) / static_cast<double> (expected);

    if (deviation > kHostTimeTolFrac)
        raise (HealthFlag::HostTimeDiscontinuity);
}
```

- [ ] Also clear the new scratch members in `HealthMonitor::reset()`. After the existing `driftRun.store(0); blockCount.store(0);` line (line 43 of HealthMonitor.cpp), add:

```cpp
    prevHostNsCapture_ = 0; prevCapValid_  = false;
    prevHostNsRender_  = 0; prevRendValid_ = false;
```

- [ ] Run `./tools/dev.cmd cmake --build build --target eb_tests`.
- [ ] Run `./tools/dev.cmd ctest --test-dir build -R "HostTime" --output-on-failure` (tests still not linked yet — this confirms test is visible).
- [ ] Commit (do not add test_hosttime.cpp to CMake yet; that is Task 3):

```
git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp
git commit -m "$(cat <<'EOF'
feat: implement HealthMonitor::observeHostTime for host-clock continuity diagnostic

RT-safe, nullable-pointer guard, independent capture/render prev-ts, 5% tolerance.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Wire the test file into CMake and make the suite green

**Files:**
- `tests/CMakeLists.txt`
- `tests/test_hosttime.cpp` (created in Task 2)

**Interfaces:**
- Consumes: `test_hosttime.cpp` (Task 2), `eb_tests` CMake target (existing)
- Produces: `HostTime` test cases discovered by `catch_discover_tests`

### Steps

- [ ] Add `test_hosttime.cpp` to the `eb_tests` source list in `tests/CMakeLists.txt` immediately after the `test_clipstatus.cpp` line:

```cmake
    test_clipstatus.cpp
    test_hosttime.cpp)
```

- [ ] Run `./tools/dev.cmd cmake --build build --target eb_tests`.
- [ ] Run `./tools/dev.cmd ctest --test-dir build -R "HostTime" --output-on-failure` and confirm all 7 host-time tests pass.
- [ ] Run the full suite `./tools/dev.cmd ctest --test-dir build --output-on-failure` and confirm all tests pass (≥ 105 total: 98 prior + 7 new + 2 from Task 1 extension = 107 expected).
- [ ] Commit:

```
git add tests/CMakeLists.txt tests/test_hosttime.cpp
git commit -m "$(cat <<'EOF'
test: add 7 host-time continuity tests (null, nominal, jump, tolerance, independence, reset)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Forward `context.hostTimeNs` in `CaptureCallback` and `RenderCallback`

**Files:**
- `src/audio/AudioEngine.cpp`

**Interfaces:**

Consumes:
- `HealthMonitor::observeHostTime(const uint64_t*, int, double, bool)` (Task 2)
- `CaptureCallback::audioDeviceIOCallbackWithContext` (AudioEngine.cpp:27)
- `RenderCallback::audioDeviceIOCallbackWithContext` (AudioEngine.cpp:63)
- `AudioEngine::grantedRate_` (AudioEngine.h:133) — the captured device rate latched at start; use it rather than calling `getCurrentSampleRate()` on the audio thread (a virtual call that may lock inside JUCE)

Produces: `hm.observeHostTime(...)` called once per block, per callback, RT-safely

### Steps

- [ ] Run the existing engine tests first to capture the before state:

```
./tools/dev.cmd ctest --test-dir build -R "AudioEngine" --output-on-failure
```

- [ ] In `CaptureCallback::audioDeviceIOCallbackWithContext` (`AudioEngine.cpp:27-48`), add `observeHostTime` as the very **first** statement (before the `numIn < 2` guard, so we always track timing even if we bail out — but that would mean calling it before we know numIn; safer to keep it after the guard). Insert it immediately **after** the `numIn < 2` early-return guard (after the `if (numIn < 2 ...) return;` line) and before `analyzeInputBlock`:

```cpp
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* /*out*/, int /*numOut*/,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& ctx) override {
        if (numIn < 2 || (int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        e.hm.observeHostTime (ctx.hostTimeNs, numSamples, e.grantedRate_, true);  // <-- add this line
        const float* l = in[0]; const float* r = in[1];
        e.hm.analyzeInputBlock (l, r, numSamples);
        // ... rest unchanged
```

  **Exact edit:** Change the parameter name in the signature from the anonymous `const juce::AudioIODeviceCallbackContext&` to `const juce::AudioIODeviceCallbackContext& ctx` and add the `observeHostTime` call. The existing file uses `const juce::AudioIODeviceCallbackContext&` with no name on line 29.

- [ ] In `RenderCallback::audioDeviceIOCallbackWithContext` (`AudioEngine.cpp:63-89`), similarly name the context parameter and add the call. Insert it **after** the early-return guard (after `if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }`) and before `bridge.pullRender`:

```cpp
    void audioDeviceIOCallbackWithContext (const float* const* /*in*/, int /*numIn*/,
                                           float* const* out, int numOut,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& ctx) override {
        if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        e.hm.observeHostTime (ctx.hostTimeNs, numSamples, e.grantedRate_, false);  // <-- add this line
        const int got = e.bridge.pullRender (mono.data(), numSamples);
        // ... rest unchanged
```

  Note: `grantedRate_` is set in `AudioEngine::start()` (line 274) before the callbacks start and is only written on the message thread, so reading it on the audio thread is safe — it is a plain `double` that will have been fully written before the audio callbacks fire (there is a happens-before from `inD->start(captureCb.get())` on line 312 of AudioEngine.cpp).

- [ ] Build: `./tools/dev.cmd cmake --build build --target eb_tests`
- [ ] Run the full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
- [ ] Confirm all tests still pass (the headless seam `processCaptureBlockForTest` does not go through the callbacks, so no new hostTime calls are made from tests, and the existing tests are unaffected).
- [ ] Commit:

```
git add src/audio/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
feat: forward context.hostTimeNs to HealthMonitor in capture and render callbacks

Wires the optional JUCE host timestamp into the HostTimeDiscontinuity diagnostic.
Null-safe; uses grantedRate_ latched at start (no virtual call on the RT thread).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

### Spec coverage

| Audit item | Addressed |
|---|---|
| §6 optional diagnostic: track `context hostTime` delta per block | Task 2 + Task 4 |
| §6: flag when delta deviates from `numSamples/sampleRate` beyond a tolerance | Task 2 (`kHostTimeTolFrac = 0.05`) |
| §6: handle unavailable (nullptr) `hostTimeNs` gracefully | Task 2, first guard; Test: "null pointer is ignored" |
| Design decision: invalidating vs guidance | Stated and justified in plan header; `raise(HostTimeDiscontinuity)` calls through the existing `raise()` path which clears `cleanCapture` (HealthMonitor.cpp:11-18) |
| §6: "decide whether invalidating or guidance; justify" | Justified in Design decision: equivalent to ExcessDrift/FifoStarved class; a clock jump during a sweep cannot be proven harmless |
| `EngineTypes.h` — only append flags, never repurpose | `1u << 9` is fresh; no existing value changed |
| `CombineMode` — not touched | Confirmed; plan touches only `EngineTypes.h`, `HealthMonitor.{h,cpp}`, `AudioEngine.cpp`, `ClipStatus.h` |
| RT-safety | `observeHostTime` uses only plain locals and two non-atomic scalar members (single-writer per callback thread); no heap, no lock, no syscall, no exception |
| No real EARS serial | All tests use synthetic `uint64_t` timestamps |
| Tests: per-thread independence | "capture and render callbacks track independently" test (Task 2) |
| Tests: reset semantics | "reset clears the prev-timestamp state" test (Task 2) |
| Tests: backward-clock-jump (unsigned wrap) | "large backward jump" test (Task 2) |
| Tests: tolerance boundary | "delta within 5% tolerance does NOT flag" test (Task 2) |

### Placeholder scan

No TBD, "similar to Task N", "add error handling", or "handle edge cases" appears in any code block. Every code snippet is self-contained with real identifiers matching the files read (e.g. `e.grantedRate_`, `ctx.hostTimeNs`, `HealthFlag::HostTimeDiscontinuity`, `raise()`, `prevCapValid_`).

### Type consistency

- `hostTimeNs` type: `const uint64_t*` — matches `juce::AudioIODeviceCallbackContext::hostTimeNs` (`juce_AudioIODevice.h:50`).
- Arithmetic: `uint64_t` subtraction for `delta`; `double` division for `deviation` — correct: no signed underflow, no integer division truncation.
- `kHostTimeTolFrac` is `double` — consistent with `kDriftRatioTol` which is also `double` (`HealthMonitor.h:32`).
- `prevHostNsCapture_` / `prevHostNsRender_` are plain non-atomic `uint64_t` members — correct per the RT-safety rule (each is written only by the audio thread for its callback; `reset()` is called from the message thread before the callbacks start, so there is a happens-before from `start()` → `hm.prepare()` → `reset()`).
- `HealthFlag::HostTimeDiscontinuity = 1u << 9` — `unsigned` domain consistent with `static_cast<unsigned>(f)` in `raise()` and all existing `operator|` / `operator&` (`EngineTypes.h:27-33`).
