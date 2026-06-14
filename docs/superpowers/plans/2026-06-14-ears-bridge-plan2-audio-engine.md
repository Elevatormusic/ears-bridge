# Plan 2 — Audio Engine + Clock Bridge

For agentic workers: REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan one task at a time, dispatching each Task to a fresh subagent with the full task text, then reviewing its diff and test output before moving on. Steps use checkbox (`- [ ]`) syntax.

**Pre-execution:** run an API-verification pass (as Plan 1 did) over this plan before implementing — it grounds the JUCE/CMake API calls against current docs (JUCE 8.0.4). The load-bearing APIs below were grounded against `docs.juce.com/master` and context7 during authoring: `AudioDeviceManager`, `AudioIODeviceType` (`createAudioDeviceTypes`, `scanForDevices`, `getDeviceNames`, `getTypeName`, `hasSeparateInputsAndOutputs`), `AudioIODevice` (`open(const BigInteger&, const BigInteger&, double, int)`, `start`, `stop`, `getAvailableSampleRates`, `getAvailableBufferSizes`, `getCurrentBitDepth`, `getCurrentSampleRate`, `getOutputLatencyInSamples`), `AudioIODeviceCallback::audioDeviceIOCallbackWithContext`, `AbstractFifo` (`prepareToWrite`/`finishedWrite`/`prepareToRead`/`finishedRead`), and `juce::Interpolators::Lagrange` / `LagrangeInterpolator::process(speedRatio, in, out, numOut, numInAvail, wrapAround)`.

**Goal:** Build the headless audio engine — `DeviceManager`, `ClockBridge`, `HealthMonitor`, and the GUI-facing `AudioEngine` facade — that wires EARS/EARS-Pro capture through Plan 1's `eb::ProcessingGraph` and across two independent clock domains into a virtual sink, with no GUI.

**Architecture:** Two independent audio callbacks (capture clock + render clock) communicate only through a lock-free `ClockBridge` (an `AbstractFifo` of `float` mono samples plus an asynchronous sample-rate converter trimmed by a PI fill-control loop); the render callback is the master clock. `DeviceManager` enumerates WASAPI/CoreAudio endpoints, detects the EARS model from device name/USB identity, and reports per-model native sample rates and bit depths. `AudioEngine` owns a `DeviceManager`, a Plan-1 `eb::ProcessingGraph`, a `ClockBridge`, and a `HealthMonitor`, designs the per-ear FIR at the active rate via `eb::FirDesigner`, and exposes the only API the GUI (Plan 3) talks to.

**Tech Stack:** C++20, JUCE 8.0.4 (`juce_audio_basics`, `juce_audio_devices`, `juce_dsp`, `juce_core`), Catch2 v3.6.0, CMake + Ninja + MSVC via `tools\dev.cmd`. Namespace `eb` throughout. macOS uses CoreAudio (aggregate-device path is Plan 4); Windows uses WASAPI ("Windows Audio" device type) for **both** devices.

---

## File structure

**Created (this plan):**
- `src/audio/DeviceId.h` — device identity + `EarsModel`; `key()` = `typeName|name|uid`; `operator==`. Stability is only as strong as `uid`: replug-stable where JUCE exposes a real endpoint id, otherwise name-stable (uid==name) — see the uid-stability note.
- `src/audio/EngineTypes.h` — `EarsModel`, `EngineStatus`, `Levels`, `Health` POD/enums shared across the engine.
- `src/audio/ModelDetect.h` / `.cpp` — pure functions: detect `EarsModel` from a device name, and map model → native sample rates / bit depths (unit-testable, hardware-free).
- `src/audio/DeviceManager.h` / `.cpp` — JUCE-backed enumeration, model/rate/bit-depth detection, open/close of capture + render devices on WASAPI/CoreAudio.
- `src/audio/ClockBridge.h` / `.cpp` — lock-free `AbstractFifo` + async SRC + PI fill-control loop across the two clocks.
- `src/audio/HealthMonitor.h` / `.cpp` — atomic xrun/slip/level telemetry + clean-capture latch (basic; Plan 4 extends).
- `src/audio/AudioEngine.h` / `.cpp` — facade wiring `DeviceManager` + `eb::ProcessingGraph` + `ClockBridge` + `HealthMonitor`; the GUI-facing API; designs the FIR at the active rate.
- `src/tools/eb_diag_main.cpp` — console diagnostic: lists devices with detected model + native rates/bit-depths, runs a ~5 s passthrough (manual verification).

**Modified:**
- `CMakeLists.txt` — add an `eb_engine` static library (sources above except the diag main), linked by `eb_tests`, the `EarsBridge` app, and a new `eb_diag` console target.
- `tests/CMakeLists.txt` — add the five new engine test sources; link `eb_engine`.

**Reused as-is from Plan 1 (rate-agnostic, do not modify):** `eb::CalFile`, `eb::FirDesigner` + `FirDesignParams` + `FirMode`, `eb::ProcessingGraph`, `eb::CombineMode`.

## Shared contract (SPINE recap)

All code is namespace `eb`, C++20, JUCE 8. These exact type names, signatures, and file paths are shared with Plans 3 and 4 — do not invent variants.

```cpp
enum class EarsModel { Unknown, Ears, EarsPro };   // set by DeviceManager from device name / USB VID:PID

struct DeviceId {
    juce::String typeName, name, uid;
    bool isVirtualSink = false;
    EarsModel model = EarsModel::Unknown;
    bool operator== (const DeviceId&) const;
    juce::String key() const;   // typeName + "|" + name + "|" + uid (see uid-stability note below)
};
// uid-stability note: rescan() fills `uid` from a real stable endpoint id ONLY where JUCE
// exposes one; where only the display name is available, uid==name and key() is NAME-stable,
// not replug-stable. CalBinder keys are therefore name-stable only — do not claim a stronger
// guarantee than the platform delivers.

enum class EngineStatus { Stopped, Running, Error };

struct Levels { float inL = 0, inR = 0, outMono = 0; bool clipL = false, clipR = false, clipOut = false; };

struct Health {
    int xruns = 0;
    long long droppedFrames = 0;
    double fifoFill = 0.0;
    bool cleanCapture = true;
    double captureToRenderRatio = 1.0;
};
```

```cpp
// AudioEngine — the ONLY surface the GUI talks to
std::vector<DeviceId> inputDevices() const;
std::vector<DeviceId> outputDevices() const;                      // virtual sinks flagged isVirtualSink
std::vector<double>   supportedSampleRates (const DeviceId&) const; // EARS -> {48000}; EARS Pro -> {44100,48000,88200,96000,176400,192000}
std::vector<int>      supportedBitDepths   (const DeviceId&) const; // EARS -> {24}; EARS Pro -> {16,24,32}
void setInput  (const DeviceId&);                                  // configure while Stopped
void setOutput (const DeviceId&);
void setSampleRate (double sr);                                    // must be one of the input native rates
void setOutputBitDepth (int bits);                                 // 16/24/32 (24 default); requested best-effort on the render format (see note)
void setLeftCalFir  (juce::AudioBuffer<float> fir);                // hot-swappable while Running
void setRightCalFir (juce::AudioBuffer<float> fir);
void setCombineMode (eb::CombineMode);
bool start (juce::String& errorOut);  void stop();  EngineStatus status() const;
Levels levels() const;   // lock-free snapshot for the GUI timer
Health health() const;   // lock-free snapshot
```

```cpp
// ClockBridge
void prepare (double captureRate, double renderRate, int channels, int capacityFrames);
void pushCapture (const float* mono, int numFrames);   // producer = capture callback
int  pullRender  (float* out, int numFrames);          // consumer = render callback; returns frames written
void setRenderRate (double);  void reset();
double fifoFill() const; int underruns() const; int overruns() const;
double currentRatio() const;   // current trimmed capture:render resample ratio (nominal * PI trim)
```

**Dataflow the engine wires:**
- Capture cb (2ch @ input native rate) → `eb::ProcessingGraph::process` (per-ear FIR + combine) → mono @ capture rate → `ClockBridge::pushCapture`. *[capture clock]*
- Virtual render cb (@ device rate) → `ClockBridge::pullRender` → duplicate mono to BOTH output channels (L=R). *[render clock; master]*
- Two callbacks, independent clocks (ALWAYS — even at the same nominal rate they drift); `ClockBridge` async SRC + fill-control loop absorbs drift. `HealthMonitor` updated from both via atomics only.

**Hard constraints:**
- Input native rate/bit-depth come from the detected model (EARS = 48k/24-bit only; EARS Pro = 44.1–192k, 16/24/32-bit). Non-native selections resample and must warn.
- Output bit depth (16/24/32, 24 default) is **requested** on the render device format, not guaranteed. JUCE shared-mode rendering is float and may not honor an arbitrary integer depth; the engine records the requested depth, requests it best-effort, and falls back to the nearest supported format. 24-bit is ample for measurement; 32-bit only matters where the sink genuinely accepts it (e.g. BlackHole) and gives no measurement benefit beyond 24-bit. Do not imply enforced bit depth anywhere in the UI or status line.
- FIR designed at the ACTIVE rate: `FirDesignParams.sampleRate = active rate`, `numTaps ≈ 8192·(activeRate/48000)` rounded to a power of two (8192@48k, 16384@96k, 32768@192k). Rebuild on rate change. The engine owns this; `ProcessingGraph` re-prepares at the active rate.
- Windows = WASAPI for BOTH devices (ASIO `hasSeparateInputsAndOutputs()==false` cannot pair EARS capture with a different virtual render). macOS = CoreAudio. Detect ASIO-only and fall back to WASAPI with a message.
- No allocation/lock/syscall on either audio callback. FIR hot-swap via `eb::ProcessingGraph::setFir` (lock-free; async load + gain ramp — let it settle before measuring).

---

## Tasks

### Task 1 — `eb_engine` library scaffold + EngineTypes + DeviceId

Stand up the new static library, the shared POD types, and `DeviceId` with its stable key and equality. This is the foundation every later task links against.

**Files**
- Create: `src/audio/EngineTypes.h`
- Create: `src/audio/DeviceId.h`
- Create: `src/audio/DeviceId.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/test_deviceid.cpp`

**Steps**

- [ ] **Write the EngineTypes header.** Create `src/audio/EngineTypes.h`:
  ```cpp
  #pragma once
  #include <juce_core/juce_core.h>   // juce::String used by sibling spine headers; Plan 4 precondition
  namespace eb {

  enum class EarsModel  { Unknown, Ears, EarsPro };
  enum class EngineStatus { Stopped, Running, Error };

  struct Levels {
      float inL = 0, inR = 0, outMono = 0;
      bool  clipL = false, clipR = false, clipOut = false;
  };

  struct Health {
      int       xruns = 0;
      long long droppedFrames = 0;
      double    fifoFill = 0.0;
      bool      cleanCapture = true;
      double    captureToRenderRatio = 1.0;
  };

  } // namespace eb
  ```

- [ ] **Write the DeviceId header.** Create `src/audio/DeviceId.h`:
  ```cpp
  #pragma once
  #include <juce_core/juce_core.h>
  #include "audio/EngineTypes.h"
  namespace eb {

  struct DeviceId {
      juce::String typeName, name, uid;
      bool      isVirtualSink = false;
      EarsModel model = EarsModel::Unknown;

      bool operator== (const DeviceId& other) const;
      // typeName + "|" + name + "|" + uid. Stability is bounded by `uid`: replug-stable only
      // where rescan() obtained a real stable endpoint id; otherwise uid==name and key() is
      // name-stable only (a known limitation — CalBinder keys are name-stable in that case).
      juce::String key() const;
  };

  } // namespace eb
  ```

- [ ] **Write the failing test.** Create `tests/test_deviceid.cpp`:
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include "audio/DeviceId.h"

  TEST_CASE("DeviceId::key concatenates typeName|name|uid") {
      eb::DeviceId d; d.typeName = "Windows Audio"; d.name = "EARS"; d.uid = "{abc-123}";
      CHECK (d.key() == juce::String ("Windows Audio|EARS|{abc-123}"));
  }

  TEST_CASE("DeviceId equality ignores volatile fields, uses identity triple + flags") {
      eb::DeviceId a; a.typeName = "Windows Audio"; a.name = "EARS"; a.uid = "u1";
      eb::DeviceId b = a;
      CHECK (a == b);
      b.uid = "u2";
      CHECK_FALSE (a == b);   // different endpoint id => different device
  }

  TEST_CASE("DeviceId key is stable across a simulated replug WHEN a real stable uid exists") {
      // This stability holds ONLY when rescan() captured a real stable endpoint id into uid
      // (here a USB hardware id). When the platform exposes only a display name, rescan() sets
      // uid==name and the key is merely name-stable, not replug-stable (see uid-stability note).
      eb::DeviceId before; before.typeName = "Windows Audio"; before.name = "miniDSP EARS"; before.uid = "USB\\VID_2752";
      eb::DeviceId after  = before;   // replug: OS index changed but the stable identity triple did not
      CHECK (before.key() == after.key());
      CHECK (before == after);
  }
  ```

- [ ] **Add `eb_engine` to the build.** In `CMakeLists.txt`, insert immediately after the `eb_core` block (after line `target_compile_definitions(eb_core PUBLIC ... )`):
  ```cmake
  # ---- Engine library (devices, clock bridge, health, facade) ----
  add_library(eb_engine STATIC
      src/audio/DeviceId.cpp
      src/audio/ModelDetect.cpp
      src/audio/ClockBridge.cpp
      src/audio/HealthMonitor.cpp
      src/audio/DeviceManager.cpp
      src/audio/AudioEngine.cpp)
  target_include_directories(eb_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(eb_engine PUBLIC cxx_std_20)
  # DeviceManager.h / AudioEngine.h are PUBLIC headers that #include <juce_audio_devices/...> and
  # declare juce::AudioDeviceManager / std::unique_ptr<juce::AudioIODevice> members, so juce_audio_devices
  # is part of eb_engine's PUBLIC interface and MUST be linked PUBLIC (mirrors the eb_core rule) so its
  # include dirs + JUCE config defines (JUCE_MODULE_AVAILABLE_juce_audio_devices, JUCE_WASAPI, ...) reach consumers.
  target_link_libraries(eb_engine
      PUBLIC
          eb_core
          juce::juce_audio_basics juce::juce_dsp juce::juce_core
          juce::juce_audio_devices
          juce::juce_recommended_config_flags
          juce::juce_recommended_warning_flags)
  target_compile_definitions(eb_engine PUBLIC
      JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
  ```
  Then link the engine into the app — change the `EarsBridge` link line to:
  ```cmake
  target_link_libraries(EarsBridge PRIVATE
      eb_core eb_engine juce::juce_gui_extra juce::juce_audio_devices
      juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
  ```

- [ ] **Register the test source.** In `tests/CMakeLists.txt`, add `test_deviceid.cpp` to the `add_executable(eb_tests ...)` list, and add `eb_engine` + `juce::juce_audio_devices` to its `target_link_libraries`:
  ```cmake
  add_executable(eb_tests
      test_smoke.cpp
      test_calfile.cpp
      test_firdesigner.cpp
      test_processinggraph.cpp
      test_deviceid.cpp)
  target_link_libraries(eb_tests PRIVATE
      eb_core eb_engine
      juce::juce_audio_basics juce::juce_dsp juce::juce_audio_devices
      juce::juce_recommended_config_flags
      Catch2::Catch2WithMain)
  ```
  (Leave the existing `target_compile_definitions` and `catch_discover_tests` lines unchanged.)

- [ ] **Run it — expect FAIL** (the `eb_engine` sources `ModelDetect.cpp`, `ClockBridge.cpp`, etc. do not exist yet, so configure/build fails to find them; `DeviceId.cpp` is also missing).
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  ```
  Expected: CMake errors / Ninja "No rule to make target" for the missing `.cpp` files. This confirms the build now *requires* the engine sources.

- [ ] **Create stub source files so the library links, then implement DeviceId.** Create the four other engine `.cpp` stubs as empty namespaced files (each will be filled in its own task), and implement `DeviceId.cpp` fully now. Create `src/audio/ModelDetect.cpp`, `src/audio/ClockBridge.cpp`, `src/audio/HealthMonitor.cpp`, `src/audio/DeviceManager.cpp`, `src/audio/AudioEngine.cpp`, each containing exactly:
  ```cpp
  // Implemented in a later task of Plan 2.
  namespace eb {}
  ```
  Then create `src/audio/DeviceId.cpp`:
  ```cpp
  #include "audio/DeviceId.h"
  namespace eb {

  juce::String DeviceId::key() const {
      return typeName + "|" + name + "|" + uid;
  }

  bool DeviceId::operator== (const DeviceId& other) const {
      return typeName == other.typeName
          && name == other.name
          && uid == other.uid
          && isVirtualSink == other.isVirtualSink
          && model == other.model;
  }

  } // namespace eb
  ```

- [ ] **Configure, build, run — expect PASS.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R DeviceId"
  ```
  Expected: the three `DeviceId` test cases pass; ctest prints `100% tests passed`.

- [ ] **Commit.**
  ```
  git add CMakeLists.txt tests/CMakeLists.txt src/audio/EngineTypes.h src/audio/DeviceId.h src/audio/DeviceId.cpp src/audio/ModelDetect.cpp src/audio/ClockBridge.cpp src/audio/HealthMonitor.cpp src/audio/DeviceManager.cpp src/audio/AudioEngine.cpp tests/test_deviceid.cpp
  git commit -m "Plan 2 Task 1: eb_engine scaffold, EngineTypes, DeviceId"
  ```

---

### Task 2 — Model detection + per-model rate/bit-depth mapping (pure, mockable)

The hardware-free heart of the dual-device support: detect `EarsModel` from a device-name string (and an optional USB id), and map the model to its native sample-rate and bit-depth whitelists. Kept pure so it is unit-tested without any device.

**Files**
- Create: `src/audio/ModelDetect.h`
- Modify: `src/audio/ModelDetect.cpp`
- Test: `tests/test_modeldetect.cpp`

**Steps**

- [ ] **Write the header.** Create `src/audio/ModelDetect.h`:
  ```cpp
  #pragma once
  #include <juce_core/juce_core.h>
  #include "audio/EngineTypes.h"
  #include <vector>
  namespace eb {

  // Detect the EARS model from a device display name and an optional USB id string
  // (VID:PID or hardware-id, as the OS reports it). Pure; no device access.
  EarsModel detectEarsModel (const juce::String& deviceName,
                             const juce::String& usbId = {});

  // Per-model native whitelists. Unknown -> empty (caller must not offer rates).
  std::vector<double> nativeSampleRates (EarsModel model);   // EARS -> {48000}; EARS Pro -> {44100,48000,88200,96000,176400,192000}
  std::vector<int>    nativeBitDepths   (EarsModel model);   // EARS -> {24}; EARS Pro -> {16,24,32}

  } // namespace eb
  ```

- [ ] **Write the failing test.** Create `tests/test_modeldetect.cpp`:
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include "audio/ModelDetect.h"
  using eb::EarsModel;

  TEST_CASE("detectEarsModel: EARS Pro recognised before plain EARS") {
      CHECK (eb::detectEarsModel ("EARS Pro")            == EarsModel::EarsPro);
      CHECK (eb::detectEarsModel ("miniDSP EARS Pro")    == EarsModel::EarsPro);
      CHECK (eb::detectEarsModel ("MINIDSP EARSPRO USB") == EarsModel::EarsPro);
  }

  TEST_CASE("detectEarsModel: original EARS") {
      CHECK (eb::detectEarsModel ("EARS")        == EarsModel::Ears);
      CHECK (eb::detectEarsModel ("miniDSP EARS") == EarsModel::Ears);
      CHECK (eb::detectEarsModel ("Microphone (EARS)") == EarsModel::Ears);
  }

  TEST_CASE("detectEarsModel: USB VID:PID promotes a generic name") {
      // miniDSP VID 2752; EARS Pro XMOS interface reported with a bare name.
      CHECK (eb::detectEarsModel ("USB Audio Device", "VID_2752&PID_0046") == EarsModel::EarsPro);
      CHECK (eb::detectEarsModel ("USB Audio Device", "VID_2752&PID_0011") == EarsModel::Ears);
  }

  TEST_CASE("detectEarsModel: unrelated devices are Unknown") {
      CHECK (eb::detectEarsModel ("Realtek High Definition Audio") == EarsModel::Unknown);
      CHECK (eb::detectEarsModel ("CABLE Output (VB-Audio Virtual Cable)") == EarsModel::Unknown);
  }

  TEST_CASE("native rate/bit-depth whitelists per model") {
      CHECK (eb::nativeSampleRates (EarsModel::Ears)    == std::vector<double>{48000});
      CHECK (eb::nativeSampleRates (EarsModel::EarsPro) ==
             std::vector<double>{44100,48000,88200,96000,176400,192000});
      CHECK (eb::nativeBitDepths (EarsModel::Ears)    == std::vector<int>{24});
      CHECK (eb::nativeBitDepths (EarsModel::EarsPro) == std::vector<int>{16,24,32});
      CHECK (eb::nativeSampleRates (EarsModel::Unknown).empty());
      CHECK (eb::nativeBitDepths   (EarsModel::Unknown).empty());
  }
  ```

- [ ] **Register the test.** Add `test_modeldetect.cpp` to the `add_executable(eb_tests ...)` list in `tests/CMakeLists.txt`.

- [ ] **Run it — expect FAIL.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  ```
  Expected: link error — `undefined reference to eb::detectEarsModel` / `eb::nativeSampleRates` (the stub `ModelDetect.cpp` defines nothing).

- [ ] **Implement.** Replace the contents of `src/audio/ModelDetect.cpp` with:
  ```cpp
  #include "audio/ModelDetect.h"
  namespace eb {

  EarsModel detectEarsModel (const juce::String& deviceName, const juce::String& usbId) {
      const auto name = deviceName.toLowerCase();
      const auto usb  = usbId.toUpperCase();

      // USB identity is the most reliable signal. miniDSP VID = 2752 (0x0AC0).
      // EARS Pro is the XMOS UAC2 interface; original EARS is the UAC1 interface.
      if (usb.contains ("VID_2752")) {
          if (usb.contains ("PID_0046") || usb.contains ("PRO")) return EarsModel::EarsPro;
          return EarsModel::Ears;
      }

      // Name-based fallback. Check "pro" first so "EARS Pro" never falls through to Ears.
      const bool mentionsEars = name.contains ("ears");
      if (mentionsEars && (name.contains ("ears pro") || name.contains ("earspro")
                            || name.replace (" ", "").contains ("earspro")))
          return EarsModel::EarsPro;
      if (mentionsEars)
          return EarsModel::Ears;

      return EarsModel::Unknown;
  }

  std::vector<double> nativeSampleRates (EarsModel model) {
      switch (model) {
          case EarsModel::Ears:    return {48000.0};
          case EarsModel::EarsPro: return {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
          case EarsModel::Unknown: default: return {};
      }
  }

  std::vector<int> nativeBitDepths (EarsModel model) {
      switch (model) {
          case EarsModel::Ears:    return {24};
          case EarsModel::EarsPro: return {16, 24, 32};
          case EarsModel::Unknown: default: return {};
      }
  }

  } // namespace eb
  ```

- [ ] **Run — expect PASS.**
  ```
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R modeldetect"
  ```
  Expected: the five `detectEarsModel` / whitelist cases pass.
  (ctest filter `-R modeldetect` matches Catch test-case names containing that token; if your discovered names differ, run without `-R` and confirm all green.)

- [ ] **Commit.**
  ```
  git add src/audio/ModelDetect.h src/audio/ModelDetect.cpp tests/test_modeldetect.cpp tests/CMakeLists.txt
  git commit -m "Plan 2 Task 2: EarsModel detection + per-model native rate/bit-depth mapping"
  ```

---

### Task 3 — ClockBridge: lock-free FIFO + async SRC + PI fill-control

The core real-time component: an `AbstractFifo`-backed mono ring buffer written by the capture callback and read by the render callback, with a `LagrangeInterpolator` doing asynchronous SRC whose ratio is trimmed by a slow PI loop on smoothed FIFO fill. Fully unit-testable by driving producer/consumer at different rates.

**Files**
- Create: `src/audio/ClockBridge.h`
- Modify: `src/audio/ClockBridge.cpp`
- Test: `tests/test_clockbridge.cpp`

**Steps**

- [ ] **Write the header.** Create `src/audio/ClockBridge.h`:
  ```cpp
  #pragma once
  #include <juce_audio_basics/juce_audio_basics.h>
  #include <juce_core/juce_core.h>
  #include <atomic>
  #include <vector>
  namespace eb {

  // Single-producer (capture cb) / single-consumer (render cb) mono bridge across two
  // free-running clocks. pushCapture() writes capture-rate samples into a lock-free FIFO;
  // pullRender() reads render-rate samples out through an async SRC whose ratio is the
  // nominal captureRate/renderRate trimmed by a PI loop on smoothed fill. No alloc/lock
  // on either path after prepare().
  class ClockBridge {
  public:
      void prepare (double captureRate, double renderRate, int channels, int capacityFrames);
      void pushCapture (const float* mono, int numFrames);  // producer
      int  pullRender  (float* out, int numFrames);          // consumer; returns frames written
      void setRenderRate (double);
      void reset();

      double fifoFill()  const;   // 0..1 fraction of capacity currently buffered (smoothed)
      int    underruns() const;
      int    overruns()  const;
      double currentRatio() const;   // current trimmed capture:render resample ratio

  private:
      // FIFO of mono float samples.
      juce::AbstractFifo fifo { 1 };
      std::vector<float> ring;            // capacity samples
      int capacity = 0;

      // SRC consumer-side scratch + interpolator.
      juce::LagrangeInterpolator src;
      std::vector<float> srcInput;        // drained from FIFO, fed to interpolator
      double captureRate = 48000.0, renderRate = 48000.0;

      // PI fill-control state (consumer thread only).
      double smoothedFill = 0.5;          // fraction
      double ratioTrim    = 1.0;          // multiplies nominal ratio
      double integ        = 0.0;

      std::atomic<int>    underrunCount { 0 };
      std::atomic<int>    overrunCount  { 0 };
      std::atomic<double> publishedFill  { 0.5 };
      std::atomic<double> publishedRatio { 1.0 };   // last trimmed capture:render ratio (for currentRatio())
  };

  } // namespace eb
  ```

- [ ] **Write the failing test.** Create `tests/test_clockbridge.cpp`:
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include <catch2/matchers/catch_matchers_floating_point.hpp>
  #include "audio/ClockBridge.h"
  #include <cmath>
  #include <vector>

  using Catch::Matchers::WithinAbs;

  // Drive producer at captureRate and consumer at renderRate over many blocks, pushing a
  // pure sine at f0 (defined in capture time). Returns peak |error| of the rendered sine vs
  // an ideal render-time sine, after a warm-up, plus fills bridge stats by reference.
  static double runBridge (eb::ClockBridge& cb, double captureRate, double renderRate,
                           double f0, int capBlock, int renderBlock, int renderBlocks,
                           int& underruns, int& overruns) {
      // Pre-fill to ~half capacity so the consumer never starts starved.
      std::vector<float> pre (capBlock, 0.0f);
      double phase = 0.0, dPhaseCap = 2.0 * juce::MathConstants<double>::pi * f0 / captureRate;
      // capture/render sample budget: produce ~ renderBlocks*renderBlock*(cap/render) capture samples.
      double capPerRender = captureRate / renderRate;
      int totalRender = renderBlocks * renderBlock;
      int totalCapture = (int) std::ceil (totalRender * capPerRender) + 4 * capBlock;

      // Producer queue of capture samples (generated up front; pushed in capBlock chunks on demand).
      std::vector<float> cap (totalCapture);
      for (int i = 0; i < totalCapture; ++i) { cap[i] = (float) std::sin (phase); phase += dPhaseCap; }

      int capPos = 0;
      auto pushSome = [&](int frames) {
          while (frames > 0 && capPos + capBlock <= totalCapture) {
              cb.pushCapture (cap.data() + capPos, capBlock);
              capPos += capBlock; frames -= capBlock;
          }
      };
      // Prime FIFO.
      pushSome (8 * capBlock);

      std::vector<float> out (renderBlock, 0.0f);
      double maxErr = 0.0;
      double rPhase = 0.0, dPhaseRen = 2.0 * juce::MathConstants<double>::pi * f0 / renderRate;
      int produced = 0;
      for (int b = 0; b < renderBlocks; ++b) {
          // Keep FIFO fed in proportion to consumption.
          pushSome ((int) std::ceil (renderBlock * capPerRender) + capBlock);
          int got = cb.pullRender (out.data(), renderBlock);
          for (int i = 0; i < got; ++i) {
              double ideal = std::sin (rPhase); rPhase += dPhaseRen;
              if (b > 16)  // ignore warm-up / group delay region
                  maxErr = std::max (maxErr, std::abs ((double) out[i] - ideal));
          }
          produced += got;
      }
      underruns = cb.underruns(); overruns = cb.overruns();
      return maxErr;
  }

  TEST_CASE("ClockBridge: equal nominal rates, no under/overflow, bounded fill") {
      eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
      int u = 0, o = 0;
      double err = runBridge (cb, 48000.0, 48000.0, 997.0, 256, 256, 400, u, o);
      INFO ("maxErr=" << err << " under=" << u << " over=" << o << " fill=" << cb.fifoFill());
      CHECK (u == 0);
      CHECK (o == 0);
      CHECK (cb.fifoFill() > 0.05);
      CHECK (cb.fifoFill() < 0.95);
      CHECK (err < 0.05);   // async SRC in-band transparent at 997 Hz
  }

  TEST_CASE("ClockBridge: 96k capture -> 48k render downsample passes a sine") {
      eb::ClockBridge cb; cb.prepare (96000.0, 48000.0, 1, 16384);
      int u = 0, o = 0;
      double err = runBridge (cb, 96000.0, 48000.0, 997.0, 256, 256, 400, u, o);
      INFO ("maxErr=" << err << " under=" << u << " over=" << o << " fill=" << cb.fifoFill());
      CHECK (u == 0);
      CHECK (o == 0);
      CHECK (cb.fifoFill() > 0.05);
      CHECK (cb.fifoFill() < 0.95);
      CHECK (err < 0.08);
  }

  TEST_CASE("ClockBridge: small drift is absorbed by the fill-control loop") {
      // Consumer slightly faster than producer nominal: render clock = 48010 Hz vs 48000.
      eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
      int u = 0, o = 0;
      // Model drift by telling the bridge the render rate is the true (drifted) rate so the
      // PI loop must trim; producer still emits at 48000.
      cb.setRenderRate (48010.0);
      double err = runBridge (cb, 48000.0, 48010.0, 997.0, 256, 256, 600, u, o);
      INFO ("ratioTrim fill=" << cb.fifoFill() << " under=" << u << " over=" << o);
      CHECK (u == 0);
      CHECK (o == 0);
      CHECK (cb.fifoFill() > 0.05);
      CHECK (cb.fifoFill() < 0.95);
  }

  TEST_CASE("ClockBridge: reset clears stats and fill") {
      eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 4096);
      std::vector<float> z (256, 0.0f), out (256, 0.0f);
      for (int i = 0; i < 4; ++i) cb.pushCapture (z.data(), 256);
      cb.pullRender (out.data(), 256);
      cb.reset();
      CHECK (cb.underruns() == 0);
      CHECK (cb.overruns()  == 0);
  }
  ```

- [ ] **Register the test.** Add `test_clockbridge.cpp` to `add_executable(eb_tests ...)` in `tests/CMakeLists.txt`.

- [ ] **Run it — expect FAIL.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  ```
  Expected: link error — `ClockBridge::prepare`, `pushCapture`, `pullRender`, etc. unresolved (stub `.cpp`).

- [ ] **Implement.** Replace the contents of `src/audio/ClockBridge.cpp` with:
  ```cpp
  #include "audio/ClockBridge.h"
  #include <algorithm>
  #include <cmath>
  namespace eb {

  void ClockBridge::prepare (double capRate, double renRate, int /*channels*/, int capacityFrames) {
      captureRate = capRate; renderRate = renRate;
      capacity = juce::jmax (1024, capacityFrames);
      ring.assign ((size_t) capacity, 0.0f);
      fifo.setTotalSize (capacity);
      fifo.reset();
      // Worst-case SRC needs ceil(ratio*numOut)+a few guard samples; size generously.
      srcInput.assign ((size_t) capacity, 0.0f);
      src.reset();
      smoothedFill = 0.5; ratioTrim = 1.0; integ = 0.0;
      underrunCount.store (0); overrunCount.store (0); publishedFill.store (0.5);
      publishedRatio.store (captureRate / juce::jmax (1.0, renderRate));
  }

  void ClockBridge::setRenderRate (double r) { renderRate = r; }

  void ClockBridge::reset() {
      fifo.reset();
      std::fill (ring.begin(), ring.end(), 0.0f);
      src.reset();
      smoothedFill = 0.5; ratioTrim = 1.0; integ = 0.0;
      underrunCount.store (0); overrunCount.store (0); publishedFill.store (0.5);
      publishedRatio.store (captureRate / juce::jmax (1.0, renderRate));
  }

  void ClockBridge::pushCapture (const float* mono, int numFrames) {
      int free = fifo.getFreeSpace();
      if (numFrames > free) {                       // ring full: drop oldest by overwriting policy
          overrunCount.fetch_add (1);
          numFrames = free;                         // write only what fits; lock-free, no alloc
      }
      int s1, sz1, s2, sz2;
      fifo.prepareToWrite (numFrames, s1, sz1, s2, sz2);
      if (sz1 > 0) juce::FloatVectorOperations::copy (ring.data() + s1, mono, sz1);
      if (sz2 > 0) juce::FloatVectorOperations::copy (ring.data() + s2, mono + sz1, sz2);
      fifo.finishedWrite (sz1 + sz2);
  }

  int ClockBridge::pullRender (float* out, int numFrames) {
      // --- PI fill-control: target half-full; trim the SRC ratio slowly. ---
      const double fillFrac = (double) fifo.getNumReady() / (double) capacity;
      smoothedFill += 0.01 * (fillFrac - smoothedFill);        // 1-pole smoother
      const double errFill = smoothedFill - 0.5;               // want 0.5
      integ = juce::jlimit (-0.02, 0.02, integ + 1.0e-4 * errFill);
      // If fill is high, consume faster (ratio up); if low, slower. Bound the trim tightly.
      ratioTrim = juce::jlimit (0.97, 1.03, 1.0 + (2.0e-2 * errFill + integ));
      publishedFill.store (smoothedFill);

      const double nominal = captureRate / renderRate;          // input samples per output sample (== speedRatio)
      const double ratio   = nominal * ratioTrim;
      publishedRatio.store (ratio);                              // expose to currentRatio() (lock-free)

      // Input samples the interpolator MAY need for numFrames outputs (upper bound).
      int needIn = (int) std::ceil (ratio * numFrames) + 4;
      needIn = juce::jmin (needIn, (int) srcInput.size());
      const int avail = fifo.getNumReady();
      const int toRead = juce::jmin (needIn, avail);

      // Peek toRead samples into the scratch buffer WITHOUT advancing the read pointer yet.
      int s1, sz1, s2, sz2;
      fifo.prepareToRead (toRead, s1, sz1, s2, sz2);
      if (sz1 > 0) juce::FloatVectorOperations::copy (srcInput.data(), ring.data() + s1, sz1);
      if (sz2 > 0) juce::FloatVectorOperations::copy (srcInput.data() + sz1, ring.data() + s2, sz2);

      if (toRead < needIn)
          underrunCount.fetch_add (1);   // FIFO starved: interpolator zero-pads the tail (wrapAround = 0)

      // process() RETURNS the actual number of input samples consumed (~ratio*numFrames, NOT toRead).
      // Advance the FIFO by exactly that, so the stateful interpolator's input stays continuous and the
      // FIFO drains at the true rate (advancing by toRead would skip samples and slowly empty the FIFO).
      const int usedIn = src.process (ratio, srcInput.data(), out, numFrames, toRead, 0);
      fifo.finishedRead (juce::jmin (usedIn, toRead));
      return numFrames;
  }

  double ClockBridge::fifoFill()  const { return publishedFill.load(); }
  int    ClockBridge::underruns() const { return underrunCount.load(); }
  int    ClockBridge::overruns()  const { return overrunCount.load(); }
  double ClockBridge::currentRatio() const { return publishedRatio.load(); }

  } // namespace eb
  ```

- [ ] **Run — expect PASS.**
  ```
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R ClockBridge"
  ```
  Expected: all four `ClockBridge` cases pass. If `err` is marginally over a threshold on your toolchain, the `INFO` line prints the actual value — only widen the bound if the failure is a small SRC tolerance (< ~20%), never to mask an under/overrun (those must stay 0).

- [ ] **Commit.**
  ```
  git add src/audio/ClockBridge.h src/audio/ClockBridge.cpp tests/test_clockbridge.cpp tests/CMakeLists.txt
  git commit -m "Plan 2 Task 3: ClockBridge lock-free FIFO + async SRC + PI fill-control"
  ```

---

### Task 4 — HealthMonitor: atomic xrun/slip/level telemetry + clean-capture latch

Aggregates counters from both audio threads into a single lock-free `Health` snapshot for the GUI, and latches `cleanCapture=false` the first time anything goes wrong. Level/clip tracking lives here too so the engine just feeds it numbers.

**Files**
- Create: `src/audio/HealthMonitor.h`
- Modify: `src/audio/HealthMonitor.cpp`
- Test: `tests/test_healthmonitor.cpp`

**Steps**

- [ ] **Write the header.** Create `src/audio/HealthMonitor.h`:
  ```cpp
  #pragma once
  #include "audio/EngineTypes.h"
  #include <atomic>
  namespace eb {

  // Lock-free telemetry sink written from both audio callbacks (atomics only) and read by
  // the GUI timer via snapshot()/levels(). cleanCapture latches false on the first fault.
  class HealthMonitor {
  public:
      void reset();

      // Called from audio threads (atomic stores/increments only).
      void reportXrun();                       // a device xrun / dropped callback
      void reportDroppedFrames (long long n);  // frames lost at the bridge (under/overrun)
      void setFifoFill (double frac);
      void setCaptureToRenderRatio (double r);
      void reportInLevels (float peakL, float peakR, bool clipL, bool clipR);
      void reportOutLevel  (float peakMono, bool clipOut);

      // Read from the GUI thread.
      Health snapshot() const;
      Levels levels()  const;

  private:
      std::atomic<int>       xruns { 0 };
      std::atomic<long long> dropped { 0 };
      std::atomic<int>       fifoFillMilli { 500 };   // fill * 1000
      std::atomic<int>       ratioMicro { 1000000 };  // ratio * 1e6
      std::atomic<bool>      clean { true };

      std::atomic<int> inLm { 0 }, inRm { 0 }, outM { 0 };       // peak * 1000
      std::atomic<bool> cL { false }, cR { false }, cO { false };
  };

  } // namespace eb
  ```

- [ ] **Write the failing test.** Create `tests/test_healthmonitor.cpp`:
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include <catch2/matchers/catch_matchers_floating_point.hpp>
  #include "audio/HealthMonitor.h"
  using Catch::Matchers::WithinAbs;

  TEST_CASE("HealthMonitor: counters accumulate and snapshot reflects them") {
      eb::HealthMonitor h; h.reset();
      h.reportXrun(); h.reportXrun();
      h.reportDroppedFrames (128);
      h.reportDroppedFrames (64);
      h.setFifoFill (0.42);
      h.setCaptureToRenderRatio (2.0);
      auto s = h.snapshot();
      CHECK (s.xruns == 2);
      CHECK (s.droppedFrames == 192);
      CHECK_THAT (s.fifoFill, WithinAbs (0.42, 1e-3));
      CHECK_THAT (s.captureToRenderRatio, WithinAbs (2.0, 1e-3));
  }

  TEST_CASE("HealthMonitor: cleanCapture latches false on first fault and stays false") {
      eb::HealthMonitor h; h.reset();
      CHECK (h.snapshot().cleanCapture);
      h.reportXrun();
      CHECK_FALSE (h.snapshot().cleanCapture);
      h.reset();                          // a fresh run clears the latch
      CHECK (h.snapshot().cleanCapture);
      h.reportDroppedFrames (1);
      CHECK_FALSE (h.snapshot().cleanCapture);
  }

  TEST_CASE("HealthMonitor: levels round-trip and clip flags") {
      eb::HealthMonitor h; h.reset();
      h.reportInLevels (0.5f, 0.25f, false, true);
      h.reportOutLevel (0.8f, false);
      auto lv = h.levels();
      CHECK_THAT (lv.inL, WithinAbs (0.5f, 2e-3));
      CHECK_THAT (lv.inR, WithinAbs (0.25f, 2e-3));
      CHECK_THAT (lv.outMono, WithinAbs (0.8f, 2e-3));
      CHECK_FALSE (lv.clipL);
      CHECK (lv.clipR);
      CHECK_FALSE (lv.clipOut);
  }
  ```

- [ ] **Register the test.** Add `test_healthmonitor.cpp` to `add_executable(eb_tests ...)` in `tests/CMakeLists.txt`.

- [ ] **Run it — expect FAIL.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  ```
  Expected: link error — `HealthMonitor::reset` / `reportXrun` / `snapshot` unresolved.

- [ ] **Implement.** Replace the contents of `src/audio/HealthMonitor.cpp` with:
  ```cpp
  #include "audio/HealthMonitor.h"
  #include <algorithm>
  #include <cmath>
  namespace eb {

  void HealthMonitor::reset() {
      xruns.store (0); dropped.store (0);
      fifoFillMilli.store (500); ratioMicro.store (1000000);
      clean.store (true);
      inLm.store (0); inRm.store (0); outM.store (0);
      cL.store (false); cR.store (false); cO.store (false);
  }

  void HealthMonitor::reportXrun() { xruns.fetch_add (1); clean.store (false); }

  void HealthMonitor::reportDroppedFrames (long long n) {
      if (n > 0) { dropped.fetch_add (n); clean.store (false); }
  }

  void HealthMonitor::setFifoFill (double frac) {
      fifoFillMilli.store ((int) std::lround (juce::jlimit (0.0, 1.0, frac) * 1000.0));
  }

  void HealthMonitor::setCaptureToRenderRatio (double r) {
      ratioMicro.store ((int) std::lround (r * 1.0e6));
  }

  void HealthMonitor::reportInLevels (float peakL, float peakR, bool clipL, bool clipR) {
      inLm.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakL) * 1000.0f));
      inRm.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakR) * 1000.0f));
      cL.store (clipL); cR.store (clipR);
  }

  void HealthMonitor::reportOutLevel (float peakMono, bool clipOut) {
      outM.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakMono) * 1000.0f));
      cO.store (clipOut);
  }

  Health HealthMonitor::snapshot() const {
      Health h;
      h.xruns = xruns.load();
      h.droppedFrames = dropped.load();
      h.fifoFill = fifoFillMilli.load() / 1000.0;
      h.captureToRenderRatio = ratioMicro.load() / 1.0e6;
      h.cleanCapture = clean.load();
      return h;
  }

  Levels HealthMonitor::levels() const {
      Levels lv;
      lv.inL = inLm.load() / 1000.0f;
      lv.inR = inRm.load() / 1000.0f;
      lv.outMono = outM.load() / 1000.0f;
      lv.clipL = cL.load(); lv.clipR = cR.load(); lv.clipOut = cO.load();
      return lv;
  }

  } // namespace eb
  ```

- [ ] **Run — expect PASS.**
  ```
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R HealthMonitor"
  ```
  Expected: the three `HealthMonitor` cases pass.

- [ ] **Commit.**
  ```
  git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp tests/CMakeLists.txt
  git commit -m "Plan 2 Task 4: HealthMonitor atomic telemetry + clean-capture latch"
  ```

---

### Task 5 — DeviceManager: enumeration, model/rate/bit-depth detection, open/close

Wraps JUCE's `AudioIODeviceType` enumeration. Lists input/output endpoints as `DeviceId`s, tags EARS/EARS-Pro (via `ModelDetect`) and virtual sinks, reports per-device native rates/bit-depths (model whitelist intersected with the device's actual `getAvailableSampleRates`), and opens a capture device + a render device as two separate WASAPI/CoreAudio contexts. The pure helpers (virtual-sink tagging, rate intersection) are unit-tested; live enumeration/open is exercised by the diagnostic in Task 7.

**Files**
- Create: `src/audio/DeviceManager.h`
- Modify: `src/audio/DeviceManager.cpp`
- Test: `tests/test_devicemanager.cpp`

**Steps**

- [ ] **Write the header.** Create `src/audio/DeviceManager.h`:
  ```cpp
  #pragma once
  #include <juce_audio_devices/juce_audio_devices.h>
  #include "audio/DeviceId.h"
  #include <memory>
  #include <vector>
  namespace eb {

  // Enumerates and opens the two device contexts. The render device is the master clock;
  // the capture device feeds the ClockBridge. Uses WASAPI ("Windows Audio") on Windows and
  // CoreAudio on macOS — never ASIO (cannot pair separate in/out devices).
  class DeviceManager {
  public:
      DeviceManager();

      // Preferred cross-device driver type name for the current OS.
      static juce::String preferredTypeName();   // "Windows Audio" / "CoreAudio"

      // Returns true if the named device type can pair a capture device with a different
      // render device (i.e. NOT ASIO). Pure check on the type object.
      static bool typeSupportsSeparateIO (const juce::AudioIODeviceType& type);

      // ---- ASIO-fallback seam (consumed by Plan 4's AsioFallback path) ----
      // Capability view of one driver type: its name and whether it can pair separate in/out
      // devices. Mirrors Plan 4's eb::DeviceTypeCaps shape without depending on that header.
      struct TypeCaps { juce::String typeName; bool separateInputsAndOutputs = true; };
      // All driver types JUCE enumerated (name + separate-I/O capability). Lets the engine build
      // the AsioFallback decision without re-walking AudioDeviceManager internals. Non-const
      // because JUCE's AudioDeviceManager::getAvailableDeviceTypes() is itself non-const.
      std::vector<TypeCaps> availableTypeCaps();
      // Caps for the type the engine would currently use (preferredTypeName(), or the override).
      TypeCaps currentTypeCaps();
      // Force the active driver type by name for the open path (used when AsioFallback decides to
      // fall back off ASIO to "Windows Audio"/"CoreAudio"). Persists until the next call/reset.
      void setCurrentType (const juce::String& typeName);

      // Heuristic: is this output device name a known/likely virtual sink?
      static bool looksLikeVirtualSink (const juce::String& name);

      void rescan();                                   // populate the cached lists
      std::vector<DeviceId> inputs()  const { return inputList; }
      std::vector<DeviceId> outputs() const { return outputList; }

      // Native rates/bit-depths for an input: model whitelist (ModelDetect) intersected with
      // the device's actually-supported rates when the device can be queried; falls back to
      // the pure whitelist when no live device is available (headless/test).
      std::vector<double> nativeRatesFor   (const DeviceId&) const;
      std::vector<int>    nativeBitDepthsFor (const DeviceId&) const;

      // Open the input (capture) and output (render) devices on the preferred type.
      // Returns empty string on success, else an error message. Channels: input opens
      // 2 channels (L/R ears); output opens 2 channels (dual-mono). Does NOT start callbacks.
      // requestedOutputBits (16/24/32) is requested best-effort on the render format; JUCE
      // shared-mode float may not honor an arbitrary integer depth, so the open falls back to
      // the nearest supported format. requestedOutputBitDepth() returns what was asked for.
      juce::String openInput  (const DeviceId&, double sampleRate, int bufferSize);
      juce::String openOutput (const DeviceId&, double sampleRate, int bufferSize,
                               int requestedOutputBits = 24);
      int requestedOutputBitDepth() const { return requestedOutBits; }

      juce::AudioIODevice* inputDevice()  const { return inDev.get(); }
      juce::AudioIODevice* outputDevice() const { return outDev.get(); }

      void closeAll();

  private:
      juce::AudioDeviceManager adm;     // owns the type list / scanning
      std::vector<DeviceId> inputList, outputList;
      std::unique_ptr<juce::AudioIODevice> inDev, outDev;
      int requestedOutBits = 24;        // last bit depth requested on openOutput (best-effort)
      juce::String forcedTypeName;      // set by setCurrentType() to override the preferred type

      juce::AudioIODeviceType* findPreferredType();    // the "Windows Audio"/"CoreAudio" type
  };

  } // namespace eb
  ```

- [ ] **Write the failing test.** Create `tests/test_devicemanager.cpp` (pure helpers only — no live device needed):
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include "audio/DeviceManager.h"

  TEST_CASE("preferredTypeName is WASAPI on Windows / CoreAudio on macOS") {
      auto t = eb::DeviceManager::preferredTypeName();
     #if JUCE_WINDOWS
      CHECK (t == juce::String ("Windows Audio"));
     #elif JUCE_MAC
      CHECK (t == juce::String ("CoreAudio"));
     #else
      CHECK (t.isNotEmpty());
     #endif
  }

  TEST_CASE("looksLikeVirtualSink tags known virtual cables, not real hardware") {
      using DM = eb::DeviceManager;
      CHECK (DM::looksLikeVirtualSink ("CABLE Input (VB-Audio Virtual Cable)"));
      CHECK (DM::looksLikeVirtualSink ("VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"));
      CHECK (DM::looksLikeVirtualSink ("BlackHole 2ch"));
      CHECK (DM::looksLikeVirtualSink ("Loopback Audio"));
      CHECK_FALSE (DM::looksLikeVirtualSink ("Speakers (Realtek High Definition Audio)"));
      CHECK_FALSE (DM::looksLikeVirtualSink ("miniDSP EARS"));
  }

  TEST_CASE("nativeRatesFor falls back to the model whitelist for an unopened device") {
      eb::DeviceManager dm;
      eb::DeviceId pro; pro.name = "miniDSP EARS Pro"; pro.model = eb::EarsModel::EarsPro;
      auto rates = dm.nativeRatesFor (pro);
      CHECK (rates == std::vector<double>{44100,48000,88200,96000,176400,192000});
      auto bits = dm.nativeBitDepthsFor (pro);
      CHECK (bits == std::vector<int>{16,24,32});

      eb::DeviceId ears; ears.name = "miniDSP EARS"; ears.model = eb::EarsModel::Ears;
      CHECK (dm.nativeRatesFor (ears) == std::vector<double>{48000});
      CHECK (dm.nativeBitDepthsFor (ears) == std::vector<int>{24});
  }
  ```

- [ ] **Register the test.** Add `test_devicemanager.cpp` to `add_executable(eb_tests ...)` in `tests/CMakeLists.txt`.

- [ ] **Run it — expect FAIL.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  ```
  Expected: link error — `DeviceManager::preferredTypeName` / `looksLikeVirtualSink` / `nativeRatesFor` unresolved.

- [ ] **Implement.** Replace the contents of `src/audio/DeviceManager.cpp` with:
  ```cpp
  #include "audio/DeviceManager.h"
  #include "audio/ModelDetect.h"
  #include <algorithm>
  namespace eb {

  DeviceManager::DeviceManager() {
      // Populate adm's internal type list without opening any device. After this,
      // adm.getAvailableDeviceTypes() returns the managed type objects.
      juce::OwnedArray<juce::AudioIODeviceType> tmp;
      adm.createAudioDeviceTypes (tmp);
  }

  juce::String DeviceManager::preferredTypeName() {
     #if JUCE_WINDOWS
      return "Windows Audio";   // WASAPI; never the ASIO type for the cross-device split
     #elif JUCE_MAC
      return "CoreAudio";
     #else
      return "ALSA";
     #endif
  }

  bool DeviceManager::typeSupportsSeparateIO (const juce::AudioIODeviceType& type) {
      return type.hasSeparateInputsAndOutputs();
  }

  bool DeviceManager::looksLikeVirtualSink (const juce::String& name) {
      const auto n = name.toLowerCase();
      static const char* tokens[] = { "cable", "vb-audio", "voicemeeter", "blackhole",
                                      "loopback", "virtual", "soundflower" };
      for (auto* t : tokens) if (n.contains (t)) return true;
      return false;
  }

  juce::AudioIODeviceType* DeviceManager::findPreferredType() {
      // An explicit override (from setCurrentType(), e.g. the ASIO->WASAPI fallback) wins.
      const auto want = forcedTypeName.isNotEmpty() ? forcedTypeName : preferredTypeName();
      for (auto* t : adm.getAvailableDeviceTypes())
          if (t->getTypeName() == want) return t;
      // Fallback: first type that can pair separate in/out (skip ASIO).
      for (auto* t : adm.getAvailableDeviceTypes())
          if (t->hasSeparateInputsAndOutputs()) return t;
      return nullptr;
  }

  std::vector<DeviceManager::TypeCaps> DeviceManager::availableTypeCaps() {
      std::vector<TypeCaps> caps;
      for (auto* t : adm.getAvailableDeviceTypes())
          caps.push_back ({ t->getTypeName(), t->hasSeparateInputsAndOutputs() });
      return caps;
  }

  DeviceManager::TypeCaps DeviceManager::currentTypeCaps() {
      const auto want = forcedTypeName.isNotEmpty() ? forcedTypeName : preferredTypeName();
      for (auto* t : adm.getAvailableDeviceTypes())
          if (t->getTypeName() == want)
              return { t->getTypeName(), t->hasSeparateInputsAndOutputs() };
      return { want, false };   // not found: report as not-separate so the fallback path engages
  }

  void DeviceManager::setCurrentType (const juce::String& typeName) {
      forcedTypeName = typeName;   // honored by findPreferredType() on the next open
  }

  // Best-effort stable endpoint id for a device. JUCE's AudioIODeviceType exposes only
  // display names (getDeviceNames); it does NOT surface a persistent endpoint GUID/UID
  // through this API on Windows ("Windows Audio") or macOS ("CoreAudio"). So today this
  // returns the name itself and uid==name. That is a KNOWN LIMITATION: key() is therefore
  // name-stable, NOT replug-stable, and CalBinder keys are name-stable only. If a future
  // build resolves a real endpoint id (e.g. via a WASAPI IMMDevice GetId() or the CoreAudio
  // kAudioDevicePropertyDeviceUID side-channel), populate it here and the key() upgrades to
  // replug-stable with no other change. Do NOT claim replug-stability while this returns name.
  static juce::String stableUidFor (const juce::String& name) {
      return name;   // name-stable only; see the limitation above
  }

  void DeviceManager::rescan() {
      inputList.clear(); outputList.clear();
      auto* type = findPreferredType();
      if (type == nullptr) return;
      type->scanForDevices();
      const auto typeName = type->getTypeName();

      for (auto& name : type->getDeviceNames (true)) {   // true => input devices
          DeviceId d; d.typeName = typeName; d.name = name; d.uid = stableUidFor (name);
          d.model = detectEarsModel (name);
          inputList.push_back (d);
      }
      for (auto& name : type->getDeviceNames (false)) {  // false => output devices
          DeviceId d; d.typeName = typeName; d.name = name; d.uid = stableUidFor (name);
          d.isVirtualSink = looksLikeVirtualSink (name);
          outputList.push_back (d);
      }
  }

  std::vector<double> DeviceManager::nativeRatesFor (const DeviceId& id) const {
      auto whitelist = nativeSampleRates (id.model);
      // If we have a live, matching open input device, intersect with its real rates.
      if (inDev != nullptr && inDev->getName() == id.name) {
          auto avail = inDev->getAvailableSampleRates();
          std::vector<double> out;
          for (double r : whitelist)
              if (std::any_of (avail.begin(), avail.end(),
                               [r](double a){ return std::abs (a - r) < 1.0; }))
                  out.push_back (r);
          if (! out.empty()) return out;
      }
      return whitelist;   // headless / not-open fallback = pure model whitelist
  }

  std::vector<int> DeviceManager::nativeBitDepthsFor (const DeviceId& id) const {
      return nativeBitDepths (id.model);
  }

  juce::String DeviceManager::openInput (const DeviceId& id, double sampleRate, int bufferSize) {
      auto* type = findPreferredType();
      if (type == nullptr) return "No suitable audio driver type (WASAPI/CoreAudio) found";
      type->scanForDevices();
      inDev.reset (type->createDevice ({}, id.name));   // input-only
      if (inDev == nullptr) return "Could not create input device: " + id.name;
      juce::BigInteger inCh; inCh.setRange (0, 2, true);     // 2 ears
      juce::BigInteger noOut;
      auto err = inDev->open (inCh, noOut, sampleRate, bufferSize);
      if (err.isNotEmpty()) { inDev.reset(); return "Open input failed: " + err; }
      return {};
  }

  juce::String DeviceManager::openOutput (const DeviceId& id, double sampleRate, int bufferSize,
                                          int requestedOutputBits) {
      // Record the requested depth (16/24/32). This is a BEST-EFFORT request: JUCE's
      // AudioIODevice::open() negotiates the render format itself and, in WASAPI/CoreAudio
      // shared mode, typically delivers float buffers — it does NOT accept an arbitrary
      // integer bit depth as an open parameter. We therefore (a) store what was asked for,
      // (b) open at the requested sample rate, and (c) let the device fall back to its
      // nearest supported format. After open, getCurrentBitDepth() reports what was actually
      // granted; callers must treat requestedOutputBitDepth() as a request, not a guarantee.
      requestedOutBits = (requestedOutputBits == 16 || requestedOutputBits == 24
                          || requestedOutputBits == 32) ? requestedOutputBits : 24;

      auto* type = findPreferredType();
      if (type == nullptr) return "No suitable audio driver type (WASAPI/CoreAudio) found";
      type->scanForDevices();
      outDev.reset (type->createDevice (id.name, {}));   // output-only render side
      if (outDev == nullptr) return "Could not create output device: " + id.name;
      juce::BigInteger noIn;
      juce::BigInteger outCh; outCh.setRange (0, 2, true);   // dual-mono L=R
      // open() negotiates the format; the device picks the nearest supported depth. There is
      // no JUCE API to force an integer render depth here, so the request is honored only to
      // the extent the driver's shared-mode format allows (verify with getCurrentBitDepth()).
      auto err = outDev->open (noIn, outCh, sampleRate, bufferSize);
      if (err.isNotEmpty()) { outDev.reset(); return "Open output failed: " + err; }
      return {};
  }

  void DeviceManager::closeAll() {
      if (inDev)  { inDev->stop();  inDev->close();  inDev.reset(); }
      if (outDev) { outDev->stop(); outDev->close(); outDev.reset(); }
  }

  } // namespace eb
  ```

- [ ] **Run — expect PASS.**
  ```
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R DeviceManager"
  ```
  Expected: the three `DeviceManager` helper cases pass (these touch only pure helpers + the whitelist fallback; no live device is opened).

- [ ] **MANUAL VERIFICATION (live enumeration — cannot be unit-tested; deferred to Task 7's diagnostic).** Note here that `rescan()`/`openInput()`/`openOutput()` are exercised by the `eb_diag` console tool built in Task 7. Do not block this task on hardware; the pure helpers above are the unit gate.

- [ ] **Commit.**
  ```
  git add src/audio/DeviceManager.h src/audio/DeviceManager.cpp tests/test_devicemanager.cpp tests/CMakeLists.txt
  git commit -m "Plan 2 Task 5: DeviceManager enumeration, model/rate detection, open/close"
  ```

---

### Task 6 — AudioEngine facade: wire the dataflow + FIR-at-active-rate, with a headless test seam

The GUI-facing facade. It owns `DeviceManager` + `eb::ProcessingGraph` + `ClockBridge` + `HealthMonitor`, exposes the full SPINE API, designs the per-ear FIR at the active rate (`numTaps ≈ 8192·rate/48000`, power-of-two), and wires the two callbacks. A **headless test seam** (`processCaptureBlockForTest`) injects a known 2-channel buffer and returns the mono output so the `ProcessingGraph` integration + combine + FIR-at-rate is verified without hardware.

**Files**
- Create: `src/audio/AudioEngine.h`
- Modify: `src/audio/AudioEngine.cpp`
- Test: `tests/test_audioengine.cpp`

**Steps**

- [ ] **Write the header.** Create `src/audio/AudioEngine.h`:
  ```cpp
  #pragma once
  #include <juce_audio_devices/juce_audio_devices.h>
  #include "audio/DeviceManager.h"
  #include "audio/ClockBridge.h"
  #include "audio/HealthMonitor.h"
  #include "audio/EngineTypes.h"
  #include "audio/ProcessingGraph.h"
  #include "audio/CombineMode.h"
  #include "cal/CalFile.h"
  #include "cal/FirDesigner.h"
  #include <atomic>
  #include <memory>
  namespace eb {

  // Compute the FIR tap count for a given active sample rate: 8192 @ 48k, scaled by
  // rate/48000, rounded UP to the next power of two (8192@48k, 16384@96k, 32768@192k).
  int firTapsForRate (double sampleRate);

  class AudioEngine {
  public:
      AudioEngine();
      ~AudioEngine();

      // ---- Device / format selection (call while Stopped) ----
      std::vector<DeviceId> inputDevices()  const;
      std::vector<DeviceId> outputDevices() const;
      std::vector<double>   supportedSampleRates (const DeviceId&) const;
      std::vector<int>      supportedBitDepths   (const DeviceId&) const;
      void setInput  (const DeviceId&);
      void setOutput (const DeviceId&);
      void setSampleRate (double sr);
      void setOutputBitDepth (int bits);   // 16/24/32 (24 default); best-effort request, not enforced

      // ---- DSP config ----
      void setLeftCalFir  (juce::AudioBuffer<float> fir);   // hot-swappable while Running
      void setRightCalFir (juce::AudioBuffer<float> fir);
      void setCombineMode (eb::CombineMode);

      // Convenience: design + load the FIR from a parsed cal file at the active rate.
      void loadLeftCal  (const CalFile&);
      void loadRightCal (const CalFile&);

      // ---- Transport ----
      bool start (juce::String& errorOut);
      void stop();
      EngineStatus status() const;

      // ---- Telemetry (lock-free snapshots) ----
      Levels levels() const;
      Health health() const;

      // ---- Headless test seam ----
      // Re-prepare the graph at the given rate and process one injected 2-ch block to mono.
      // No device I/O. Spins until the async Convolution IR load + gain ramp settles.
      void prepareForTest (double sampleRate, int blockSize);
      void processCaptureBlockForTest (const float* inL, const float* inR,
                                       float* outMono, int numSamples);

  private:
      void rescanDevices();
      static int nextPow2 (int v);

      DeviceManager      devices;
      ProcessingGraph    graph;
      ClockBridge        bridge;
      HealthMonitor      hm;

      DeviceId inputId, outputId;
      double   activeRate = 48000.0;
      int      outputBits = 24;
      int      blockSize  = 0;

      std::atomic<int> engineStatus { (int) EngineStatus::Stopped };

      // Audio callback adapters (own no state beyond pointers back to this).
      struct CaptureCallback;  struct RenderCallback;
      std::unique_ptr<CaptureCallback> captureCb;
      std::unique_ptr<RenderCallback>  renderCb;
      friend struct CaptureCallback;   friend struct RenderCallback;
  };

  } // namespace eb
  ```

- [ ] **Write the failing test.** Create `tests/test_audioengine.cpp`:
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include <catch2/matchers/catch_matchers_floating_point.hpp>
  #include "audio/AudioEngine.h"
  #include "cal/FirDesigner.h"
  #include <vector>
  #include <cmath>
  using Catch::Matchers::WithinAbs;

  TEST_CASE("firTapsForRate scales 8192@48k to power-of-two per rate") {
      CHECK (eb::firTapsForRate (48000.0)  == 8192);
      CHECK (eb::firTapsForRate (96000.0)  == 16384);
      CHECK (eb::firTapsForRate (192000.0) == 32768);
      CHECK (eb::firTapsForRate (44100.0)  == 8192);    // 7526 -> next pow2
      CHECK (eb::firTapsForRate (88200.0)  == 16384);
  }

  static juce::AudioBuffer<float> unitImpulse (int taps) {
      juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
  }

  TEST_CASE("AudioEngine headless seam: identity FIR + Average combine averages the ears") {
      eb::AudioEngine eng;
      const int N = 64;
      eng.prepareForTest (48000.0, N);
      eng.setLeftCalFir  (unitImpulse (8));
      eng.setRightCalFir (unitImpulse (8));
      eng.setCombineMode (eb::CombineMode::Average);

      std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);
      // Spin a few times via the seam to settle the async IR load + gain ramp.
      bool settled = false;
      for (int rep = 0; rep < 3000 && ! settled; ++rep) {
          eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), N);
          if (std::abs (out[N-1] - 0.4f) < 1e-3f) settled = true;
          else juce::Thread::sleep (1);
      }
      REQUIRE (settled);
      CHECK_THAT (out[N-1], WithinAbs (0.4f, 1e-3));   // (0.5+0.3)/2
  }

  TEST_CASE("AudioEngine: real R_HPN cal designed at 96k cuts the 4 kHz resonance") {
      auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_8604350.txt");
      REQUIRE (f.existsAsFile());
      auto cal = eb::CalFile::parse (f.loadFileAsString());

      eb::AudioEngine eng;
      const int N = 128;
      eng.prepareForTest (96000.0, N);
      eng.setSampleRate (96000.0);            // active rate drives tap count = 16384
      eng.loadRightCal (cal);                 // designs FIR at 96k internally
      eng.setCombineMode (eb::CombineMode::TwoPassRight);

      // Drive a 4 kHz sine and confirm it is strongly attenuated vs a 200 Hz reference.
      auto rms = [&](double freq) {
          std::vector<float> inL (N, 0.0f), inR (N, 0.0f), out (N, 0.0f);
          double ph = 0.0, d = 2.0 * juce::MathConstants<double>::pi * freq / 96000.0;
          double acc = 0.0; int blocks = 200;
          for (int b = 0; b < blocks; ++b) {
              for (int i = 0; i < N; ++i) { inR[i] = (float) std::sin (ph); ph += d; }
              eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), N);
              if (b > 40) for (int i = 0; i < N; ++i) acc += (double) out[i] * out[i];
              else juce::Thread::sleep (1);   // let IR settle during warm-up blocks
          }
          return std::sqrt (acc / (double) ((blocks - 41) * N));
      };
      double r200 = rms (200.0), r4k = rms (4000.0);
      INFO ("rms200=" << r200 << " rms4k=" << r4k);
      CHECK (r4k < 0.4 * r200);   // ~4 kHz resonance inverted -> strong cut
  }
  ```

- [ ] **Register the test.** Add `test_audioengine.cpp` to `add_executable(eb_tests ...)` in `tests/CMakeLists.txt`.

- [ ] **Run it — expect FAIL.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  ```
  Expected: link error — `eb::firTapsForRate`, `AudioEngine::prepareForTest`, etc. unresolved.

- [ ] **Implement.** Replace the contents of `src/audio/AudioEngine.cpp` with:
  ```cpp
  #include "audio/AudioEngine.h"
  #include <cmath>
  namespace eb {

  int AudioEngine::nextPow2 (int v) {
      int p = 1; while (p < v) p <<= 1; return p;
  }

  int firTapsForRate (double sampleRate) {
      const double scaled = 8192.0 * (sampleRate / 48000.0);
      int v = (int) std::ceil (scaled);
      int p = 1; while (p < v) p <<= 1; return p;
  }

  // ---- Audio callback adapters ----------------------------------------------------------
  // Capture: 2ch in -> ProcessingGraph -> mono -> bridge.pushCapture. No alloc/lock/syscall.
  struct AudioEngine::CaptureCallback : juce::AudioIODeviceCallback {
      AudioEngine& e;
      std::vector<float> mono;   // sized in prepare; NOT resized on the callback
      explicit CaptureCallback (AudioEngine& o) : e (o) {}

      void audioDeviceAboutToStart (juce::AudioIODevice* dev) override {
          mono.assign ((size_t) juce::jmax (1, dev->getCurrentBufferSizeSamples()), 0.0f);
      }
      void audioDeviceStopped() override {}

      void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                             float* const* /*out*/, int /*numOut*/,
                                             int numSamples,
                                             const juce::AudioIODeviceCallbackContext&) override {
          if (numIn < 2 || (int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
          const float* l = in[0]; const float* r = in[1];
          // Input level/clip telemetry.
          float pkL = 0, pkR = 0;
          for (int i = 0; i < numSamples; ++i) { pkL = juce::jmax (pkL, std::abs (l[i])); pkR = juce::jmax (pkR, std::abs (r[i])); }
          e.hm.reportInLevels (pkL, pkR, pkL >= 0.999f, pkR >= 0.999f);

          e.graph.process (l, r, mono.data(), numSamples);   // per-ear FIR + combine
          e.bridge.pushCapture (mono.data(), numSamples);
      }
  };

  // Render (MASTER clock): bridge.pullRender -> duplicate mono to L=R. No alloc/lock/syscall.
  struct AudioEngine::RenderCallback : juce::AudioIODeviceCallback {
      AudioEngine& e;
      std::vector<float> mono;
      explicit RenderCallback (AudioEngine& o) : e (o) {}

      void audioDeviceAboutToStart (juce::AudioIODevice* dev) override {
          mono.assign ((size_t) juce::jmax (1, dev->getCurrentBufferSizeSamples()), 0.0f);
          e.bridge.setRenderRate (dev->getCurrentSampleRate());
      }
      void audioDeviceStopped() override {}

      void audioDeviceIOCallbackWithContext (const float* const* /*in*/, int /*numIn*/,
                                             float* const* out, int numOut,
                                             int numSamples,
                                             const juce::AudioIODeviceCallbackContext&) override {
          if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
          e.bridge.pullRender (mono.data(), numSamples);
          float pk = 0;
          for (int i = 0; i < numSamples; ++i) pk = juce::jmax (pk, std::abs (mono[i]));
          e.hm.reportOutLevel (pk, pk >= 0.999f);
          for (int ch = 0; ch < numOut; ++ch)            // duplicate mono to both channels
              if (out[ch] != nullptr)
                  juce::FloatVectorOperations::copy (out[ch], mono.data(), numSamples);
          e.hm.setFifoFill (e.bridge.fifoFill());
      }
  };

  // ---- Lifecycle ------------------------------------------------------------------------
  AudioEngine::AudioEngine() {
      captureCb = std::make_unique<CaptureCallback> (*this);
      renderCb  = std::make_unique<RenderCallback>  (*this);
      devices.rescan();
  }
  AudioEngine::~AudioEngine() { stop(); }

  void AudioEngine::rescanDevices() { devices.rescan(); }

  std::vector<DeviceId> AudioEngine::inputDevices()  const { return devices.inputs(); }
  std::vector<DeviceId> AudioEngine::outputDevices() const { return devices.outputs(); }

  std::vector<double> AudioEngine::supportedSampleRates (const DeviceId& id) const {
      return devices.nativeRatesFor (id);
  }
  std::vector<int> AudioEngine::supportedBitDepths (const DeviceId& id) const {
      return devices.nativeBitDepthsFor (id);
  }

  void AudioEngine::setInput  (const DeviceId& id) { inputId = id; }
  void AudioEngine::setOutput (const DeviceId& id) { outputId = id; }
  void AudioEngine::setSampleRate (double sr) { activeRate = sr; }
  void AudioEngine::setOutputBitDepth (int bits) {   // 16/24/32; anything else -> 24 default
      outputBits = (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
  }

  void AudioEngine::setLeftCalFir  (juce::AudioBuffer<float> fir) { graph.setFir (0, std::move (fir)); }
  void AudioEngine::setRightCalFir (juce::AudioBuffer<float> fir) { graph.setFir (1, std::move (fir)); }
  void AudioEngine::setCombineMode (eb::CombineMode m) { graph.setCombineMode (m); }

  void AudioEngine::loadLeftCal (const CalFile& cal) {
      FirDesignParams p; p.sampleRate = activeRate; p.numTaps = firTapsForRate (activeRate);
      graph.setFir (0, FirDesigner::design (cal, p));
  }
  void AudioEngine::loadRightCal (const CalFile& cal) {
      FirDesignParams p; p.sampleRate = activeRate; p.numTaps = firTapsForRate (activeRate);
      graph.setFir (1, FirDesigner::design (cal, p));
  }

  EngineStatus AudioEngine::status() const { return (EngineStatus) engineStatus.load(); }
  Levels AudioEngine::levels() const { return hm.levels(); }
  Health AudioEngine::health() const {
      auto h = hm.snapshot();
      h.fifoFill = bridge.fifoFill();
      // Report the actual trimmed capture:render ratio the ClockBridge control loop is using.
      // devices.outputDevice() is const-qualified in DeviceManager, so this stays const-legal.
      double ren = activeRate;
      if (auto* od = devices.outputDevice()) ren = od->getCurrentSampleRate();
      h.captureToRenderRatio = activeRate / juce::jmax (1.0, ren);
      return h;
  }

  bool AudioEngine::start (juce::String& errorOut) {
      if (status() == EngineStatus::Running) return true;
      hm.reset();

      const int bufSize = 512;   // typical WASAPI shared block; device may adjust
      auto eIn = devices.openInput (inputId, activeRate, bufSize);
      if (eIn.isNotEmpty()) { errorOut = eIn; engineStatus.store ((int) EngineStatus::Error); return false; }
      auto eOut = devices.openOutput (outputId, activeRate, bufSize, outputBits);   // best-effort depth
      if (eOut.isNotEmpty()) { errorOut = eOut; devices.closeAll(); engineStatus.store ((int) EngineStatus::Error); return false; }

      auto* inD  = devices.inputDevice();
      auto* outD = devices.outputDevice();
      const double capRate = inD->getCurrentSampleRate();
      const double renRate = outD->getCurrentSampleRate();
      const int    maxBlk  = juce::jmax (inD->getCurrentBufferSizeSamples(),
                                         outD->getCurrentBufferSizeSamples());

      graph.prepare (capRate, maxBlk);
      // Capacity: ~250 ms of capture-rate mono, power-of-two-ish, never below 4096.
      const int cap = juce::jmax (4096, juce::nextPowerOfTwo ((int) (capRate * 0.25)));
      bridge.prepare (capRate, renRate, 1, cap);
      bridge.reset();

      // Render is the master: start it last so the FIFO has primed once capture runs.
      inD->start (captureCb.get());
      outD->start (renderCb.get());
      engineStatus.store ((int) EngineStatus::Running);
      return true;
  }

  void AudioEngine::stop() {
      if (status() != EngineStatus::Running) { devices.closeAll(); return; }
      devices.closeAll();
      bridge.reset();
      engineStatus.store ((int) EngineStatus::Stopped);
  }

  // ---- Headless test seam ---------------------------------------------------------------
  void AudioEngine::prepareForTest (double sampleRate, int block) {
      activeRate = sampleRate; blockSize = block;
      graph.prepare (sampleRate, block);
  }
  void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                                float* outMono, int numSamples) {
      graph.process (inL, inR, outMono, numSamples);
  }

  } // namespace eb
  ```

- [ ] **Run — expect PASS.**
  ```
  cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R AudioEngine"
  ```
  Expected: `firTapsForRate`, the identity-Average seam, and the 96 k R_HPN cut tests pass. The `INFO` line prints `rms200`/`rms4k` if the cut margin needs inspection; the cut must remain clearly below the 200 Hz reference.

- [ ] **MANUAL VERIFICATION — real-time-safety contract (no unit test can prove this).** Read `CaptureCallback::audioDeviceIOCallbackWithContext` and `RenderCallback::audioDeviceIOCallbackWithContext` and confirm by inspection: (a) no `new`/`malloc`/`std::vector::resize`/`push_back`; (b) no locks/mutexes; (c) no file/console/syscall; (d) every buffer (`mono`) is sized in `audioDeviceAboutToStart`, never on the callback; (e) `ClockBridge::pushCapture`/`pullRender` and `ProcessingGraph::process` are themselves alloc/lock-free (verified in Tasks 3 and Plan 1). PASS = all five hold. Recommend a dedicated rt-safety review (and optionally a debug `juce::ScopedNoDenormals` + an audio-thread allocation guard) **before** the first live run.

- [ ] **Commit.**
  ```
  git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp tests/CMakeLists.txt
  git commit -m "Plan 2 Task 6: AudioEngine facade, dataflow wiring, FIR-at-active-rate, headless seam"
  ```

---

### Task 7 — `eb_diag` console diagnostic: enumerate + tag + 5 s passthrough (manual verification)

A small console target that exercises the live device path the unit tests cannot: it lists every input/output endpoint with detected model + native rates/bit-depths + virtual-sink tag, then opens the first EARS/EARS-Pro input and a chosen virtual sink and runs a ~5 s passthrough so you can confirm audio reaches the virtual cable.

**Files**
- Create: `src/tools/eb_diag_main.cpp`
- Modify: `CMakeLists.txt`

**Steps**

- [ ] **Write the diagnostic.** Create `src/tools/eb_diag_main.cpp`:
  ```cpp
  // Console diagnostic for Plan 2: lists devices with detected model + native rates/bit-depths,
  // then opens an EARS/EARS-Pro input + a virtual sink and runs a ~5 s passthrough.
  // Usage:  eb_diag                 -> list only
  //         eb_diag run "<outName>" -> list + 5 s passthrough into the named output
  #include <juce_audio_devices/juce_audio_devices.h>
  #include "audio/AudioEngine.h"
  #include "audio/ModelDetect.h"
  #include <iostream>

  static const char* modelName (eb::EarsModel m) {
      switch (m) { case eb::EarsModel::Ears: return "EARS";
                   case eb::EarsModel::EarsPro: return "EARS Pro";
                   default: return "—"; }
  }

  int main (int argc, char** argv) {
      juce::ScopedJuceInitialiser_GUI juceInit;   // needed for device subsystem on some OSes
      eb::AudioEngine eng;

      std::cout << "== INPUT DEVICES ==\n";
      for (auto& d : eng.inputDevices()) {
          std::cout << "  [" << modelName (d.model) << "] " << d.name << "\n";
          std::cout << "      rates:";
          for (double r : eng.supportedSampleRates (d)) std::cout << " " << (int) r;
          std::cout << "   bits:";
          for (int b : eng.supportedBitDepths (d)) std::cout << " " << b;
          std::cout << "\n";
      }
      std::cout << "== OUTPUT DEVICES ==\n";
      for (auto& d : eng.outputDevices())
          std::cout << "  " << (d.isVirtualSink ? "[virtual] " : "          ") << d.name << "\n";

      if (argc >= 3 && juce::String (argv[1]) == "run") {
          // Pick the first recognised EARS/EARS-Pro input.
          eb::DeviceId chosenIn;
          for (auto& d : eng.inputDevices())
              if (d.model != eb::EarsModel::Unknown) { chosenIn = d; break; }
          if (chosenIn.name.isEmpty()) { std::cout << "No EARS input found.\n"; return 2; }

          eb::DeviceId chosenOut;
          for (auto& d : eng.outputDevices())
              if (d.name == juce::String (argv[2])) { chosenOut = d; break; }
          if (chosenOut.name.isEmpty()) { std::cout << "Output not found: " << argv[2] << "\n"; return 3; }

          auto rates = eng.supportedSampleRates (chosenIn);
          const double rate = rates.empty() ? 48000.0 : rates.front();
          eng.setInput (chosenIn); eng.setOutput (chosenOut);
          eng.setSampleRate (rate); eng.setOutputBitDepth (24);

          juce::String err;
          if (! eng.start (err)) { std::cout << "start failed: " << err << "\n"; return 4; }
          std::cout << "Passthrough running at " << (int) rate << " Hz for 5 s...\n";
          juce::Thread::sleep (5000);
          auto h = eng.health(); auto lv = eng.levels();
          eng.stop();
          std::cout << "xruns=" << h.xruns << " dropped=" << h.droppedFrames
                    << " fifoFill=" << h.fifoFill << " clean=" << (h.cleanCapture ? "yes" : "no") << "\n";
          std::cout << "inL=" << lv.inL << " inR=" << lv.inR << " outMono=" << lv.outMono << "\n";
      }
      return 0;
  }
  ```

- [ ] **Add the target.** In `CMakeLists.txt`, after the `EarsBridge` block (before `enable_testing()`), insert:
  ```cmake
  # ---- Console diagnostic (live device enumeration + passthrough; manual use) ----
  juce_add_console_app(eb_diag PRODUCT_NAME "eb_diag")
  target_sources(eb_diag PRIVATE src/tools/eb_diag_main.cpp)
  target_compile_definitions(eb_diag PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
  target_link_libraries(eb_diag PRIVATE
      eb_core eb_engine juce::juce_audio_devices
      juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
  ```

- [ ] **Build the diagnostic — expect success (compiles + links).**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build --target eb_diag"
  ```
  Expected: `eb_diag.exe` produced under `build\` (path printed by Ninja). No tests here — this target is for manual hardware verification.

- [ ] **MANUAL VERIFICATION A — enumeration (no EARS required).** Run the list mode:
  ```
  build\eb_diag.exe
  ```
  PASS criteria:
  - The `== INPUT DEVICES ==` and `== OUTPUT DEVICES ==` headers print.
  - Any installed virtual cable (e.g. `CABLE Input (VB-Audio Virtual Cable)`) appears under outputs prefixed with `[virtual]`.
  - If an EARS is plugged in, its input line shows `[EARS]` or `[EARS Pro]` and a `rates:`/`bits:` line: EARS → `rates: 48000   bits: 24`; EARS Pro → `rates: 44100 48000 88200 96000 176400 192000   bits: 16 24 32`.
  - Real hardware (e.g. Realtek) shows `[—]` (Unknown) and no virtual tag.

- [ ] **MANUAL VERIFICATION B — 5 s passthrough (EARS + VB-CABLE required).** With an EARS plugged in and VB-CABLE installed, run:
  ```
  build\eb_diag.exe run "CABLE Input (VB-Audio Virtual Cable)"
  ```
  (Substitute your exact VB-CABLE render name from Verification A.) PASS criteria:
  - Prints `Passthrough running at 48000 Hz for 5 s...` (or the EARS-Pro rate you select).
  - During the 5 s, tap/speak into the EARS coupler; afterwards the summary shows `inL`/`inR` > 0 and `outMono` > 0.
  - `xruns=0`, `dropped=0`, `clean=yes` for a clean run on idle hardware (occasional `xruns` under heavy CPU load is acceptable and correctly flips `clean=no`).
  - Confirm audio reached the cable: open Windows Sound → Recording → `CABLE Output` and watch its level meter move while the passthrough runs, OR open the cable's capture endpoint in any recorder and confirm signal. This is the §13 open-item end-to-end smoke test (do it once before relying on the bridge).

- [ ] **Commit.**
  ```
  git add CMakeLists.txt src/tools/eb_diag_main.cpp
  git commit -m "Plan 2 Task 7: eb_diag console diagnostic (enumerate + 5 s passthrough)"
  ```

---

### Task 8 — Full regression + integration sanity

Confirm the whole engine builds and every unit test still passes together, and that the app + diag still link.

**Files**
- Modify: none (verification only).

**Steps**

- [ ] **Clean-ish reconfigure + build everything.**
  ```
  cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
  cmd /c "tools\dev.cmd cmake --build build"
  ```
  Expected: `eb_core`, `eb_engine`, `eb_tests`, `EarsBridge`, and `eb_diag` all build with no errors.

- [ ] **Run the full test suite — expect PASS.**
  ```
  cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure"
  ```
  Expected: every test case green — Plan 1 (`calfile`, `firdesigner`, `processinggraph`, `smoke`) plus Plan 2 (`DeviceId`, `modeldetect`, `ClockBridge`, `HealthMonitor`, `DeviceManager`, `AudioEngine`). ctest prints `100% tests passed`.

- [ ] **Commit (if any incidental fixes were needed; otherwise skip).**
  ```
  git add -A
  git commit -m "Plan 2 Task 8: full-suite regression green for the headless audio engine"
  ```

---

## Done criteria

- `eb_engine` static library exists and is linked by `eb_tests`, `EarsBridge`, and `eb_diag`.
- These exact SPINE types/files are implemented in `eb`: `EarsModel`, `EngineStatus`, `Levels`, `Health` (`src/audio/EngineTypes.h`); `DeviceId` with `key()`/`operator==` (`src/audio/DeviceId.{h,cpp}`); `ClockBridge` (`src/audio/ClockBridge.{h,cpp}`); `HealthMonitor` (`src/audio/HealthMonitor.{h,cpp}`); `DeviceManager` (`src/audio/DeviceManager.{h,cpp}`); `AudioEngine` (`src/audio/AudioEngine.{h,cpp}`) with the full GUI-facing API.
- `AudioEngine` exposes `inputDevices/outputDevices/supportedSampleRates/supportedBitDepths/setInput/setOutput/setSampleRate/setOutputBitDepth/setLeftCalFir/setRightCalFir/setCombineMode/start/stop/status/levels/health` verbatim per the SPINE, and wires capture→`eb::ProcessingGraph`→`ClockBridge`→render with the render callback as master.
- Dual-device reality is honored: EARS → `{48000}` / `{24}`; EARS Pro → `{44100,48000,88200,96000,176400,192000}` / `{16,24,32}`, detected from device name / USB VID:PID; input opens at its native rate on WASAPI/CoreAudio (never ASIO); `setOutputBitDepth(16|24|32)` (24 default) is threaded into `DeviceManager::openOutput(...)` and requested best-effort on the render side (shared-mode float may not honor an arbitrary integer depth — recorded + nearest-supported fallback, never claimed as enforced).
- FIR is designed at the active rate via `eb::FirDesigner` with `numTaps` = `firTapsForRate(rate)` (8192@48k, 16384@96k, 32768@192k), rebuilt on rate change; FIR hot-swap goes through `eb::ProcessingGraph::setFir`.
- All unit tests pass (`ctest` 100%): `ClockBridge` (different rates + drift, bounded fill, no under/overrun, sine passes), `DeviceId`/`ModelDetect`, per-model rate/bit-depth maps, `HealthMonitor` counters + clean-capture latch, `AudioEngine` headless seam (combine + FIR-at-rate).
- Manual verification blocks (live enumeration; 5 s EARS→VB-CABLE passthrough; rt-safety inspection) are documented with explicit PASS criteria and were run once before any reliance on the live bridge.
- No allocation/lock/syscall on either audio callback (verified by inspection per the rt-safety block); an rt-safety review is recommended before first live use.
