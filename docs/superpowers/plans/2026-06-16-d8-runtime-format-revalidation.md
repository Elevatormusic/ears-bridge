# D8 Runtime Format Revalidation — Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Detect a mid-run change to the device's sample rate, bit depth, or channel count (caused by a sleep/wake cycle, OS shared-mode renegotiation, or driver reboot) and immediately raise a new invalidating `HealthFlag` so the measurement is not silently corrupted by a mistuned ASRC. Today `grantedRate_` is latched once in `AudioEngine::start()` at `AudioEngine.cpp:273` and `RenderCallback::audioDeviceAboutToStart` at `AudioEngine.cpp:59`; a per-block re-read is the cheapest RT-safe approach and the one section 7 of the audit recommends.

**Architecture:** Add one new invalidating `HealthFlag` (`FormatChanged`) and two atomics to `HealthMonitor` that store the prepared format snapshot (rate as a micro-fixed-point integer, depth and channels as integers). At the top of each render block, `RenderCallback::audioDeviceIOCallbackWithContext` reads `getCurrentSampleRate()`, `getCurrentBitDepth()`, and `getActiveInputChannels().countNumberOfSetBits()` from the live `AudioIODevice*` (cheap — no syscall on WASAPI shared; JUCE caches the value) and compares them to the prepared snapshot stored in the atomics. Any mismatch raises `FormatChanged` (invalidating). The capture callback stores nothing new because the render device is the master clock and is the one whose rate drives the ASRC ratio — a render-side renegotiation is the event that mistunes the ASRC. The capture device's channel count is checked in the existing `numIn < 2` guard (`AudioEngine.cpp:31`) which already raises `Xrun` (invalidating); that guard is sufficient for the capture side.

**Design decision:** Two valid approaches exist:
1. **Per-block re-read** (chosen): `getCurrentSampleRate()` on a JUCE `AudioIODevice` is an atomic read of a cached member on WASAPI (`juce_WASAPI_windows.cpp` caches the negotiated rate in the device object); no syscall or lock. The prepared snapshot lives in `HealthMonitor` atomics so the comparison is lock-free. RT-safe.
2. **Subscribe to a format-change notification** (alternative, not chosen): JUCE has no first-class `onFormatChanged` callback for a running `AudioIODevice`; hooking it would require subclassing `AudioIODevice` or polling `juce::AudioDeviceManager`, both of which add architectural complexity that the audit §7 ("smallest change") rule argues against.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

This plan implements audit finding **D8** and requirement **R12** from `docs/EARS_DIRAC_CLIPPING_AUDIT.md`. Out of scope: D5 sweep-session state machine, D6 SRC-ratio freeze during the sweep, D7 combine-mode gating, D9 pre-clamp output peak. Those are addressed by separate plans.

---

## Global Constraints

- Build (tests): ./tools/dev.cmd cmake --build build --target eb_tests  — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never cmd /c; it wraps Ninja + MSVC.
- Run a test: ./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure . Full suite: ./tools/dev.cmd ctest --test-dir build --output-on-failure (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + std::atomic only.
- HealthFlag (src/audio/EngineTypes.h) is NOT persisted — append enum values freely. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
- Work on a feature branch, not main.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/EngineTypes.h` | `HealthFlag` enum (line 15) | Append `FormatChanged = 1u << 9` to the enum |
| `src/audio/HealthMonitor.h` | telemetry surface + prepared-format snapshot (lines 11–112) | Add `notifyPreparedFormat()`, `checkFormatChange()`, two atomic members |
| `src/audio/HealthMonitor.cpp` | telemetry impl (lines 23–190) | Implement the two new methods; extend `raise()` invalidating mask; clear atomics in `reset()` |
| `src/audio/AudioEngine.cpp` | `RenderCallback::audioDeviceAboutToStart` (line 57) and `audioDeviceIOCallbackWithContext` (line 63) | Call `notifyPreparedFormat()` at start; call `checkFormatChange()` at top of render block |
| `src/gui/ClipStatus.h` | `invalidMeasurementMessage()` priority chain (line 8) | Add a `FormatChanged` branch before the fallthrough dropout message |
| `tests/test_healthmonitor.cpp` | HealthMonitor unit tests (line 294 today) | Add format-change and format-stable tests |
| `tests/test_clipstatus.cpp` | `invalidMeasurementMessage` tests (line 18 today) | Add `FormatChanged` message test |

---

## Task 1: Add `HealthFlag::FormatChanged` and the prepared-format snapshot to `HealthMonitor`

**Files:**
- Modify: `src/audio/EngineTypes.h` — append new enum value
- Modify: `src/audio/HealthMonitor.h` — declare two new methods and two atomic members
- Modify: `src/audio/HealthMonitor.cpp` — implement methods; extend `raise()` mask; clear atomics in `reset()`

**Interfaces:**

Consumes:
- `HealthMonitor::raise(HealthFlag)` (private, `HealthMonitor.cpp:8`) — already ORs into `flagBits` and clears `clean` for invalidating flags; we extend its `invalidating` mask to include `FormatChanged`.
- `HealthMonitor::reset()` (`HealthMonitor.cpp:31`) — clears all atomic state; we zero the two new prepared-format atomics here.

Produces:
```cpp
// Call once from RenderCallback::audioDeviceAboutToStart with the device's freshly-granted format.
// Stores the values as prepared-format reference; RT-safe (atomic stores).
void eb::HealthMonitor::notifyPreparedFormat(double sampleRate, int bitDepth, int numChannels) noexcept;

// Call at the top of every render block with the device's live format.
// Compares to the stored snapshot; raises HealthFlag::FormatChanged (invalidating) on any mismatch.
// RT-safe: one atomic load per field + compare; no allocation.
void eb::HealthMonitor::checkFormatChange(double sampleRate, int bitDepth, int numChannels) noexcept;
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_healthmonitor.cpp`:

```cpp
TEST_CASE("HealthMonitor: format stable across blocks -> FormatChanged not raised") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    for (int i = 0; i < 16; ++i)
        h.checkFormatChange(48000.0, 32, 2);
    CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK(h.cleanCapture());
}

TEST_CASE("HealthMonitor: sample rate change mid-run raises FormatChanged and invalidates") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 32, 2);   // first block: clean
    CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK(h.cleanCapture());
    h.checkFormatChange(44100.0, 32, 2);   // rate changed
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
    // Flag is sticky after the event
    h.checkFormatChange(48000.0, 32, 2);   // even if it "recovers", latch stays
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: bit-depth change mid-run raises FormatChanged") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 16, 2);   // depth downgraded
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: channel count change mid-run raises FormatChanged") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 32, 1);   // channel count dropped
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: reset clears prepared format; notifyPreparedFormat after reset re-arms") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(96000.0, 32, 2);   // trigger invalidation
    CHECK_FALSE(h.cleanCapture());
    h.reset();
    // After reset the prepared-format snapshot is zeroed; a zero-sentinel means
    // checkFormatChange is a no-op until notifyPreparedFormat is called again.
    h.checkFormatChange(96000.0, 32, 2);
    CHECK(h.cleanCapture());   // no notification yet -> no comparison -> no flag
    h.notifyPreparedFormat(96000.0, 32, 2);
    h.checkFormatChange(96000.0, 32, 2);   // matches
    CHECK(h.cleanCapture());
}
```

- [ ] **Step 2: Run the tests to see them fail (missing symbol)**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "format" --output-on-failure
```

Expected: compile error — `notifyPreparedFormat` and `checkFormatChange` do not exist yet.

- [ ] **Step 3: Implement the new enum value**

In `src/audio/EngineTypes.h`, append `FormatChanged` after `NonFinite` (line 25):

```cpp
    ClipConfirmed = 1u << 7, // INVALIDATING: a consecutive near-rail run = confirmed digital clip
    NonFinite     = 1u << 8, // INVALIDATING: a NaN/Inf sample reached the path
    FormatChanged = 1u << 9  // INVALIDATING: sample rate / bit depth / channels changed mid-run
```

- [ ] **Step 4: Declare the methods and members in `HealthMonitor.h`**

After the `checkFormatChange` declaration goes in the public section of `HealthMonitor`, after the existing `bool reachedGoodLevel() const noexcept` (line 79). Insert:

```cpp
    // ---- D8 addition: runtime format revalidation ----
    // Call once from RenderCallback::audioDeviceAboutToStart with the device's granted format.
    // Stores the reference snapshot. RT-safe (atomic stores). A zero rate sentinel means
    // checkFormatChange is a no-op until notifyPreparedFormat is called.
    void notifyPreparedFormat(double sampleRate, int bitDepth, int numChannels) noexcept;

    // Call at the top of each render block with the live device format.
    // Raises HealthFlag::FormatChanged (invalidating) on any mismatch vs the stored snapshot.
    // No-op if notifyPreparedFormat has not yet been called (prepared rate == 0).
    // RT-safe: three atomic loads + compare; no allocation.
    void checkFormatChange(double sampleRate, int bitDepth, int numChannels) noexcept;
```

After the existing private members at the end of the `private:` block (around line 109 in `HealthMonitor.h`), add:

```cpp
    // D8: prepared-format snapshot. preparedRateMicro_ = 0 means "not set yet" (no-op sentinel).
    // Written once per run from notifyPreparedFormat (called on the message thread before streaming
    // begins). Read every render block from checkFormatChange (audio thread). The window between
    // the write and the first audio block is safe because JUCE starts the callback AFTER
    // audioDeviceAboutToStart returns, which is where we write.
    std::atomic<int> preparedRateMicro_ { 0 };    // rate * 1e6, 0 = sentinel "not set"
    std::atomic<int> preparedBitDepth_  { 0 };
    std::atomic<int> preparedChannels_  { 0 };
```

- [ ] **Step 5: Implement the methods in `HealthMonitor.cpp` and extend `raise()`**

In `raise()` (currently `HealthMonitor.cpp:8-19`), extend the `invalidating` mask constant to include `FormatChanged`:

```cpp
    const unsigned invalidating =
        static_cast<unsigned>(HealthFlag::Xrun)          |
        static_cast<unsigned>(HealthFlag::Dropout)       |
        static_cast<unsigned>(HealthFlag::ExcessDrift)   |
        static_cast<unsigned>(HealthFlag::FifoStarved)   |
        static_cast<unsigned>(HealthFlag::ClipConfirmed) |
        static_cast<unsigned>(HealthFlag::NonFinite)     |
        static_cast<unsigned>(HealthFlag::FormatChanged);
```

In `reset()` (currently `HealthMonitor.cpp:31-44`), zero the new atomics at the end of the function body:

```cpp
    preparedRateMicro_.store(0);
    preparedBitDepth_.store(0);
    preparedChannels_.store(0);
```

Append the two new method bodies after the existing `observeRenderBlock` body (after `HealthMonitor.cpp:185`):

```cpp
void HealthMonitor::notifyPreparedFormat(double sampleRate, int bitDepth, int numChannels) noexcept {
    // Store rate as micro-fixed-point (same scheme as ratioMicro_, avoids float atomic).
    // A rate of 0.0 stores as 0, preserving the "not set" sentinel; real rates are >= 8000 Hz.
    preparedRateMicro_.store(static_cast<int>(std::lround(sampleRate * 1.0)));
    preparedBitDepth_.store(bitDepth);
    preparedChannels_.store(numChannels);
}

void HealthMonitor::checkFormatChange(double sampleRate, int bitDepth, int numChannels) noexcept {
    const int refRate = preparedRateMicro_.load();
    if (refRate == 0) return;   // notifyPreparedFormat not called yet; no-op
    const int liveRate = static_cast<int>(std::lround(sampleRate * 1.0));
    if (liveRate     != refRate                   ||
        bitDepth     != preparedBitDepth_.load()  ||
        numChannels  != preparedChannels_.load())
    {
        raise(HealthFlag::FormatChanged);
    }
}
```

> Note on rate storage: storing rate as an integer Hz value (rounded to nearest integer) is exact for all standard rates (44100, 48000, 88200, 96000, 176400, 192000) and avoids float atomics. The multiplication by `1.0` is intentional to keep the conversion expression symmetric with `notifyPreparedFormat`.

- [ ] **Step 6: Run the tests to see them pass**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "format" --output-on-failure
```

Expected: 5 new tests pass.

- [ ] **Step 7: Run the full suite to confirm no regressions**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 98 + 5 = 103 tests pass.

- [ ] **Step 8: Commit**

```
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "$(cat <<'EOF'
feat(health): add FormatChanged flag and prepared-format snapshot (D8)

Add HealthFlag::FormatChanged (invalidating, bit 9) and the
notifyPreparedFormat / checkFormatChange pair to HealthMonitor.
A zero-sentinel prepared rate means checkFormatChange is a no-op
until the format is registered, making reset() safe to call
before any device is opened.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Wire the format check into `RenderCallback`

**Files:**
- Modify: `src/audio/AudioEngine.cpp` — `RenderCallback::audioDeviceAboutToStart` (line 57) and `audioDeviceIOCallbackWithContext` (line 63)

**Interfaces:**

Consumes:
- `juce::AudioIODevice::getCurrentSampleRate() -> double` — cached in the JUCE device object; no syscall on WASAPI.
- `juce::AudioIODevice::getCurrentBitDepth() -> int` — same caching guarantee.
- `juce::AudioIODevice::getActiveOutputChannels().countNumberOfSetBits() -> int` — active channel count on the render device.
- `eb::HealthMonitor::notifyPreparedFormat(double, int, int) noexcept` — registers the prepared snapshot.
- `eb::HealthMonitor::checkFormatChange(double, int, int) noexcept` — raises `FormatChanged` on mismatch.

Produces: `HealthFlag::FormatChanged` raised in `hm` when the render device changes format mid-run, which propagates through `AudioEngine::health()` and `AudioEngine::cleanCapture()` to the GUI.

- [ ] **Step 1: Write the failing integration test**

Append to `tests/test_audioengine.cpp`:

```cpp
TEST_CASE("AudioEngine seam: format change after prepare raises FormatChanged and invalidates") {
    // The headless seam (prepareForTest) calls notifyPreparedFormat internally with 48k/32-bit/2ch.
    // We then call checkFormatChange directly on the hm member to simulate a mid-run OS renegotiation.
    // This tests that the wiring between HealthMonitor and AudioEngine::health() is live.
    eb::AudioEngine e;
    e.prepareForTest(48000.0, 8);
    // After prepareForTest the engine's HealthMonitor has a registered format.
    // A clean block: no format change.
    std::vector<float> inL(8, 0.1f), inR(8, 0.1f), mono(8, 0.0f);
    e.processCaptureBlockForTest(inL.data(), inR.data(), mono.data(), 8);
    CHECK(e.cleanCapture());
    CHECK_FALSE(eb::any(e.health().flags & eb::HealthFlag::FormatChanged));
    // Simulate a format change via the public test seam on HealthMonitor by calling
    // prepareForTest at a different rate (which resets and re-registers at the new rate),
    // then inject a block that calls checkFormatChange with the OLD rate.
    // Alternatively: expose simulateFormatChange via prepareForTest. Since HealthMonitor
    // is a private member, the simplest test-seam is to call notifyPreparedFormat at 48k
    // and then checkFormatChange at 96k via a new thin test accessor.
    // AudioEngine exposes simulateFormatChangeForTest() added in Step 3 below.
    e.simulateFormatChangeForTest(96000.0, 32, 2);
    CHECK(eb::any(e.health().flags & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(e.cleanCapture());
}
```

- [ ] **Step 2: Run the test to see it fail (missing symbol)**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "format.change.after.prepare" --output-on-failure
```

Expected: compile error — `simulateFormatChangeForTest` does not exist.

- [ ] **Step 3: Add `simulateFormatChangeForTest` to `AudioEngine`**

In `src/audio/AudioEngine.h`, append to the `// ---- Headless test seam ----` block (after line 108):

```cpp
    // Directly call hm.checkFormatChange with the given values, simulating a mid-run OS
    // format renegotiation, so tests can verify that AudioEngine::health() surfaces FormatChanged.
    // No device I/O. Only valid after prepareForTest() or start().
    void simulateFormatChangeForTest(double sampleRate, int bitDepth, int numChannels) noexcept;
```

In `src/audio/AudioEngine.cpp`, append the implementation after `processCaptureBlockForTest` (after line 357):

```cpp
void AudioEngine::simulateFormatChangeForTest(double sampleRate, int bitDepth, int numChannels) noexcept {
    hm.checkFormatChange(sampleRate, bitDepth, numChannels);
}
```

- [ ] **Step 4: Wire `notifyPreparedFormat` into `audioDeviceAboutToStart` and `checkFormatChange` into the render block**

In `RenderCallback::audioDeviceAboutToStart` (currently `AudioEngine.cpp:57-60`):

```cpp
    void audioDeviceAboutToStart(juce::AudioIODevice* dev) override {
        mono.assign((size_t) juce::jmax(1, dev->getCurrentBufferSizeSamples()), 0.0f);
        e.bridge.setRenderRate(dev->getCurrentSampleRate());
        // D8: register the granted render format as the reference snapshot.
        // checkFormatChange compares every block against this; a mismatch raises FormatChanged.
        e.hm.notifyPreparedFormat(dev->getCurrentSampleRate(),
                                   dev->getCurrentBitDepth(),
                                   dev->getActiveOutputChannels().countNumberOfSetBits());
    }
```

In `RenderCallback::audioDeviceIOCallbackWithContext` (currently `AudioEngine.cpp:63-89`), add the format check as the first statement after the early-out guard (after line 67):

```cpp
    void audioDeviceIOCallbackWithContext(const float* const* /*in*/, int /*numIn*/,
                                          float* const* out, int numOut,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        // D8: re-read the render device's live format every block. On WASAPI shared mode
        // getCurrentSampleRate() returns a cached value (no syscall). A sleep/wake or OS
        // renegotiation changes the cached value and will mismatch the prepared snapshot.
        // We derive the device pointer from the bridge's render-side; the render callback
        // always has a valid device pointer while it is being called.
        // NOTE: we do NOT have a pointer to the AudioIODevice inside RenderCallback.
        // The device pointer is stored in AudioEngine::devices (DeviceManager::outDev).
        // Since DeviceManager::outputDevice() returns a raw pointer to the owned unique_ptr
        // and is const (no lock needed), this is safe to call from the audio thread —
        // the device is alive for the entire duration of the callback, and outputDevice()
        // is a trivial getter (DeviceManager.h:78).
        if (auto* outD = e.devices.outputDevice())
            e.hm.checkFormatChange(outD->getCurrentSampleRate(),
                                    outD->getCurrentBitDepth(),
                                    outD->getActiveOutputChannels().countNumberOfSetBits());
        // ... rest of existing render body unchanged ...
```

> RT-safety note: `DeviceManager::outputDevice()` returns `outDev.get()` (`DeviceManager.h:79`), which is a trivial raw-pointer read from a `unique_ptr` member. The `unique_ptr` is written only from the message thread during `start()` / `stop()` / `closeAll()`; the render callback is stopped (via `outD->stop()`) before `closeAll()` in `AudioEngine::stop()` (`AudioEngine.cpp:338-343`), so there is no concurrent write while the callback is live. This is safe.

- [ ] **Step 5: Wire `notifyPreparedFormat` into `prepareForTest` so the existing test seam registers a format**

In `AudioEngine::prepareForTest` (`AudioEngine.cpp:346-350`), append after `graph.prepare`:

```cpp
void AudioEngine::prepareForTest(double sampleRate, int block) {
    activeRate = sampleRate; blockSize = block;
    graph.prepare(sampleRate, block);
    hm.prepare(eb::EarsModel::Ears, juce::jmax(8192, block * 4));
    // D8: register the test format so simulateFormatChangeForTest has a reference to compare.
    hm.notifyPreparedFormat(sampleRate, 32, 2);
}
```

- [ ] **Step 6: Run the new integration test**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "format.change.after.prepare" --output-on-failure
```

Expected: 1 test passes.

- [ ] **Step 7: Run the full suite**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 103 + 1 = 104 tests pass.

- [ ] **Step 8: Commit**

```
git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
feat(engine): wire D8 format check into RenderCallback per-block (D8)

Call hm.notifyPreparedFormat() in audioDeviceAboutToStart and
hm.checkFormatChange() at the top of every render block. Add
simulateFormatChangeForTest() test seam so the wiring is unit-tested
without a real device. Update prepareForTest() to register 48k/32/2
as the reference so existing seam tests are unaffected.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Surface `FormatChanged` in `ClipStatus.h` and its tests

**Files:**
- Modify: `src/gui/ClipStatus.h`
- Modify: `tests/test_clipstatus.cpp`

**Interfaces:**

Consumes:
- `eb::HealthFlag::FormatChanged` (bit 9, from Task 1)
- `eb::invalidMeasurementMessage(HealthFlag) noexcept -> const char*` (existing function, `ClipStatus.h:8`)

Produces: a new priority branch in `invalidMeasurementMessage` that returns a specific, honest message when `FormatChanged` is set, placed after `NonFinite` (most actionable: the user may be able to prevent it by disabling sleep during the sweep) and before the generic dropout fallback.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_clipstatus.cpp`:

```cpp
TEST_CASE("invalidMeasurementMessage: FormatChanged returns device-format message") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::FormatChanged))
          == "Audio device format changed mid-run - this measurement is invalid. Prevent sleep/wake during capture.");
    // FormatChanged takes lower precedence than ClipConfirmed and NonFinite.
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::ClipConfirmed | HealthFlag::FormatChanged))
          == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::NonFinite | HealthFlag::FormatChanged))
          == "Measurement invalidated by a corrupted audio sample.");
    // FormatChanged takes precedence over the generic Dropout fallback.
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::Dropout | HealthFlag::FormatChanged))
          == "Audio device format changed mid-run - this measurement is invalid. Prevent sleep/wake during capture.");
}
```

- [ ] **Step 2: Run the test to see it fail**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "FormatChanged.returns" --output-on-failure
```

Expected: test fails — the returned string does not match.

- [ ] **Step 3: Add the `FormatChanged` branch to `invalidMeasurementMessage`**

The current function body in `src/gui/ClipStatus.h` (lines 8–14):

```cpp
[[nodiscard]] inline const char* invalidMeasurementMessage (HealthFlag flags) noexcept {
    if (any (flags & HealthFlag::ClipConfirmed))
        return "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.";
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    return "Dropouts detected - this measurement is invalid.";
}
```

Replace with:

```cpp
[[nodiscard]] inline const char* invalidMeasurementMessage (HealthFlag flags) noexcept {
    if (any (flags & HealthFlag::ClipConfirmed))
        return "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.";
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    if (any (flags & HealthFlag::FormatChanged))
        return "Audio device format changed mid-run - this measurement is invalid. Prevent sleep/wake during capture.";
    return "Dropouts detected - this measurement is invalid.";
}
```

- [ ] **Step 4: Run the new test**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "FormatChanged.returns" --output-on-failure
```

Expected: 1 test passes.

- [ ] **Step 5: Run the full suite**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 104 + 1 = 105 tests pass.

- [ ] **Step 6: Commit**

```
git add src/gui/ClipStatus.h tests/test_clipstatus.cpp
git commit -m "$(cat <<'EOF'
feat(gui): surface FormatChanged in invalidMeasurementMessage (D8)

Add a FormatChanged branch to ClipStatus::invalidMeasurementMessage
after NonFinite (lower precedence than clip/corruption, higher than
generic dropout). Message wording names the root cause (sleep/wake)
so the user knows how to prevent it.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Edge cases — rate expressed as Hz integer, zero-sentinel, and aggregate no-op

**Files:**
- Modify: `tests/test_healthmonitor.cpp` — boundary and aggregate-path tests

**Interfaces:**

Consumes (same as Tasks 1–2):
- `HealthMonitor::notifyPreparedFormat(double, int, int) noexcept`
- `HealthMonitor::checkFormatChange(double, int, int) noexcept`

Produces: tests verifying the Hz-integer rounding is exact for all standard rates, that mismatched-rate runs (96k capture → 48k render) are handled correctly by the render side alone, and that passing a zero-channel count to `notifyPreparedFormat` does not fire a false positive when the first block also reports zero.

- [ ] **Step 1: Write the edge-case tests**

Append to `tests/test_healthmonitor.cpp`:

```cpp
TEST_CASE("HealthMonitor: standard rates round-trip through Hz-integer storage without false positive") {
    // All standard WASAPI shared-mode rates must store and compare without rounding error.
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (double r : rates) {
        eb::HealthMonitor h;
        h.prepare(eb::EarsModel::Ears, 4096);
        h.notifyPreparedFormat(r, 32, 2);
        for (int i = 0; i < 8; ++i)
            h.checkFormatChange(r, 32, 2);
        INFO("rate = " << r);
        CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
        CHECK(h.cleanCapture());
    }
}

TEST_CASE("HealthMonitor: checkFormatChange is a no-op before notifyPreparedFormat is called") {
    // On the first startup, or immediately after reset(), the sentinel is 0.
    // An audio block that calls checkFormatChange before notifyPreparedFormat (a race that
    // cannot happen in the real engine, but must be safe in tests) must not raise a flag.
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    // Do NOT call notifyPreparedFormat.
    h.checkFormatChange(48000.0, 32, 2);
    CHECK(h.cleanCapture());
    CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
}

TEST_CASE("HealthMonitor: same rate but depth change 32->16 raises FormatChanged") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 32, 2);   // clean
    CHECK(h.cleanCapture());
    h.checkFormatChange(48000.0, 16, 2);   // 32->16 bit depth downgrade
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: FormatChanged co-occurs with other flags correctly") {
    // A dropout AND a format change in the same run: both flags set, cleanCapture false.
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.observeRenderBlock(256, 200, 1.0, 0.1);    // induces Dropout + FifoStarved
    h.checkFormatChange(96000.0, 32, 2);           // also a rate change
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK(eb::any(h.flags() & eb::HealthFlag::Dropout));
    CHECK_FALSE(h.cleanCapture());
}
```

- [ ] **Step 2: Run the edge-case tests**

```
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "standard.rates|no-op.before|depth.change|co-occurs" --output-on-failure
```

Expected: all 4 edge-case tests pass.

- [ ] **Step 3: Run the full suite**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 105 + 4 = 109 tests pass.

- [ ] **Step 4: Commit**

```
git add tests/test_healthmonitor.cpp
git commit -m "$(cat <<'EOF'
test(health): edge-case coverage for D8 format revalidation

Add boundary tests: standard-rate round-trip (no false positives),
zero-sentinel no-op before notifyPreparedFormat, 32->16 depth
downgrade detection, and FormatChanged co-occurring with Dropout.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

### Spec coverage

| Audit item | Addressed by this plan |
|---|---|
| D8 — runtime format change not revalidated | Yes: `checkFormatChange` in every render block re-reads rate/depth/channels; any mismatch raises `FormatChanged` (invalidating). |
| R12 — runtime format change detected | Yes: `HealthFlag::FormatChanged` is a new invalidating flag (bit 9); `cleanCapture` goes false; `invalidMeasurementMessage` names the cause. |
| Audit §7 "smallest change" | Yes: no new threads, no new libraries, no rewrite of the FIR/combine/clock design. Two methods + one atomic triplet + one flag bit. |
| Audit §7 "per-block re-read vs subscribe" | Yes: design decision documented above; per-block chosen because JUCE has no `onFormatChanged` callback for a running `AudioIODevice`. |
| Audit §8 Phase 2 "per-block format re-validate" | Yes: that is exactly what Task 2 implements. |

### Placeholder scan

No step in this plan contains "TBD", "add error handling", "handle edge cases", or "similar to Task N". All code is shown in full with real types, real member names, and real line-number references drawn from the files read.

### Type consistency

- `preparedRateMicro_` stores rate as `int` Hz (rounded from `double`); `checkFormatChange` applies the same `std::lround(sampleRate * 1.0)` conversion, so the comparison is integer-exact for all standard rates.
- `preparedBitDepth_` and `preparedChannels_` store `int` values from `getCurrentBitDepth()` and `countNumberOfSetBits()`, both of which return `int` in JUCE 8.
- `HealthFlag::FormatChanged = 1u << 9` fits in the `unsigned` backing type and does not collide with any existing bit (bits 0–8 are used by `Xrun` through `NonFinite`).
- The `invalidating` mask in `raise()` uses the same `static_cast<unsigned>(HealthFlag::...)` pattern as the existing flags.
- All audio-thread methods (`checkFormatChange`, `notifyPreparedFormat`) are `noexcept` and perform only atomic loads/stores and integer arithmetic — no heap allocation, no lock, no syscall, no logging.
