# Plan 3 — GUI

For agentic workers: **REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development`** to execute this plan. Dispatch each Task to a fresh implementation subagent, review its diff against the Task's PASS criteria, then commit. Steps use checkbox (`- [ ]`) syntax — check each off as it lands. Do not batch Tasks; one Task = one review checkpoint.

**Pre-execution:** run an API-verification pass (as Plan 1 did) over this plan before implementing — it grounds every JUCE/CMake API call against current docs (JUCE 8.0.4). The load-bearing JUCE APIs used here (`ApplicationProperties`/`PropertiesFile`, `LookAndFeel_V4`, `Component`/`Slider`/`ComboBox`/`Timer`, `FileDragAndDropTarget`, `Graphics` path/text) were grounded via context7 `/websites/juce_master` and docs.juce.com while authoring; re-verify before coding.

**Goal:** Build the modern dark GUI layer of EARS Bridge — Settings persistence, dark theme, cal-curve thumbnails, drag-drop cal slots, device/rate/bit-depth pickers, meters, and the full `MainComponent` layout — wired to the Plan 2 `eb::AudioEngine` facade.

**Architecture:** A single resizable dark window (`eb::MainComponent`) owns one `eb::AudioEngine` (Plan 2) and one `eb::Settings`, and polls the engine's lock-free `levels()`/`health()` snapshots from a `juce::Timer`. Pure, headless-testable helpers (coordinate transforms, rate-menu derivation, combine-mode ordering, numTaps-for-rate) live in small free functions/classes so the GUI shells stay thin. Device/rate/bit-depth changes reconfigure the engine while Stopped and rebuild both FIRs on a worker thread via `eb::FirDesigner::design` at the active rate.

**Tech Stack:** C++20, JUCE 8.0.4 (`juce_gui_basics`, `juce_gui_extra`, `juce_audio_basics`, `juce_dsp`), CMake + Ninja + MSVC (Windows) / Ninja + clang (macOS), Catch2 v3.6.0. Namespace `eb` throughout.

---

## File structure

Created in this plan:

| File | Responsibility |
| --- | --- |
| `src/state/Settings.h` / `.cpp` | Persistence via `juce::ApplicationProperties`/`PropertiesFile`: device keys, model, cal paths, combine mode, sample rate, output bit depth, output trim, advanced toggles. Round-trip save/load. |
| `src/gui/Theme.h` / `.cpp` | `eb::Theme : juce::LookAndFeel_V4` dark scheme + named brand colours; pure colour helpers. |
| `src/gui/PlotMath.h` | Pure free functions: `freqToX`, `dbToY` (log-x 20 Hz–20 kHz, linear-dB y) and inverse — the thumbnail coordinate transform, headless-testable. |
| `src/gui/RateMenu.h` | Pure helpers: build the rate-menu model from `supportedSampleRates`, flag non-native (resample) rates; combine-mode ordering-by-rigor + recommended flag; `numTapsForRate`. |
| `src/gui/LevelMeter.h` / `.cpp` | Vertical dB meter component with peak hold + clip LED; fed a float from the GUI timer. |
| `src/gui/CurveThumbnail.h` / `.cpp` | Static FR plot of a parsed `eb::CalFile` using `PlotMath`. |
| `src/gui/CalSlotComponent.h` / `.cpp` | One cal slot: `FileDragAndDropTarget`, parses dropped file via `eb::CalFile`, shows serial + type, HEQ-as-mic-cal warning badge, renders the thumbnail; fires an onCalLoaded callback. |
| `src/gui/DevicePicker.h` / `.cpp` | `ComboBox` bound to a `std::vector<eb::DeviceId>`; shows detected EARS/EARS Pro model; fires onDeviceChosen. |
| `src/gui/MainComponent.h` / `.cpp` | Top-level layout (design-spec §8); owns `AudioEngine` + `Settings`; `juce::Timer` polling; wires pickers/rate/bit-depth/combine/transport. |

Modified:

| File | Change |
| --- | --- |
| `src/Main.cpp` | Host `eb::MainComponent` in the `DocumentWindow` (replace the empty `juce::Component`). |
| `CMakeLists.txt` | New `eb_gui` STATIC lib (gui + state sources); link `juce_gui_basics`/`juce_gui_extra`; `EarsBridge` app links `eb_gui`; `eb_tests` gains the new test files + `eb_gui`. |
| `tests/CMakeLists.txt` | Add `test_settings.cpp`, `test_plotmath.cpp`, `test_ratemenu.cpp`; link `eb_gui`. |

## Shared contract (SPINE recap)

This plan **consumes** the Plan 2 `eb::AudioEngine` facade and Plan 1 DSP core verbatim. It **defines no new shared types** — every cross-plan type comes from the SPINE. Render the SPINE interfaces this plan touches:

```cpp
namespace eb {

enum class EarsModel { Unknown, Ears, EarsPro };

struct DeviceId {
    juce::String typeName, name, uid;
    bool isVirtualSink = false;
    EarsModel model = EarsModel::Unknown;
    bool operator== (const DeviceId&) const;
    juce::String key() const;   // typeName + "|" + name + "|" + uid, stable across replug
};

enum class EngineStatus { Stopped, Running, Error };

struct Levels { float inL = 0, inR = 0, outMono = 0; bool clipL = false, clipR = false, clipOut = false; };
struct Health { int xruns = 0; long long droppedFrames = 0; double fifoFill = 0.0; bool cleanCapture = true; double captureToRenderRatio = 1.0; };

} // namespace eb
```

```cpp
// AudioEngine — the ONLY engine surface the GUI talks to (Plan 2).
namespace eb {
class AudioEngine {
public:
    std::vector<DeviceId> inputDevices() const;
    std::vector<DeviceId> outputDevices() const;                  // virtual sinks flagged isVirtualSink
    std::vector<double>   supportedSampleRates (const DeviceId&) const; // EARS -> {48000}; EARS Pro -> {44100,48000,88200,96000,176400,192000}
    std::vector<int>      supportedBitDepths   (const DeviceId&) const; // EARS -> {24}; EARS Pro -> {16,24,32}
    void setInput  (const DeviceId&);                             // configure while Stopped
    void setOutput (const DeviceId&);
    void setSampleRate    (double sr);                            // must be one of the input native rates
    void setOutputBitDepth (int bits);                           // 16/24/32 (24 default); requested best-effort on the virtual-sink render format
    void setLeftCalFir  (juce::AudioBuffer<float> fir);          // from eb::FirDesigner::design at the active rate; hot-swappable while Running
    void setRightCalFir (juce::AudioBuffer<float> fir);
    void setCombineMode (eb::CombineMode);
    bool start (juce::String& errorOut);
    void stop();
    EngineStatus status() const;
    Levels levels() const;   // lock-free snapshot for the GUI timer
    Health health() const;   // lock-free snapshot
};
} // namespace eb
```

Plan 1 (reuse as-is, rate-agnostic): `eb::CalFile` (`parse`, `serial`, `type`, `points` of `{freqHz,splDb,phaseDeg}`, `CalType{Unknown,Raw,Hpn,Heq,Idf}`, throws `eb::CalParseError`); `eb::FirDesigner::design(const CalFile&, const FirDesignParams&)` with `FirDesignParams{sampleRate,numTaps,designFftOrder,mode,invert,maxBoostDb,bandLowHz,bandHighHz}` and `FirMode{MinPhaseMagnitude,ComplexWithPhase}`; `eb::CombineMode{TwoPassLeft,TwoPassRight,Average,Sum}`.

> **Plan-2 dependency note:** Tasks 1–6 (Settings, Theme, PlotMath, RateMenu, LevelMeter, CurveThumbnail, CalSlotComponent) depend only on Plan 1 + JUCE and can be built before Plan 2 lands. Tasks 7–9 (DevicePicker, MainComponent, Main.cpp wiring) link against `eb::AudioEngine`. If Plan 2 is not yet merged when you reach Task 7, build against the SPINE `AudioEngine` header signature above (the header is owned by Plan 2 at `src/audio/AudioEngine.h`); the GUI only ever calls the public methods listed.

---

## Task 1 — Settings (PropertiesFile round-trip)

**Files**

- Create: `src/state/Settings.h`, `src/state/Settings.cpp`
- Create test: `tests/test_settings.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

### Steps

- [ ] **Add the `eb_gui` static lib + GUI module wiring to `CMakeLists.txt`.** Insert this block immediately after the `eb_core` block (after its `target_compile_definitions(... )`) and BEFORE the `juce_add_gui_app` block:

```cmake
# ---- GUI + state library (components, theme, persistence) ----
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp
    src/gui/DevicePicker.cpp
    src/gui/MainComponent.cpp)
target_include_directories(eb_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eb_gui PUBLIC cxx_std_20)
# GUI headers expose juce_gui_* and juce_audio types publicly -> PUBLIC so include dirs/
# defines reach eb_tests and the app target. AudioEngine + DeviceId::key()/operator== live in
# eb_engine (which PUBLIC-links eb_core + juce_audio_devices), so link eb_engine (NOT eb_core).
target_link_libraries(eb_gui
    PUBLIC
        eb_engine
        juce::juce_gui_extra juce::juce_gui_basics
        juce::juce_audio_basics juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
target_compile_definitions(eb_gui PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
```

> NOTE: `MainComponent.cpp` and `DevicePicker.cpp` reference `src/audio/AudioEngine.h` (Plan 2). Until Plan 2 is merged, this lib still configures/compiles Tasks 1–6 because those `.cpp` files are added incrementally — add `Theme.cpp`/`LevelMeter.cpp`/`CurveThumbnail.cpp`/`CalSlotComponent.cpp`/`DevicePicker.cpp`/`MainComponent.cpp` to `add_library` only as each Task creates the file. **For Task 1, the `add_library(eb_gui STATIC ...)` list is just `src/state/Settings.cpp`.** Each later Task's first step appends its `.cpp`.

  So for **Task 1 only**, use this minimal form:

```cmake
# ---- GUI + state library (components, theme, persistence) ----
add_library(eb_gui STATIC
    src/state/Settings.cpp)
target_include_directories(eb_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eb_gui PUBLIC cxx_std_20)
target_link_libraries(eb_gui
    PUBLIC
        eb_engine
        juce::juce_gui_extra juce::juce_gui_basics
        juce::juce_audio_basics juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
target_compile_definitions(eb_gui PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
```

- [ ] **Wire `eb_gui` into the test target — ADDITIVELY.** In `tests/CMakeLists.txt`, **APPEND** `test_settings.cpp` to the EXISTING `add_executable(eb_tests ...)` list (which already has the Plan 1 + Plan 2 test sources — do NOT delete them) and ADD `eb_gui` to the existing `target_link_libraries` (keep `eb_engine` + `juce_audio_devices`; also keep the separate `diag_clockbridge` target further down). Result:

```cmake
add_executable(eb_tests
    test_smoke.cpp
    test_calfile.cpp
    test_firdesigner.cpp
    test_processinggraph.cpp
    test_deviceid.cpp
    test_modeldetect.cpp
    test_clockbridge.cpp
    test_healthmonitor.cpp
    test_devicemanager.cpp
    test_audioengine.cpp
    test_settings.cpp)
target_link_libraries(eb_tests PRIVATE
    eb_core eb_engine eb_gui
    juce::juce_audio_basics juce::juce_dsp juce::juce_audio_devices
    juce::juce_recommended_config_flags
    Catch2::Catch2WithMain)
target_compile_definitions(eb_tests PRIVATE
    EB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

include(Catch)
catch_discover_tests(eb_tests)
```
(Later GUI-logic tasks append `test_plotmath.cpp` / `test_ratemenu.cpp` the same additive way. The `diag_clockbridge` executable block below the test target stays unchanged.)

- [ ] **Write the failing test** `tests/test_settings.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "state/Settings.h"
#include <juce_core/juce_core.h>

using Catch::Matchers::WithinAbs;

// Each test gets a private temp dir so the PropertiesFile can't collide with a
// real user-settings file or another test run.
static juce::File makeTempDir() {
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("eb_settings_test_" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}

TEST_CASE("Settings round-trips every field through a PropertiesFile") {
    auto dir = makeTempDir();
    {
        eb::Settings s (dir);
        s.setInputKey  ("WASAPI|EARS Pro|VID_2752&PID_0034");
        s.setOutputKey ("WASAPI|CABLE Input (VB-Audio Virtual Cable)|{uid}");
        s.setInputModel (eb::EarsModel::EarsPro);
        s.setSampleRate (96000.0);
        s.setOutputBitDepth (24);
        s.setCombineMode (eb::CombineMode::TwoPassRight);
        s.setLeftCalPath  ("C:/cal/L_HPN_8604350.txt");
        s.setRightCalPath ("C:/cal/R_HPN_8604350.txt");
        s.setOutputTrimDb (-3.5);
        s.setFirLength (16384);
        s.setComplexPhase (true);
        s.flush();   // force write-through
    }

    eb::Settings reloaded (dir);   // re-open same folder -> reads back the file
    CHECK (reloaded.inputKey()  == juce::String ("WASAPI|EARS Pro|VID_2752&PID_0034"));
    CHECK (reloaded.outputKey() == juce::String ("WASAPI|CABLE Input (VB-Audio Virtual Cable)|{uid}"));
    CHECK (reloaded.inputModel() == eb::EarsModel::EarsPro);
    CHECK_THAT (reloaded.sampleRate(), WithinAbs (96000.0, 1e-9));
    CHECK (reloaded.outputBitDepth() == 24);
    CHECK (reloaded.combineMode() == eb::CombineMode::TwoPassRight);
    CHECK (reloaded.leftCalPath()  == juce::String ("C:/cal/L_HPN_8604350.txt"));
    CHECK (reloaded.rightCalPath() == juce::String ("C:/cal/R_HPN_8604350.txt"));
    CHECK_THAT (reloaded.outputTrimDb(), WithinAbs (-3.5, 1e-9));
    CHECK (reloaded.firLength() == 16384);
    CHECK (reloaded.complexPhase() == true);

    dir.deleteRecursively();
}

TEST_CASE("Settings returns sane defaults on a fresh store") {
    auto dir = makeTempDir();
    eb::Settings s (dir);
    CHECK (s.inputKey().isEmpty());
    CHECK (s.outputKey().isEmpty());
    CHECK (s.inputModel() == eb::EarsModel::Unknown);
    CHECK_THAT (s.sampleRate(), WithinAbs (48000.0, 1e-9));  // default native EARS rate
    CHECK (s.outputBitDepth() == 24);
    CHECK (s.combineMode() == eb::CombineMode::TwoPassLeft);
    CHECK_THAT (s.outputTrimDb(), WithinAbs (0.0, 1e-9));
    CHECK (s.firLength() == 0);   // 0 = Auto: taps scale with rate via numTapsForRate()
    CHECK (s.complexPhase() == false);
    dir.deleteRecursively();
}
```

- [ ] **Run it, expect FAIL (does not compile — `Settings.h` missing).**

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
```
Expected: configure succeeds; build FAILS with `state/Settings.h: No such file or directory`.

- [ ] **Implement `src/state/Settings.h`:**

```cpp
#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>   // juce::PropertiesFile / Options (NOT in juce_core)
#include "audio/EngineTypes.h"   // eb::EarsModel (Plan 2 SPINE)
#include "audio/CombineMode.h"   // eb::CombineMode (Plan 1)

namespace eb {

// Thin typed wrapper over a juce::PropertiesFile. Stores GUI/engine selections so
// the app restores its last configuration on launch. Constructed with an explicit
// settings folder (defaulting to the per-user app-data dir) so tests can use a temp
// dir. All getters fall back to sane defaults when a key is absent.
class Settings {
public:
    // dir == empty -> per-user application-data folder. Otherwise the given folder
    // (used by tests).
    explicit Settings (const juce::File& dir = {});

    juce::String inputKey()  const;   void setInputKey  (const juce::String&);
    juce::String outputKey() const;   void setOutputKey (const juce::String&);
    EarsModel    inputModel() const;  void setInputModel (EarsModel);
    double       sampleRate() const;  void setSampleRate (double);
    int          outputBitDepth() const; void setOutputBitDepth (int);
    CombineMode  combineMode() const; void setCombineMode (CombineMode);
    juce::String leftCalPath()  const; void setLeftCalPath  (const juce::String&);
    juce::String rightCalPath() const; void setRightCalPath (const juce::String&);
    double       outputTrimDb() const; void setOutputTrimDb (double);
    int          firLength() const;   void setFirLength (int);   // 0 = Auto (numTapsForRate); else explicit override
    bool         complexPhase() const; void setComplexPhase (bool);

    void flush();   // force the PropertiesFile to disk immediately

private:
    juce::PropertiesFile& props();
    std::unique_ptr<juce::PropertiesFile> file;
};

} // namespace eb
```

> Plan 2 is already merged: `src/audio/EngineTypes.h` (defining `enum class EarsModel`) and `src/audio/CombineMode.h` are the real, fully-populated headers. Include them as-is — do NOT create shim headers (a duplicate definition would cause an ODR/redefinition clash), and do NOT `git add src/audio/EngineTypes.h` in the Task 1 commit (it already exists).

- [ ] **Implement `src/state/Settings.cpp`:**

```cpp
#include "state/Settings.h"

namespace eb {

namespace {
    constexpr const char* kInputKey   = "inputKey";
    constexpr const char* kOutputKey  = "outputKey";
    constexpr const char* kModel      = "inputModel";
    constexpr const char* kRate       = "sampleRate";
    constexpr const char* kBits       = "outputBitDepth";
    constexpr const char* kCombine    = "combineMode";
    constexpr const char* kLeftCal    = "leftCalPath";
    constexpr const char* kRightCal   = "rightCalPath";
    constexpr const char* kTrim       = "outputTrimDb";
    constexpr const char* kFirLen     = "firLength";
    constexpr const char* kComplex    = "complexPhase";
}

Settings::Settings (const juce::File& dir) {
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "EarsBridge";
    opts.filenameSuffix      = ".settings";
    opts.folderName          = "EarsBridge";
    opts.osxLibrarySubFolder = "Application Support";
    opts.storageFormat       = juce::PropertiesFile::storeAsXML;
    opts.millisecondsBeforeSaving = 0;   // write on every change

    auto target = dir == juce::File()
        ? opts.getDefaultFile()
        : dir.getChildFile ("EarsBridge.settings");
    file = std::make_unique<juce::PropertiesFile> (target, opts);
}

juce::PropertiesFile& Settings::props() { return *file; }

juce::String Settings::inputKey()  const { return file->getValue (kInputKey); }
void Settings::setInputKey (const juce::String& v) { file->setValue (kInputKey, v); }

juce::String Settings::outputKey() const { return file->getValue (kOutputKey); }
void Settings::setOutputKey (const juce::String& v) { file->setValue (kOutputKey, v); }

EarsModel Settings::inputModel() const {
    return static_cast<EarsModel> (file->getIntValue (kModel, (int) EarsModel::Unknown));
}
void Settings::setInputModel (EarsModel m) { file->setValue (kModel, (int) m); }

double Settings::sampleRate() const { return file->getDoubleValue (kRate, 48000.0); }
void Settings::setSampleRate (double sr) { file->setValue (kRate, sr); }

int Settings::outputBitDepth() const { return file->getIntValue (kBits, 24); }
void Settings::setOutputBitDepth (int b) { file->setValue (kBits, b); }

CombineMode Settings::combineMode() const {
    return static_cast<CombineMode> (file->getIntValue (kCombine, (int) CombineMode::TwoPassLeft));
}
void Settings::setCombineMode (CombineMode m) { file->setValue (kCombine, (int) m); }

juce::String Settings::leftCalPath()  const { return file->getValue (kLeftCal); }
void Settings::setLeftCalPath (const juce::String& v) { file->setValue (kLeftCal, v); }

juce::String Settings::rightCalPath() const { return file->getValue (kRightCal); }
void Settings::setRightCalPath (const juce::String& v) { file->setValue (kRightCal, v); }

double Settings::outputTrimDb() const { return file->getDoubleValue (kTrim, 0.0); }
void Settings::setOutputTrimDb (double db) { file->setValue (kTrim, db); }

// Default 0 = "Auto": use numTapsForRate(activeRate) so taps scale with the sample rate.
// A non-zero value is an explicit user override (4096/8192/16384/32768).
int Settings::firLength() const { return file->getIntValue (kFirLen, 0); }
void Settings::setFirLength (int n) { file->setValue (kFirLen, n); }

bool Settings::complexPhase() const { return file->getBoolValue (kComplex, false); }
void Settings::setComplexPhase (bool b) { file->setValue (kComplex, b); }

void Settings::flush() { file->saveIfNeeded(); }

} // namespace eb
```

- [ ] **Run, expect PASS.**

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R Settings"
```
Expected: `Settings round-trips every field...` and `Settings returns sane defaults...` both pass (2 test cases).

- [ ] **Commit.**

```
git add src/state/Settings.h src/state/Settings.cpp tests/test_settings.cpp tests/CMakeLists.txt CMakeLists.txt src/audio/EngineTypes.h
git commit -m "Plan 3 Task 1: Settings persistence via PropertiesFile"
```

---

## Task 2 — PlotMath (pure coordinate transform)

**Files**

- Create: `src/gui/PlotMath.h` (header-only, pure functions)
- Create test: `tests/test_plotmath.cpp`
- Modify: `tests/CMakeLists.txt`

### Steps

- [ ] **Add the test to `tests/CMakeLists.txt`** — add `test_plotmath.cpp` to `add_executable(eb_tests ...)` (after `test_settings.cpp`). No new link libs (PlotMath is header-only, no JUCE types in its signatures beyond `float`).

- [ ] **Write the failing test** `tests/test_plotmath.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "gui/PlotMath.h"
#include <cmath>

using Catch::Matchers::WithinAbs;

// Plot rect: x in [0,W], freq log-mapped 20 Hz -> 20 kHz; y in [0,H], dB top->bottom.
TEST_CASE("freqToX maps the band endpoints to the plot edges") {
    const float W = 300.0f;
    CHECK_THAT (eb::freqToX (20.0f,    W), WithinAbs (0.0f,   1e-3));
    CHECK_THAT (eb::freqToX (20000.0f, W), WithinAbs (300.0f, 1e-3));
}

TEST_CASE("freqToX is logarithmic: a decade is a fixed fraction") {
    const float W = 300.0f;
    // 20 -> 20000 spans 3 decades. 200 Hz is exactly 1 decade up -> 1/3 of width.
    CHECK_THAT (eb::freqToX (200.0f, W),  WithinAbs (100.0f, 1e-2));
    // 2000 Hz is 2 decades up -> 2/3 of width.
    CHECK_THAT (eb::freqToX (2000.0f, W), WithinAbs (200.0f, 1e-2));
}

TEST_CASE("freqToX clamps out-of-band input to the edges") {
    const float W = 300.0f;
    CHECK_THAT (eb::freqToX (5.0f,     W), WithinAbs (0.0f,   1e-3)); // below 20 Hz
    CHECK_THAT (eb::freqToX (40000.0f, W), WithinAbs (300.0f, 1e-3)); // above 20 kHz
}

TEST_CASE("dbToY maps top dB to y=0 and bottom dB to y=H, inverted") {
    const float H = 200.0f, topDb = 24.0f, botDb = -24.0f;
    CHECK_THAT (eb::dbToY ( 24.0f, H, topDb, botDb), WithinAbs (0.0f,   1e-3)); // max dB at top
    CHECK_THAT (eb::dbToY (-24.0f, H, topDb, botDb), WithinAbs (200.0f, 1e-3)); // min dB at bottom
    CHECK_THAT (eb::dbToY (  0.0f, H, topDb, botDb), WithinAbs (100.0f, 1e-3)); // 0 dB centre
}

TEST_CASE("dbToY clamps out-of-range dB to the plot edges") {
    const float H = 200.0f, topDb = 24.0f, botDb = -24.0f;
    CHECK_THAT (eb::dbToY ( 99.0f, H, topDb, botDb), WithinAbs (0.0f,   1e-3));
    CHECK_THAT (eb::dbToY (-99.0f, H, topDb, botDb), WithinAbs (200.0f, 1e-3));
}
```

- [ ] **Run it, expect FAIL (does not compile — `PlotMath.h` missing).**

```
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
```
Expected: build FAILS with `gui/PlotMath.h: No such file or directory`.

- [ ] **Implement `src/gui/PlotMath.h`:**

```cpp
#pragma once
#include <algorithm>
#include <cmath>

namespace eb {

// Fixed display band for cal thumbnails (design-spec §8.2): 20 Hz .. 20 kHz, log x.
inline constexpr float kPlotFreqLo = 20.0f;
inline constexpr float kPlotFreqHi = 20000.0f;

// Map a frequency to an x pixel in [0, widthPx] on a log scale, clamped to the band.
inline float freqToX (float freqHz, float widthPx) {
    const float f  = std::clamp (freqHz, kPlotFreqLo, kPlotFreqHi);
    const float lo = std::log10 (kPlotFreqLo);
    const float hi = std::log10 (kPlotFreqHi);
    const float t  = (std::log10 (f) - lo) / (hi - lo);   // 0..1
    return t * widthPx;
}

// Inverse of freqToX: x pixel -> frequency (Hz). Useful for hover read-outs.
inline float xToFreq (float xPx, float widthPx) {
    const float lo = std::log10 (kPlotFreqLo);
    const float hi = std::log10 (kPlotFreqHi);
    const float t  = (widthPx <= 0.0f) ? 0.0f : std::clamp (xPx / widthPx, 0.0f, 1.0f);
    return std::pow (10.0f, lo + t * (hi - lo));
}

// Map a dB value to a y pixel in [0, heightPx]; topDb sits at y=0, botDb at y=heightPx
// (screen y grows downward), clamped to the range.
inline float dbToY (float db, float heightPx, float topDb, float botDb) {
    const float d = std::clamp (db, botDb, topDb);
    const float t = (topDb - d) / (topDb - botDb);   // 0 at top, 1 at bottom
    return t * heightPx;
}

} // namespace eb
```

- [ ] **Run, expect PASS.**

```
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R freqToX|dbToY"
```
Expected: all 5 PlotMath test cases pass.

- [ ] **Commit.**

```
git add src/gui/PlotMath.h tests/test_plotmath.cpp tests/CMakeLists.txt
git commit -m "Plan 3 Task 2: PlotMath pure freq->x / dB->y transforms"
```

---

## Task 3 — RateMenu / combine-order / numTaps helpers (pure)

**Files**

- Create: `src/gui/RateMenu.h` (header-only, pure functions)
- Create test: `tests/test_ratemenu.cpp`
- Modify: `tests/CMakeLists.txt`

### Steps

- [ ] **Add the test to `tests/CMakeLists.txt`** — add `test_ratemenu.cpp` to `add_executable(eb_tests ...)`. `RateMenu.h` includes `audio/CombineMode.h` (Plan 1, already in `eb_core`, already linked).

- [ ] **Write the failing test** `tests/test_ratemenu.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "gui/RateMenu.h"
#include "audio/CombineMode.h"
#include <vector>

TEST_CASE("buildRateMenu marks every device-native rate as non-resampling") {
    std::vector<double> native { 44100, 48000, 88200, 96000, 176400, 192000 };
    auto items = eb::buildRateMenu (native, /*selectedSr*/ 96000.0);
    REQUIRE (items.size() == 6);
    for (auto& it : items) CHECK (it.resampleWarning == false);
    // The 96 kHz entry is the selected one.
    auto sel = std::find_if (items.begin(), items.end(),
                             [](const eb::RateMenuItem& i){ return i.selected; });
    REQUIRE (sel != items.end());
    CHECK (sel->rate == 96000.0);
}

TEST_CASE("buildRateMenu flags a selected rate the input cannot do natively") {
    std::vector<double> native { 48000 };                 // original EARS
    auto items = eb::buildRateMenu (native, /*selectedSr*/ 96000.0);
    // The 48000 native entry is present and clean...
    REQUIRE (items.size() == 2);
    auto fortyEight = std::find_if (items.begin(), items.end(),
                                    [](const eb::RateMenuItem& i){ return i.rate == 48000.0; });
    REQUIRE (fortyEight != items.end());
    CHECK (fortyEight->resampleWarning == false);
    // ...and the non-native 96000 selection appears, flagged as a resample.
    auto ninetySix = std::find_if (items.begin(), items.end(),
                                   [](const eb::RateMenuItem& i){ return i.rate == 96000.0; });
    REQUIRE (ninetySix != items.end());
    CHECK (ninetySix->resampleWarning == true);
    CHECK (ninetySix->selected == true);
}

TEST_CASE("buildRateMenu without a non-native selection emits exactly the native rates") {
    std::vector<double> native { 48000 };
    auto items = eb::buildRateMenu (native, /*selectedSr*/ 48000.0);
    REQUIRE (items.size() == 1);
    CHECK (items[0].rate == 48000.0);
    CHECK (items[0].resampleWarning == false);
    CHECK (items[0].selected == true);
}

TEST_CASE("combineModeOrder lists modes by rigor with TwoPass recommended") {
    auto order = eb::combineModeOrder();
    REQUIRE (order.size() == 4);
    CHECK (order[0].mode == eb::CombineMode::TwoPassLeft);
    CHECK (order[1].mode == eb::CombineMode::TwoPassRight);
    CHECK (order[2].mode == eb::CombineMode::Average);
    CHECK (order[3].mode == eb::CombineMode::Sum);
    // The two TwoPass single-ear modes carry the Recommended badge; Average/Sum do not.
    CHECK (order[0].recommended == true);
    CHECK (order[1].recommended == true);
    CHECK (order[2].recommended == false);
    CHECK (order[3].recommended == false);
    // Sum carries the +6 dB clip-risk warning.
    CHECK (order[3].clipRiskWarning == true);
}

TEST_CASE("numTapsForRate scales 8192 @ 48k to powers of two") {
    CHECK (eb::numTapsForRate (48000.0)  == 8192);
    CHECK (eb::numTapsForRate (96000.0)  == 16384);
    CHECK (eb::numTapsForRate (192000.0) == 32768);
    CHECK (eb::numTapsForRate (44100.0)  == 8192);   // ~7526 -> nearest pow2 = 8192
    CHECK (eb::numTapsForRate (88200.0)  == 16384);  // ~15053 -> nearest pow2 = 16384
    CHECK (eb::numTapsForRate (176400.0) == 32768);  // ~30106 -> nearest pow2 = 32768
}
```

- [ ] **Run it, expect FAIL (does not compile — `RateMenu.h` missing).**

```
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
```
Expected: build FAILS with `gui/RateMenu.h: No such file or directory`.

- [ ] **Implement `src/gui/RateMenu.h`:**

```cpp
#pragma once
#include "audio/CombineMode.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace eb {

// One row of the sample-rate ComboBox model.
struct RateMenuItem {
    double rate = 0.0;
    bool   selected = false;
    bool   resampleWarning = false;   // true when this rate is NOT a device-native rate
};

// Build the rate menu for a device whose native rates are `native`, given the
// currently-selected rate. Native rates appear in order (clean). If `selectedSr` is
// not among the native rates, it is appended as a flagged (resample-warning) entry so
// the user still sees their forced selection. Exactly one item is marked selected.
inline std::vector<RateMenuItem> buildRateMenu (const std::vector<double>& native,
                                                double selectedSr) {
    std::vector<RateMenuItem> out;
    out.reserve (native.size() + 1);
    bool selectedIsNative = false;
    for (double r : native) {
        const bool sel = std::abs (r - selectedSr) < 0.5;
        if (sel) selectedIsNative = true;
        out.push_back ({ r, sel, /*resampleWarning*/ false });
    }
    if (! selectedIsNative)
        out.push_back ({ selectedSr, /*selected*/ true, /*resampleWarning*/ true });
    return out;
}

// One row of the combine-mode selector, ordered by methodological rigor.
struct CombineMenuItem {
    CombineMode mode;
    bool recommended = false;       // the two-pass single-ear workflow
    bool clipRiskWarning = false;   // Sum: +6 dB level risk
};

// Combine modes ORDERED BY RIGOR (design-spec §6): TwoPass-L, TwoPass-R, Average, Sum.
inline std::vector<CombineMenuItem> combineModeOrder() {
    return {
        { CombineMode::TwoPassLeft,  /*recommended*/ true,  /*clipRisk*/ false },
        { CombineMode::TwoPassRight, /*recommended*/ true,  /*clipRisk*/ false },
        { CombineMode::Average,      /*recommended*/ false, /*clipRisk*/ false },
        { CombineMode::Sum,          /*recommended*/ false, /*clipRisk*/ true  },
    };
}

// Tap count for the FIR at a given rate: N ~ 8192 * (rate / 48000), rounded to the
// nearest power of two (design-spec §7 / SPINE). 8192@48k, 16384@96k, 32768@192k.
inline int numTapsForRate (double rate) {
    const double ideal = 8192.0 * (rate / 48000.0);
    int n = 1;
    while (n < (int) std::lround (ideal)) n <<= 1;
    // n is now the smallest pow2 >= ideal; pick whichever pow2 (n or n/2) is nearer.
    const int lower = n >> 1;
    if (lower >= 1 && (ideal - lower) < (n - ideal)) n = lower;
    return n;
}

} // namespace eb
```

- [ ] **Run, expect PASS.**

```
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure -R buildRateMenu|combineModeOrder|numTapsForRate"
```
Expected: all 6 RateMenu test cases pass. (Sanity-check the `numTapsForRate` rounding: 44100→ideal≈7526, lower=4096, upper=8192; (7526−4096)=3430 vs (8192−7526)=666 → 8192. ✓)

- [ ] **Commit.**

```
git add src/gui/RateMenu.h tests/test_ratemenu.cpp tests/CMakeLists.txt
git commit -m "Plan 3 Task 3: RateMenu/combine-order/numTaps pure helpers"
```

---

## Task 4 — Theme (dark LookAndFeel_V4)

**Files**

- Create: `src/gui/Theme.h`, `src/gui/Theme.cpp`
- Modify: `CMakeLists.txt` (append `src/gui/Theme.cpp` to `eb_gui`)

No unit test (pure visuals); ships with a MANUAL VERIFICATION block exercised in Task 9.

### Steps

- [ ] **Append `src/gui/Theme.cpp` to the `eb_gui` `add_library` list in `CMakeLists.txt`.** The list becomes:

```cmake
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp)
```

- [ ] **Implement `src/gui/Theme.h`:**

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {

// Dark application theme. Subclasses LookAndFeel_V4 with a custom dark ColourScheme
// plus EARS-Bridge brand colours, and exposes named accents for component code to
// pull (meters, badges) instead of hard-coding literals.
class Theme : public juce::LookAndFeel_V4 {
public:
    Theme();

    // Brand palette (also installed into the ColourScheme where applicable).
    static juce::Colour bg()        { return juce::Colour (0xff141619); } // window
    static juce::Colour panel()     { return juce::Colour (0xff1d2024); } // cards/slots
    static juce::Colour outline()   { return juce::Colour (0xff2c3036); }
    static juce::Colour text()      { return juce::Colour (0xffe6e8ea); }
    static juce::Colour textDim()   { return juce::Colour (0xff9aa0a6); }
    static juce::Colour accent()    { return juce::Colour (0xff4ea1ff); } // primary blue
    static juce::Colour ok()        { return juce::Colour (0xff39c07a); } // running / clean
    static juce::Colour warn()      { return juce::Colour (0xfff0a93b); } // resample / HEQ badge
    static juce::Colour danger()    { return juce::Colour (0xffe5484d); } // clip / error
    static juce::Colour meterLo()   { return juce::Colour (0xff39c07a); } // meter green zone
    static juce::Colour meterMid()  { return juce::Colour (0xffe0c43a); } // meter yellow zone
    static juce::Colour meterHi()   { return juce::Colour (0xffe5484d); } // meter red zone
};

} // namespace eb
```

- [ ] **Implement `src/gui/Theme.cpp`:**

```cpp
#include "gui/Theme.h"

namespace eb {

Theme::Theme() {
    using juce::ColourScheme;
    ColourScheme scheme = {
        bg(),          // windowBackground
        panel(),       // widgetBackground
        outline(),     // menuBackground
        outline(),     // outline
        text(),        // defaultText
        accent(),      // defaultFill
        text(),        // highlightedText
        accent(),      // highlightedFill
        text()         // menuText
    };
    setColourScheme (scheme);

    // Direct component-colour overrides for the controls the GUI uses.
    setColour (juce::ResizableWindow::backgroundColourId,    bg());
    setColour (juce::Label::textColourId,                    text());
    setColour (juce::ComboBox::backgroundColourId,           panel());
    setColour (juce::ComboBox::textColourId,                 text());
    setColour (juce::ComboBox::outlineColourId,              outline());
    setColour (juce::ComboBox::arrowColourId,                accent());
    setColour (juce::PopupMenu::backgroundColourId,          panel());
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent().withAlpha (0.35f));
    setColour (juce::TextButton::buttonColourId,             panel());
    setColour (juce::TextButton::buttonOnColourId,           accent());
    setColour (juce::TextButton::textColourOnId,             juce::Colours::black);
    setColour (juce::TextButton::textColourOffId,            text());
    setColour (juce::Slider::thumbColourId,                  accent());
    setColour (juce::Slider::trackColourId,                  accent().withAlpha (0.6f));
    setColour (juce::Slider::backgroundColourId,             outline());
    setColour (juce::ToggleButton::textColourId,             text());
    setColour (juce::ToggleButton::tickColourId,             accent());
    setColour (juce::TooltipWindow::backgroundColourId,      panel());
    setColour (juce::TooltipWindow::textColourId,            text());
}

} // namespace eb
```

- [ ] **Build the `eb_gui` lib to confirm it compiles** (no test target yet links Theme functionally, so build the lib directly):

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_gui"
```
Expected: `eb_gui` builds with no errors.

- [ ] **MANUAL VERIFICATION (deferred to Task 9 window):** When the app first renders in Task 9, confirm: window background is near-black charcoal (`#141619`), text is light grey and legible, ComboBox arrows and the Start button highlight are blue (`#4ea1ff`). PASS = no default JUCE grey theme visible anywhere; all panels read as dark cards on a darker window.

- [ ] **Commit.**

```
git add src/gui/Theme.h src/gui/Theme.cpp CMakeLists.txt
git commit -m "Plan 3 Task 4: dark LookAndFeel_V4 theme"
```

---

## Task 5 — LevelMeter

**Files**

- Create: `src/gui/LevelMeter.h`, `src/gui/LevelMeter.cpp`
- Modify: `CMakeLists.txt` (append `src/gui/LevelMeter.cpp` to `eb_gui`)

GUI rendering → MANUAL VERIFICATION. The dB-mapping it uses is `PlotMath`-independent (linear amplitude → meter fill), kept trivial and self-contained.

### Steps

- [ ] **Append `src/gui/LevelMeter.cpp` to the `eb_gui` `add_library` list** so it reads:

```cmake
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp)
```

- [ ] **Implement `src/gui/LevelMeter.h`:**

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {

// A vertical level meter: green->yellow->red gradient fill scaled by the supplied
// linear peak amplitude (0..1+), with a short-decay peak-hold tick and a clip LED.
// The owning component pushes new values from its GUI timer via setLevel(); the meter
// does no audio-thread work itself.
class LevelMeter : public juce::Component {
public:
    explicit LevelMeter (juce::String caption = {});

    // peak: linear amplitude (0 = silence, 1 = full scale). clip: latch the red LED.
    void setLevel (float peakLinear, bool clip);

    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float level = 0.0f;       // smoothed display level (linear)
    float peakHold = 0.0f;    // decaying peak marker (linear)
    bool  clipLatched = false;

    static float linearToFrac (float linear);   // map linear amp -> 0..1 bar fill via dBFS
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace eb
```

- [ ] **Implement `src/gui/LevelMeter.cpp`:**

```cpp
#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

LevelMeter::LevelMeter (juce::String caption) : label (std::move (caption)) {
    setOpaque (false);
}

float LevelMeter::linearToFrac (float linear) {
    // Display window: -60 dBFS (bottom) .. 0 dBFS (top).
    constexpr float floorDb = -60.0f;
    const float db = (linear <= 1.0e-6f) ? floorDb : 20.0f * std::log10 (linear);
    const float t  = juce::jlimit (0.0f, 1.0f, (db - floorDb) / (0.0f - floorDb));
    return t;
}

void LevelMeter::setLevel (float peakLinear, bool clip) {
    // Fast attack, slow release for a readable bar.
    level = juce::jmax (peakLinear, level * 0.80f);
    peakHold = juce::jmax (peakLinear, peakHold * 0.95f);
    if (clip) clipLatched = true;        // latches until a manual repaint cycle clears it
    repaint();
}

void LevelMeter::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    // Caption + clip LED strip at the top.
    auto ledArea = r.removeFromTop (10.0f);
    g.setColour (clipLatched ? Theme::danger() : Theme::outline());
    g.fillRoundedRectangle (ledArea.removeFromRight (10.0f), 2.0f);
    if (label.isNotEmpty()) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (11.0f));
        g.drawText (label, ledArea, juce::Justification::centredLeft);
    }

    r.removeFromTop (2.0f);
    // Track.
    g.setColour (Theme::panel());
    g.fillRoundedRectangle (r, 3.0f);

    // Filled portion (bottom-up).
    const float frac = linearToFrac (level);
    auto fill = r.withTop (r.getBottom() - frac * r.getHeight());
    juce::ColourGradient grad (Theme::meterLo(), fill.getX(), fill.getBottom(),
                               Theme::meterHi(), fill.getX(), r.getY(), false);
    grad.addColour (0.7, Theme::meterMid());
    g.setGradientFill (grad);
    g.fillRoundedRectangle (fill, 3.0f);

    // Peak-hold tick.
    const float py = r.getBottom() - linearToFrac (peakHold) * r.getHeight();
    g.setColour (Theme::text());
    g.fillRect (r.getX(), py - 1.0f, r.getWidth(), 2.0f);

    g.setColour (Theme::outline());
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
}

} // namespace eb
```

- [ ] **Build the lib to confirm it compiles.**

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_gui"
```
Expected: `eb_gui` builds clean.

- [ ] **MANUAL VERIFICATION (in Task 9 window, with the bridge Running and a signal into the EARS):**
  - The L and R input meters rise/fall with the signal; bar fills from the bottom; colour shifts green→yellow→red as level climbs.
  - A peak-hold tick floats above the bar and decays slowly.
  - Driving the input near full scale latches the clip LED red at the top of the meter.
  - PASS = meters track audibly-correct levels and the clip LED only lights on near-full-scale signal.

- [ ] **Commit.**

```
git add src/gui/LevelMeter.h src/gui/LevelMeter.cpp CMakeLists.txt
git commit -m "Plan 3 Task 5: vertical LevelMeter with peak-hold and clip LED"
```

---

## Task 6 — CurveThumbnail and CalSlotComponent

**Files**

- Create: `src/gui/CurveThumbnail.h`, `src/gui/CurveThumbnail.cpp`
- Create: `src/gui/CalSlotComponent.h`, `src/gui/CalSlotComponent.cpp`
- Modify: `CMakeLists.txt` (append both `.cpp` to `eb_gui`)

`CurveThumbnail` uses the Task-2 `PlotMath` transform (already unit-tested). Rendering itself → MANUAL VERIFICATION. The cal-parse / HEQ-warning logic reuses Plan 1 `eb::CalFile`.

### Steps

- [ ] **Append both `.cpp` files to the `eb_gui` `add_library` list:**

```cmake
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp)
```

- [ ] **Implement `src/gui/CurveThumbnail.h`:**

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "cal/CalFile.h"
#include <optional>

namespace eb {

// Static FR plot of a parsed CalFile: log-x 20 Hz..20 kHz, linear-dB y. Draws the
// raw cal curve (as loaded) so the user can eyeball the EARS canal resonance bump.
// Empty (placeholder) until setCalFile() is given a curve.
class CurveThumbnail : public juce::Component {
public:
    CurveThumbnail();
    void setCalFile (const eb::CalFile& cal);
    void clear();
    void paint (juce::Graphics&) override;

private:
    std::optional<eb::CalFile> curve;
    float topDb = 24.0f, botDb = -24.0f;   // auto-fit to the data on setCalFile
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveThumbnail)
};

} // namespace eb
```

- [ ] **Implement `src/gui/CurveThumbnail.cpp`:**

```cpp
#include "gui/CurveThumbnail.h"
#include "gui/PlotMath.h"
#include "gui/Theme.h"
#include <algorithm>
#include <cmath>

namespace eb {

CurveThumbnail::CurveThumbnail() { setOpaque (false); }

void CurveThumbnail::clear() { curve.reset(); repaint(); }

void CurveThumbnail::setCalFile (const eb::CalFile& cal) {
    curve = cal;
    // Auto-fit the dB axis to the data with a small margin, then snap to a symmetric
    // range so 0 dB stays centred and the grid reads cleanly.
    float lo = 0.0f, hi = 0.0f;
    for (auto& p : cal.points) {
        lo = std::min (lo, (float) p.splDb);
        hi = std::max (hi, (float) p.splDb);
    }
    const float mag = std::max ({ 6.0f, std::abs (lo), std::abs (hi) }) + 3.0f;
    topDb =  std::ceil (mag / 6.0f) * 6.0f;   // round up to a 6 dB multiple
    botDb = -topDb;
    repaint();
}

void CurveThumbnail::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (Theme::panel());
    g.fillRoundedRectangle (r, 4.0f);

    const float W = r.getWidth(), H = r.getHeight();

    // Grid: decade verticals (100, 1k, 10k) + 0 dB horizontal.
    g.setColour (Theme::outline());
    for (float f : { 100.0f, 1000.0f, 10000.0f }) {
        const float x = r.getX() + freqToX (f, W);
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
    }
    const float zeroY = r.getY() + dbToY (0.0f, H, topDb, botDb);
    g.drawHorizontalLine ((int) zeroY, r.getX(), r.getRight());

    if (! curve || curve->points.size() < 2) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (11.0f));
        g.drawText ("no cal loaded", r, juce::Justification::centred);
        return;
    }

    // The cal curve.
    juce::Path path;
    bool started = false;
    for (auto& p : curve->points) {
        const float x = r.getX() + freqToX ((float) p.freqHz, W);
        const float y = r.getY() + dbToY ((float) p.splDb, H, topDb, botDb);
        if (! started) { path.startNewSubPath (x, y); started = true; }
        else            path.lineTo (x, y);
    }
    g.setColour (Theme::accent());
    g.strokePath (path, juce::PathStrokeType (1.5f));

    // dB axis labels (top/bottom of the fitted range).
    g.setColour (Theme::textDim());
    g.setFont (juce::Font (10.0f));
    g.drawText (juce::String ((int) topDb) + " dB",
                r.removeFromTop (12.0f), juce::Justification::topLeft);
}

} // namespace eb
```

- [ ] **Implement `src/gui/CalSlotComponent.h`:**

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "cal/CalFile.h"
#include "gui/CurveThumbnail.h"
#include <functional>
#include <optional>

namespace eb {

// One calibration slot ("Left ear cal" / "Right ear cal"). Accepts a dropped .txt/.frd
// file, parses it via eb::CalFile, shows the filename + parsed serial + type tag, draws
// the static FR thumbnail, and raises a non-blocking warning badge if an HEQ file is
// loaded as a mic-cal (double-target risk, design-spec §3.7). Fires onCalLoaded with the
// absolute file path when a valid cal is parsed.
class CalSlotComponent : public juce::Component,
                         public juce::FileDragAndDropTarget {
public:
    explicit CalSlotComponent (juce::String title);

    // Programmatically load a path (used to restore from Settings). Returns false and
    // shows an error line on parse failure (keeps any previously valid curve).
    bool loadFromFile (const juce::File&);

    std::optional<eb::CalFile> calFile() const { return cal; }

    std::function<void (const juce::File&)> onCalLoaded;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit  (const juce::StringArray&) override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void applyParsed (const eb::CalFile&, const juce::File&);

    juce::String slotTitle;
    juce::Label  fileLabel;     // filename
    juce::Label  detailLabel;   // "Serial 860-4350 - HPN"
    juce::Label  errorLabel;    // parse error line (red), hidden unless set
    CurveThumbnail thumbnail;
    std::optional<eb::CalFile> cal;
    bool heqWarning = false;    // HEQ loaded as mic-cal
    bool dragHover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalSlotComponent)
};

} // namespace eb
```

- [ ] **Implement `src/gui/CalSlotComponent.cpp`:**

```cpp
#include "gui/CalSlotComponent.h"
#include "gui/Theme.h"

namespace eb {

static juce::String typeName (eb::CalType t) {
    switch (t) {
        case eb::CalType::Hpn: return "HPN";
        case eb::CalType::Heq: return "HEQ";
        case eb::CalType::Raw: return "RAW";
        case eb::CalType::Idf: return "IDF";
        default:               return "Unknown";
    }
}

CalSlotComponent::CalSlotComponent (juce::String title) : slotTitle (std::move (title)) {
    addAndMakeVisible (thumbnail);

    fileLabel.setColour (juce::Label::textColourId, Theme::text());
    fileLabel.setFont (juce::Font (13.0f, juce::Font::bold));
    fileLabel.setText ("Drop a cal file (.txt / .frd)", juce::dontSendNotification);
    addAndMakeVisible (fileLabel);

    detailLabel.setColour (juce::Label::textColourId, Theme::textDim());
    detailLabel.setFont (juce::Font (11.0f));
    addAndMakeVisible (detailLabel);

    errorLabel.setColour (juce::Label::textColourId, Theme::danger());
    errorLabel.setFont (juce::Font (11.0f));
    addAndMakeVisible (errorLabel);
}

bool CalSlotComponent::isInterestedInFileDrag (const juce::StringArray& files) {
    for (auto& f : files) {
        auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".txt" || ext == ".frd") return true;
    }
    return false;
}

void CalSlotComponent::fileDragEnter (const juce::StringArray&, int, int) {
    dragHover = true; repaint();
}
void CalSlotComponent::fileDragExit (const juce::StringArray&) {
    dragHover = false; repaint();
}

void CalSlotComponent::filesDropped (const juce::StringArray& files, int, int) {
    dragHover = false;
    for (auto& f : files) {
        juce::File file (f);
        auto ext = file.getFileExtension().toLowerCase();
        if (ext == ".txt" || ext == ".frd") { loadFromFile (file); break; }
    }
    repaint();
}

bool CalSlotComponent::loadFromFile (const juce::File& file) {
    if (! file.existsAsFile()) {
        errorLabel.setText ("File not found: " + file.getFileName(), juce::dontSendNotification);
        repaint();
        return false;
    }
    try {
        auto parsed = eb::CalFile::parse (file.loadFileAsString());
        applyParsed (parsed, file);
        if (onCalLoaded) onCalLoaded (file);
        return true;
    } catch (const eb::CalParseError& e) {
        // Keep any previously valid curve; surface the error with context.
        errorLabel.setText (juce::String ("Parse error: ") + e.what(), juce::dontSendNotification);
        repaint();
        return false;
    }
}

void CalSlotComponent::applyParsed (const eb::CalFile& parsed, const juce::File& file) {
    cal = parsed;
    errorLabel.setText ({}, juce::dontSendNotification);
    fileLabel.setText (file.getFileName(), juce::dontSendNotification);
    juce::String detail = "Serial " + (parsed.serial.isNotEmpty() ? parsed.serial : juce::String ("?"))
                        + "  -  " + typeName (parsed.type);
    detailLabel.setText (detail, juce::dontSendNotification);
    heqWarning = (parsed.type == eb::CalType::Heq);
    thumbnail.setCalFile (parsed);
    repaint();
}

void CalSlotComponent::resized() {
    auto r = getLocalBounds().reduced (8);
    r.removeFromTop (18);                 // title strip (painted)
    auto top = r.removeFromTop (20);
    fileLabel.setBounds (top);
    detailLabel.setBounds (r.removeFromTop (16));
    errorLabel.setBounds (r.removeFromTop (14));
    r.removeFromTop (4);
    thumbnail.setBounds (r);
}

void CalSlotComponent::paint (juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    g.setColour (Theme::panel());
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (dragHover ? Theme::accent() : Theme::outline());
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, dragHover ? 2.0f : 1.0f);

    // Title.
    g.setColour (Theme::textDim());
    g.setFont (juce::Font (11.0f, juce::Font::bold));
    g.drawText (slotTitle, getLocalBounds().reduced (10, 6).removeFromTop (16),
                juce::Justification::topLeft);

    // HEQ-as-mic-cal warning badge (non-blocking).
    if (heqWarning) {
        auto badge = getLocalBounds().reduced (8).removeFromTop (18)
                         .removeFromRight (132).toFloat();
        g.setColour (Theme::warn());
        g.fillRoundedRectangle (badge, 4.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::Font (10.0f, juce::Font::bold));
        g.drawText ("HEQ - double-target?", badge, juce::Justification::centred);
    }
}

} // namespace eb
```

- [ ] **Build the lib to confirm it compiles.**

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_gui"
```
Expected: `eb_gui` builds clean.

- [ ] **MANUAL VERIFICATION (in Task 9 window):**
  - Drag `R_HPN_8604350.txt` onto the "Left ear cal" slot. PASS = filename shows, detail reads `Serial 860-4350  -  HPN`, the thumbnail draws a curve with a visible dip near 4 kHz (the loaded HPN is the *cut* curve), no warning badge.
  - Drag an `L_HEQ_*.txt` onto a slot. PASS = orange `HEQ - double-target?` badge appears top-right; the curve still renders; nothing blocks.
  - Drag a malformed/non-monotonic file. PASS = red error line appears with context; the previously loaded curve stays drawn.
  - PASS overall = drag highlights the slot border blue while hovering; the thumbnail's 0 dB line and decade gridlines are visible.

- [ ] **Commit.**

```
git add src/gui/CurveThumbnail.h src/gui/CurveThumbnail.cpp src/gui/CalSlotComponent.h src/gui/CalSlotComponent.cpp CMakeLists.txt
git commit -m "Plan 3 Task 6: CurveThumbnail + drag-drop CalSlotComponent"
```

---

## Task 7 — DevicePicker

**Files**

- Create: `src/gui/DevicePicker.h`, `src/gui/DevicePicker.cpp`
- Modify: `CMakeLists.txt` (append `src/gui/DevicePicker.cpp` to `eb_gui`)

Consumes the SPINE `eb::DeviceId` / `eb::EarsModel`. GUI interaction → MANUAL VERIFICATION; the device-row-formatting (model tag string) is small and self-contained.

### Steps

- [ ] **Append `src/gui/DevicePicker.cpp` to the `eb_gui` `add_library` list:**

```cmake
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp
    src/gui/DevicePicker.cpp)
```

- [ ] **Implement `src/gui/DevicePicker.h`:**

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/DeviceId.h"     // eb::DeviceId, eb::EarsModel (Plan 2 SPINE)
#include <vector>
#include <functional>
#include <optional>

namespace eb {

// A labelled ComboBox bound to a list of DeviceId. Each row shows the device name and,
// for inputs, the detected EARS / EARS Pro model tag; virtual sinks are tagged
// "(virtual)" for outputs. Selecting a row fires onDeviceChosen with the DeviceId.
class DevicePicker : public juce::Component {
public:
    explicit DevicePicker (juce::String caption);

    // Repopulate from a device list, preselecting the device whose key() matches
    // selectedKey (if present). Does NOT fire onDeviceChosen.
    void setDevices (const std::vector<DeviceId>& devices, const juce::String& selectedKey = {});

    std::optional<DeviceId> selectedDevice() const;

    std::function<void (const DeviceId&)> onDeviceChosen;

    void resized() override;

private:
    static juce::String rowText (const DeviceId&);

    juce::Label label;
    juce::ComboBox combo;
    std::vector<DeviceId> items;   // index i -> combo item id (i+1)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DevicePicker)
};

} // namespace eb
```

- [ ] **Implement `src/gui/DevicePicker.cpp`:**

```cpp
#include "gui/DevicePicker.h"
#include "gui/Theme.h"

namespace eb {

static juce::String modelTag (EarsModel m) {
    switch (m) {
        case EarsModel::Ears:    return "  [EARS]";
        case EarsModel::EarsPro: return "  [EARS Pro]";
        default:                 return {};
    }
}

DevicePicker::DevicePicker (juce::String caption) {
    label.setText (caption, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, Theme::textDim());
    label.setFont (juce::Font (11.0f, juce::Font::bold));
    addAndMakeVisible (label);

    combo.setTextWhenNoChoicesAvailable ("no devices found");
    combo.setTextWhenNothingSelected ("select a device");
    addAndMakeVisible (combo);

    combo.onChange = [this] {
        const int idx = combo.getSelectedId() - 1;
        if (idx >= 0 && idx < (int) items.size() && onDeviceChosen)
            onDeviceChosen (items[(size_t) idx]);
    };
}

juce::String DevicePicker::rowText (const DeviceId& d) {
    juce::String t = d.name;
    t += modelTag (d.model);
    if (d.isVirtualSink) t += "  (virtual)";
    return t;
}

void DevicePicker::setDevices (const std::vector<DeviceId>& devices, const juce::String& selectedKey) {
    items = devices;
    combo.clear (juce::dontSendNotification);
    int selectId = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const int id = (int) i + 1;
        combo.addItem (rowText (items[i]), id);
        if (selectedKey.isNotEmpty() && items[i].key() == selectedKey) selectId = id;
    }
    if (selectId != 0) combo.setSelectedId (selectId, juce::dontSendNotification);
}

std::optional<DeviceId> DevicePicker::selectedDevice() const {
    const int idx = combo.getSelectedId() - 1;
    if (idx >= 0 && idx < (int) items.size()) return items[(size_t) idx];
    return std::nullopt;
}

void DevicePicker::resized() {
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    combo.setBounds (r.removeFromTop (28));
}

} // namespace eb
```

> **SPINE include:** `audio/DeviceId.h` is owned by Plan 2. It declares `struct DeviceId { ... juce::String key() const; ... EarsModel model; }` and `enum class EarsModel` exactly as in the SPINE. If Plan 2 is unmerged, a minimal `src/audio/DeviceId.h` matching the SPINE signatures unblocks compilation; Plan 2 replaces it.

- [ ] **Build the lib to confirm it compiles.**

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_gui"
```
Expected: `eb_gui` builds clean (against the real or shim `DeviceId.h`).

- [ ] **MANUAL VERIFICATION (in Task 9 window):**
  - With an EARS Pro connected, the input picker lists it as `EARS Pro ...  [EARS Pro]`; with the original EARS, `... [EARS]`. PASS = the correct model tag appears.
  - The output picker lists VB-CABLE / BlackHole tagged `(virtual)`. PASS = virtual sinks are visually distinguished.
  - Selecting a device fires `onDeviceChosen` (verify via the rate/bit-depth menu refresh in Task 9). PASS = changing the input device repopulates the rate menu.

- [ ] **Commit.**

```
git add src/gui/DevicePicker.h src/gui/DevicePicker.cpp CMakeLists.txt
git commit -m "Plan 3 Task 7: DevicePicker ComboBox with model/virtual tags"
```

---

## Task 8 — MainComponent (layout + engine wiring)

**Files**

- Create: `src/gui/MainComponent.h`, `src/gui/MainComponent.cpp`
- Modify: `CMakeLists.txt` (append `src/gui/MainComponent.cpp` to `eb_gui`)

This is the top-level integration: owns `eb::AudioEngine` + `eb::Settings`, lays out §8, polls `levels()`/`health()` from a `juce::Timer`, and reconfigures the engine + rebuilds FIRs on device/rate changes. No new TDD units (its leaf helpers — PlotMath, RateMenu, Settings — are already tested); behaviour → MANUAL VERIFICATION.

### Steps

- [ ] **Append `src/gui/MainComponent.cpp` to the `eb_gui` `add_library` list:**

```cmake
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp
    src/gui/DevicePicker.cpp
    src/gui/MainComponent.cpp)
```

- [ ] **Implement `src/gui/MainComponent.h`:**

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/AudioEngine.h"      // eb::AudioEngine (Plan 2 SPINE)
#include "cal/FirDesigner.h"        // eb::FirDesigner (Plan 1)
#include "state/Settings.h"
#include "gui/Theme.h"
#include "gui/DevicePicker.h"
#include "gui/CalSlotComponent.h"
#include "gui/LevelMeter.h"
#include "gui/RateMenu.h"
#include <memory>

namespace eb {

// Top-level window content (design-spec §8). Owns the AudioEngine and Settings,
// builds the full layout, polls engine telemetry on a timer, and reconfigures the
// engine on user actions. FIRs are (re)designed on a worker thread at the active rate.
class MainComponent : public juce::Component,
                      public juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void timerCallback() override;   // polls levels()/health()

private:
    void refreshDeviceLists();
    void onInputChosen  (const DeviceId&);
    void onOutputChosen (const DeviceId&);
    void rebuildRateMenu();
    void rebuildBitDepthMenu();
    void onRateChosen();
    void onBitDepthChosen();
    void onCombineChosen();
    void rebuildFirsAsync();         // design both FIRs at the active rate off-thread
    void onLeftCalLoaded  (const juce::File&);
    void onRightCalLoaded (const juce::File&);
    void onStartStop();
    void updateStatusLine();
    double activeRate() const;

    Theme theme;
    AudioEngine engine;
    Settings settings;

    // Input row.
    DevicePicker inputPicker { "INPUT (EARS)" };
    // Cal slots.
    CalSlotComponent leftCal  { "LEFT EAR CAL" };
    CalSlotComponent rightCal { "RIGHT EAR CAL" };
    // Combine.
    juce::Label    combineLabel;
    juce::ComboBox combineBox;
    juce::Label    combineHint;
    // Output row.
    DevicePicker outputPicker { "OUTPUT (VIRTUAL SINK)" };
    juce::Label  outputHint;     // "set this device's capture side as Dirac's Recording device"
    juce::Label  preflightLabel; // warnings (yellow)
    // Rate + bit depth.
    juce::Label    rateLabel;    juce::ComboBox rateBox;    juce::Label rateWarn;
    juce::Label    bitLabel;     juce::ComboBox bitBox;
    // Meters.
    LevelMeter meterL { "L" }, meterR { "R" }, meterOut { "OUT" };
    juce::Label cleanLight;      // "clean capture" status dot text
    // Transport.
    juce::TextButton startStop { "Start" };
    juce::Label statusLine;
    // Advanced disclosure.
    juce::ToggleButton advancedToggle { "Advanced" };
    juce::ToggleButton complexPhaseToggle { "Complex (with-phase) FIR" };
    juce::Label    firLenLabel; juce::ComboBox firLenBox;
    juce::Label    trimLabel;   juce::Slider trimSlider;

    // FIR-length combo item id for the "Auto (scales with rate)" choice. Distinct from every
    // real tap count (4096/8192/16384/32768) so it never collides with an explicit override.
    static constexpr int kFirLenAutoId = 1;

    std::vector<RateMenuItem> rateModel;       // index -> combo id (i+1)
    std::vector<int> bitModel;                 // index -> combo id (i+1)
    std::vector<CombineMenuItem> combineModel; // index -> combo id (i+1)

    std::unique_ptr<juce::ThreadPool> firPool;  // off-thread FIR design

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace eb
```

- [ ] **Implement `src/gui/MainComponent.cpp`:**

```cpp
#include "gui/MainComponent.h"

namespace eb {

MainComponent::MainComponent() {
    setLookAndFeel (&theme);
    firPool = std::make_unique<juce::ThreadPool> (1);

    // --- Input picker ---
    inputPicker.onDeviceChosen = [this] (const DeviceId& d) { onInputChosen (d); };
    addAndMakeVisible (inputPicker);

    // --- Cal slots ---
    leftCal.onCalLoaded  = [this] (const juce::File& f) { onLeftCalLoaded (f); };
    rightCal.onCalLoaded = [this] (const juce::File& f) { onRightCalLoaded (f); };
    addAndMakeVisible (leftCal);
    addAndMakeVisible (rightCal);

    // --- Combine selector (ordered by rigor) ---
    combineLabel.setText ("COMBINE MODE", juce::dontSendNotification);
    combineLabel.setColour (juce::Label::textColourId, Theme::textDim());
    combineLabel.setFont (juce::Font (11.0f, juce::Font::bold));
    addAndMakeVisible (combineLabel);

    combineModel = combineModeOrder();
    for (size_t i = 0; i < combineModel.size(); ++i) {
        auto& m = combineModel[i];
        juce::String label;
        switch (m.mode) {
            case CombineMode::TwoPassLeft:  label = "Two-pass: Left ear only";  break;
            case CombineMode::TwoPassRight: label = "Two-pass: Right ear only"; break;
            case CombineMode::Average:      label = "Average (L+R)/2";          break;
            case CombineMode::Sum:          label = "Sum L+R";                  break;
        }
        if (m.recommended)     label += "   [Recommended]";
        if (m.clipRiskWarning) label += "   (+6 dB clip risk)";
        combineBox.addItem (label, (int) i + 1);
    }
    combineBox.onChange = [this] { onCombineChosen(); };
    addAndMakeVisible (combineBox);

    combineHint.setText ("Two-pass single-ear mirrors miniDSP's official method: "
                         "route Dirac playback to one earcup per pass.",
                         juce::dontSendNotification);
    combineHint.setColour (juce::Label::textColourId, Theme::textDim());
    combineHint.setFont (juce::Font (10.5f));
    combineHint.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (combineHint);

    // --- Output picker + hint + preflight ---
    outputPicker.onDeviceChosen = [this] (const DeviceId& d) { onOutputChosen (d); };
    addAndMakeVisible (outputPicker);

    outputHint.setText ("In Dirac Live, pick this device's CAPTURE side as the Recording device.",
                        juce::dontSendNotification);
    outputHint.setColour (juce::Label::textColourId, Theme::textDim());
    outputHint.setFont (juce::Font (10.5f));
    addAndMakeVisible (outputHint);

    preflightLabel.setColour (juce::Label::textColourId, Theme::warn());
    preflightLabel.setFont (juce::Font (10.5f));
    addAndMakeVisible (preflightLabel);

    // --- Rate + bit depth ---
    rateLabel.setText ("SAMPLE RATE", juce::dontSendNotification);
    rateLabel.setColour (juce::Label::textColourId, Theme::textDim());
    rateLabel.setFont (juce::Font (11.0f, juce::Font::bold));
    addAndMakeVisible (rateLabel);
    rateBox.onChange = [this] { onRateChosen(); };
    addAndMakeVisible (rateBox);
    rateWarn.setColour (juce::Label::textColourId, Theme::warn());
    rateWarn.setFont (juce::Font (10.5f));
    addAndMakeVisible (rateWarn);

    bitLabel.setText ("OUTPUT BIT DEPTH", juce::dontSendNotification);
    bitLabel.setColour (juce::Label::textColourId, Theme::textDim());
    bitLabel.setFont (juce::Font (11.0f, juce::Font::bold));
    addAndMakeVisible (bitLabel);
    bitBox.onChange = [this] { onBitDepthChosen(); };
    addAndMakeVisible (bitBox);

    // --- Meters ---
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);
    addAndMakeVisible (meterOut);
    cleanLight.setColour (juce::Label::textColourId, Theme::ok());
    cleanLight.setFont (juce::Font (11.0f, juce::Font::bold));
    cleanLight.setText ("clean", juce::dontSendNotification);
    addAndMakeVisible (cleanLight);

    // --- Transport ---
    startStop.onClick = [this] { onStartStop(); };
    addAndMakeVisible (startStop);
    statusLine.setColour (juce::Label::textColourId, Theme::textDim());
    statusLine.setFont (juce::Font (11.0f));
    statusLine.setText ("Stopped", juce::dontSendNotification);
    addAndMakeVisible (statusLine);

    // --- Advanced ---
    addAndMakeVisible (advancedToggle);
    advancedToggle.onClick = [this] { resized(); };

    complexPhaseToggle.onClick = [this] {
        settings.setComplexPhase (complexPhaseToggle.getToggleState());
        rebuildFirsAsync();
    };
    addChildComponent (complexPhaseToggle);

    firLenLabel.setText ("FIR LENGTH", juce::dontSendNotification);
    firLenLabel.setColour (juce::Label::textColourId, Theme::textDim());
    firLenLabel.setFont (juce::Font (10.5f, juce::Font::bold));
    addChildComponent (firLenLabel);
    // "Auto" (item id kFirLenAutoId) means taps scale with the rate via numTapsForRate();
    // the explicit overrides keep item id == tap count. 32768 stays valid for 192 kHz.
    firLenBox.addItem ("Auto (scales with rate)", kFirLenAutoId);
    for (int n : { 4096, 8192, 16384, 32768 }) firLenBox.addItem (juce::String (n), n);
    firLenBox.onChange = [this] {
        const int id = firLenBox.getSelectedId();
        settings.setFirLength (id == kFirLenAutoId ? 0 : id);   // 0 = Auto sentinel in Settings
        rebuildFirsAsync();
    };
    addChildComponent (firLenBox);

    trimLabel.setText ("OUTPUT TRIM (dB)", juce::dontSendNotification);
    trimLabel.setColour (juce::Label::textColourId, Theme::textDim());
    trimLabel.setFont (juce::Font (10.5f, juce::Font::bold));
    addChildComponent (trimLabel);
    trimSlider.setRange (-24.0, 0.0, 0.1);
    trimSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    trimSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    trimSlider.onValueChange = [this] { settings.setOutputTrimDb (trimSlider.getValue()); };
    addChildComponent (trimSlider);

    // --- Restore persisted state ---
    combineBox.setSelectedId ((int) settings.combineMode() + 1, juce::dontSendNotification);
    complexPhaseToggle.setToggleState (settings.complexPhase(), juce::dontSendNotification);
    firLenBox.setSelectedId (settings.firLength() > 0 ? settings.firLength() : kFirLenAutoId,
                             juce::dontSendNotification);   // 0 (Auto) -> the Auto item
    trimSlider.setValue (settings.outputTrimDb(), juce::dontSendNotification);

    refreshDeviceLists();
    rebuildRateMenu();
    rebuildBitDepthMenu();

    if (settings.leftCalPath().isNotEmpty())
        leftCal.loadFromFile (juce::File (settings.leftCalPath()));
    if (settings.rightCalPath().isNotEmpty())
        rightCal.loadFromFile (juce::File (settings.rightCalPath()));

    setSize (980, 720);
    startTimerHz (30);   // poll levels()/health()
}

MainComponent::~MainComponent() {
    stopTimer();
    if (firPool) firPool->removeAllJobs (true, 2000);
    engine.stop();
    setLookAndFeel (nullptr);
}

double MainComponent::activeRate() const { return settings.sampleRate(); }

void MainComponent::refreshDeviceLists() {
    inputPicker.setDevices  (engine.inputDevices(),  settings.inputKey());
    outputPicker.setDevices (engine.outputDevices(), settings.outputKey());
}

void MainComponent::onInputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;   // configure only while Stopped
    engine.setInput (d);
    settings.setInputKey (d.key());
    settings.setInputModel (d.model);

    // Snap the selected rate to a native rate if the new device can't do the old one.
    auto rates = engine.supportedSampleRates (d);
    if (! rates.empty()) {
        bool ok = false;
        for (double r : rates) if (std::abs (r - settings.sampleRate()) < 0.5) ok = true;
        if (! ok) { settings.setSampleRate (rates.front()); engine.setSampleRate (rates.front()); }
    }
    rebuildRateMenu();
    rebuildBitDepthMenu();
    rebuildFirsAsync();   // taps depend on the rate, which may have just changed
    updateStatusLine();
}

void MainComponent::onOutputChosen (const DeviceId& d) {
    if (engine.status() == EngineStatus::Running) return;
    engine.setOutput (d);
    settings.setOutputKey (d.key());
    // Preflight note for the recommended Dirac flow.
    preflightLabel.setText (d.isVirtualSink ? juce::String()
                                            : "Selected output is not a known virtual sink.",
                            juce::dontSendNotification);
    rebuildBitDepthMenu();
    updateStatusLine();
}

void MainComponent::rebuildRateMenu() {
    auto sel = inputPicker.selectedDevice();
    std::vector<double> native = sel ? engine.supportedSampleRates (*sel)
                                     : std::vector<double> { 48000.0 };
    rateModel = buildRateMenu (native, settings.sampleRate());

    rateBox.clear (juce::dontSendNotification);
    int selectId = 0;
    bool warn = false;
    for (size_t i = 0; i < rateModel.size(); ++i) {
        auto& it = rateModel[i];
        juce::String txt = juce::String (it.rate / 1000.0, 1) + " kHz";
        if (it.resampleWarning) txt += "  (resample)";
        rateBox.addItem (txt, (int) i + 1);
        if (it.selected) { selectId = (int) i + 1; warn = it.resampleWarning; }
    }
    if (selectId != 0) rateBox.setSelectedId (selectId, juce::dontSendNotification);
    rateWarn.setText (warn ? "This rate is not native to the selected input - it will be resampled."
                           : juce::String(),
                      juce::dontSendNotification);
}

void MainComponent::rebuildBitDepthMenu() {
    auto sel = outputPicker.selectedDevice();
    // Offer the device's full supported output depths: EARS Pro -> {16,24,32}, EARS -> {24}.
    // 24-bit is the default; 32-bit is offered only where the sink accepts it (e.g. BlackHole)
    // and carries no measurement benefit beyond 24-bit. No clamp to {16,24}.
    bitModel = sel ? engine.supportedBitDepths (*sel) : std::vector<int> { 16, 24, 32 };
    std::vector<int> allowed;
    for (int b : bitModel) if (b == 16 || b == 24 || b == 32) allowed.push_back (b);
    if (allowed.empty()) allowed = { 24 };
    bitModel = allowed;

    bitBox.clear (juce::dontSendNotification);
    int selectId = 0;
    for (size_t i = 0; i < bitModel.size(); ++i) {
        bitBox.addItem (juce::String (bitModel[i]) + "-bit", (int) i + 1);
        if (bitModel[i] == settings.outputBitDepth()) selectId = (int) i + 1;
    }
    if (selectId == 0 && ! bitModel.empty()) {
        // Persisted depth not offered by this device: default to 24-bit when available
        // (ample for measurement), else fall back to the first supported depth.
        int defaultIdx = 0;
        for (size_t i = 0; i < bitModel.size(); ++i) if (bitModel[i] == 24) { defaultIdx = (int) i; break; }
        selectId = defaultIdx + 1;
        settings.setOutputBitDepth (bitModel[(size_t) defaultIdx]);
    }
    if (selectId != 0) bitBox.setSelectedId (selectId, juce::dontSendNotification);
    engine.setOutputBitDepth (settings.outputBitDepth());
}

void MainComponent::onRateChosen() {
    const int idx = rateBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) rateModel.size()) return;
    const double sr = rateModel[(size_t) idx].rate;
    settings.setSampleRate (sr);
    engine.setSampleRate (sr);
    rateWarn.setText (rateModel[(size_t) idx].resampleWarning
                          ? "This rate is not native to the selected input - it will be resampled."
                          : juce::String(),
                      juce::dontSendNotification);
    rebuildFirsAsync();   // taps scale with rate
    updateStatusLine();
}

void MainComponent::onBitDepthChosen() {
    const int idx = bitBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) bitModel.size()) return;
    settings.setOutputBitDepth (bitModel[(size_t) idx]);
    engine.setOutputBitDepth (bitModel[(size_t) idx]);
}

void MainComponent::onCombineChosen() {
    const int idx = combineBox.getSelectedId() - 1;
    if (idx < 0 || idx >= (int) combineModel.size()) return;
    const auto mode = combineModel[(size_t) idx].mode;
    settings.setCombineMode (mode);
    engine.setCombineMode (mode);
}

void MainComponent::onLeftCalLoaded (const juce::File& f) {
    settings.setLeftCalPath (f.getFullPathName());
    rebuildFirsAsync();
}
void MainComponent::onRightCalLoaded (const juce::File& f) {
    settings.setRightCalPath (f.getFullPathName());
    rebuildFirsAsync();
}

void MainComponent::rebuildFirsAsync() {
    // Snapshot the inputs needed to design the FIRs, then run the (heavy) design off
    // the message thread and hand the result to the engine via setLeft/RightCalFir
    // (hot-swappable). A single-thread pool serialises rebuilds so a rapid rate change
    // can't race two designs onto the engine out of order.
    const double sr = activeRate();
    const int taps = (settings.firLength() > 0) ? settings.firLength() : numTapsForRate (sr);
    const auto mode = settings.complexPhase() ? FirMode::ComplexWithPhase
                                              : FirMode::MinPhaseMagnitude;
    auto left  = leftCal.calFile();
    auto right = rightCal.calFile();

    firPool->removeAllJobs (false, 0);   // drop a stale pending rebuild; let a running one finish
    firPool->addJob ([this, sr, taps, mode, left, right] {
        FirDesignParams p;
        p.sampleRate = sr;
        p.numTaps    = taps;
        p.mode       = mode;
        p.invert     = true;
        if (left) {
            auto ir = FirDesigner::design (*left, p);
            juce::MessageManager::callAsync ([this, ir = std::move (ir)]() mutable {
                engine.setLeftCalFir (std::move (ir));
            });
        }
        if (right) {
            auto ir = FirDesigner::design (*right, p);
            juce::MessageManager::callAsync ([this, ir = std::move (ir)]() mutable {
                engine.setRightCalFir (std::move (ir));
            });
        }
    });
}

void MainComponent::onStartStop() {
    if (engine.status() == EngineStatus::Running) {
        engine.stop();
        startStop.setButtonText ("Start");
    } else {
        juce::String err;
        if (engine.start (err)) {
            startStop.setButtonText ("Stop");
        } else {
            preflightLabel.setText ("Start failed: " + err, juce::dontSendNotification);
        }
    }
    updateStatusLine();
}

void MainComponent::updateStatusLine() {
    switch (engine.status()) {
        case EngineStatus::Running:
            statusLine.setText ("Running  -  " + juce::String (activeRate() / 1000.0, 1)
                                + " kHz  -  " + juce::String (settings.outputBitDepth()) + "-bit",
                                juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::ok());
            break;
        case EngineStatus::Error:
            statusLine.setText ("Error", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::danger());
            break;
        default:
            statusLine.setText ("Stopped", juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::textDim());
            break;
    }
}

void MainComponent::timerCallback() {
    const auto lv = engine.levels();
    meterL.setLevel  (lv.inL,    lv.clipL);
    meterR.setLevel  (lv.inR,    lv.clipR);
    meterOut.setLevel (lv.outMono, lv.clipOut);

    const auto h = engine.health();
    cleanLight.setText (h.cleanCapture ? "clean" : "DROPOUTS", juce::dontSendNotification);
    cleanLight.setColour (juce::Label::textColourId, h.cleanCapture ? Theme::ok() : Theme::danger());

    if (engine.status() == EngineStatus::Running) {
        statusLine.setText ("Running  -  " + juce::String (activeRate() / 1000.0, 1)
                            + " kHz  -  " + juce::String (settings.outputBitDepth())
                            + "-bit  -  xruns " + juce::String (h.xruns),
                            juce::dontSendNotification);
    }
}

void MainComponent::paint (juce::Graphics& g) { g.fillAll (Theme::bg()); }

void MainComponent::resized() {
    auto r = getLocalBounds().reduced (16);

    // Row 1: input picker.
    inputPicker.setBounds (r.removeFromTop (48));
    r.removeFromTop (10);

    // Row 2: two cal slots side by side.
    auto calRow = r.removeFromTop (170);
    auto leftHalf = calRow.removeFromLeft (calRow.getWidth() / 2 - 6);
    calRow.removeFromLeft (12);
    leftCal.setBounds  (leftHalf);
    rightCal.setBounds (calRow);
    r.removeFromTop (10);

    // Row 3: combine selector + hint.
    combineLabel.setBounds (r.removeFromTop (16));
    combineBox.setBounds (r.removeFromTop (28));
    combineHint.setBounds (r.removeFromTop (28));
    r.removeFromTop (10);

    // Row 4: output picker + hint + preflight.
    outputPicker.setBounds (r.removeFromTop (48));
    outputHint.setBounds (r.removeFromTop (16));
    preflightLabel.setBounds (r.removeFromTop (16));
    r.removeFromTop (10);

    // Row 5: rate + bit depth side by side.
    auto rbRow = r.removeFromTop (60);
    auto rateCol = rbRow.removeFromLeft (rbRow.getWidth() / 2 - 6);
    rbRow.removeFromLeft (12);
    rateLabel.setBounds (rateCol.removeFromTop (16));
    rateBox.setBounds (rateCol.removeFromTop (28));
    rateWarn.setBounds (rateCol.removeFromTop (16));
    bitLabel.setBounds (rbRow.removeFromTop (16));
    bitBox.setBounds (rbRow.removeFromTop (28));
    r.removeFromTop (10);

    // Row 6: meters (fixed-width vertical bars) + clean light.
    auto meterRow = r.removeFromTop (120);
    auto mL = meterRow.removeFromLeft (60); meterRow.removeFromLeft (8);
    auto mR = meterRow.removeFromLeft (60); meterRow.removeFromLeft (8);
    auto mO = meterRow.removeFromLeft (60);
    meterL.setBounds   (mL);
    meterR.setBounds   (mR);
    meterOut.setBounds (mO);
    cleanLight.setBounds (meterRow.removeFromTop (20).reduced (8, 0));
    r.removeFromTop (10);

    // Row 7: transport.
    auto transport = r.removeFromTop (36);
    startStop.setBounds (transport.removeFromLeft (120));
    transport.removeFromLeft (12);
    statusLine.setBounds (transport);
    r.removeFromTop (8);

    // Row 8: advanced disclosure.
    advancedToggle.setBounds (r.removeFromTop (24));
    const bool adv = advancedToggle.getToggleState();
    complexPhaseToggle.setVisible (adv);
    firLenLabel.setVisible (adv); firLenBox.setVisible (adv);
    trimLabel.setVisible (adv);   trimSlider.setVisible (adv);
    if (adv) {
        complexPhaseToggle.setBounds (r.removeFromTop (24));
        firLenLabel.setBounds (r.removeFromTop (16));
        firLenBox.setBounds (r.removeFromTop (26).removeFromLeft (140));
        trimLabel.setBounds (r.removeFromTop (16));
        trimSlider.setBounds (r.removeFromTop (28));
    }
}

} // namespace eb
```

> **SPINE include:** `audio/AudioEngine.h` is owned by Plan 2 and declares the `eb::AudioEngine` class with EXACTLY the public methods in the SPINE recap above. This file only calls those methods. The `firPool->removeAllJobs(false,0)` + single-thread pool pattern guarantees no allocation/heavy work on the message thread blocks the UI, and `MessageManager::callAsync` marshals the finished FIR back to the message thread before `setLeftCalFir/setRightCalFir` (which the engine hot-swaps lock-free into `ProcessingGraph::setFir`).

- [ ] **Build the lib to confirm it compiles** (requires Plan 2's `AudioEngine.h` / `DeviceId.h` / `EngineTypes.h` present — real or shim):

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target eb_gui"
```
Expected: `eb_gui` builds clean.

- [ ] **MANUAL VERIFICATION (deferred to Task 9 — needs the running app):** layout/behaviour checks are consolidated in Task 9's block (they exercise the full window).

- [ ] **Commit.**

```
git add src/gui/MainComponent.h src/gui/MainComponent.cpp CMakeLists.txt
git commit -m "Plan 3 Task 8: MainComponent layout + AudioEngine wiring"
```

---

## Task 9 — Host MainComponent in the app + end-to-end manual pass

**Files**

- Modify: `src/Main.cpp`
- Modify: `CMakeLists.txt` (link `eb_gui` into the `EarsBridge` app target)

### Steps

- [ ] **Link `eb_gui` into the app.** In `CMakeLists.txt`, change the `EarsBridge` target's `target_link_libraries` to add `eb_gui` (it already links `eb_core` transitively via `eb_gui`, but keep both explicit for clarity):

```cmake
target_link_libraries(EarsBridge PRIVATE
    eb_gui eb_core juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
```

- [ ] **Replace `src/Main.cpp` to host `eb::MainComponent`:**

```cpp
#include <juce_gui_extra/juce_gui_extra.h>
#include "gui/MainComponent.h"

class EarsBridgeApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return "EARS Bridge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override {
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override { window = nullptr; }

private:
    class MainWindow : public juce::DocumentWindow {
    public:
        MainWindow()
            : juce::DocumentWindow ("EARS Bridge",
                                    juce::Colour (0xff141619),
                                    juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar (true);
            setContentOwned (new eb::MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (EarsBridgeApp)
```

- [ ] **Build the full app.**

```
cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"
cmd /c "tools\dev.cmd cmake --build build --target EarsBridge"
```
Expected: `EarsBridge` (and `eb_gui`, `eb_core`) build with no errors; the app binary appears under `build/`.

- [ ] **Run the full headless test suite to confirm nothing regressed.**

```
cmd /c "tools\dev.cmd cmake --build build --target eb_tests"
cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure"
```
Expected: all Plan-1 tests + the new `Settings`, `PlotMath`, `RateMenu` tests pass (0 failures).

- [ ] **MANUAL VERIFICATION — full window (launch `build/EarsBridge_artefacts/Release/EARS Bridge.exe`, or the `.exe` under `build/`):**
  1. **Theme (Task 4):** window background near-black charcoal; all panels are dark cards; text light grey; Start button + ComboBox arrows blue. PASS = no default-grey JUCE theme anywhere.
  2. **Device pickers (Task 7):** input picker lists the connected EARS/EARS Pro with the correct `[EARS]` / `[EARS Pro]` tag; output picker tags virtual sinks `(virtual)`. PASS = correct model + virtual tags.
  3. **Rate menu (Task 8):** with the original EARS selected, the rate menu shows only `48.0 kHz`; selecting any other rate appends a `(resample)` entry and the yellow resample warning shows. With EARS Pro selected, the menu shows `44.1 / 48.0 / 88.2 / 96.0 / 176.4 / 192.0 kHz`, all clean. PASS = menu reflects the selected device's native rates per §9.
  4. **Bit depth (Task 8):** with EARS Pro the bit-depth menu shows `16-bit`, `24-bit`, and `32-bit` (24-bit default and selected); with the original EARS it shows `24-bit` only. Switching it updates the status line's `-bit` segment. (Output depth is a best-effort request on the render format, not enforced — see Plan 2; 32-bit only matters where the sink, e.g. BlackHole, accepts it, with no measurement benefit beyond 24-bit.) PASS = 16/24/32 selectable on EARS Pro with 24 the default, persisted.
  5. **Cal slots (Task 6):** drag `R_HPN_8604350.txt` onto each slot; detail reads `Serial 860-4350  -  HPN`; thumbnail draws the curve with the 4 kHz feature; no badge. Drag an HEQ file → orange `HEQ - double-target?` badge appears. PASS as Task 6 block.
  6. **Combine selector (Task 8):** four entries in rigor order, the two two-pass entries carry `[Recommended]`, `Sum L+R` carries `(+6 dB clip risk)`; the method hint line is visible. PASS = ordering + badges + hint present.
  7. **Meters + transport (Tasks 5, 8):** click **Start**; with the EARS connected and a signal present, L/R input meters and the OUT meter move; the status line reads `Running - <rate> kHz - <bits>-bit - xruns 0`; the `clean` light is green. Pull the EARS or force an xrun → the clean light turns red `DROPOUTS`. Click **Stop** → status returns to `Stopped`. PASS = transport toggles, meters track, telemetry updates at ~30 Hz.
  8. **Advanced disclosure (Task 8):** tick **Advanced** → Complex-phase toggle, FIR-length combo (Auto / 4096 / 8192 / 16384 / 32768; default **Auto** = scales with rate via `numTapsForRate`), and Output-trim slider appear. With **Auto** selected, the designed tap count tracks the sample rate (8192 @ 48 k → 16384 @ 96 k → 32768 @ 192 k); picking an explicit value overrides it. Changing FIR length or complex-phase triggers a (silent, off-thread) FIR rebuild without freezing the UI; the trim slider moves smoothly. PASS = Auto is the default and scales with rate; explicit overrides apply; disclosure reveals controls.
  9. **Persistence (Task 1):** set a non-default rate, bit depth, combine mode, and cal files; quit; relaunch. PASS = every selection (device keys, rate, bit depth, combine, cal paths, trim, FIR length) is restored.

- [ ] **Commit.**

```
git add src/Main.cpp CMakeLists.txt
git commit -m "Plan 3 Task 9: host MainComponent in the EarsBridge app"
```

---

## Done criteria

- [ ] `cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure"` passes with the three new headless suites green: `Settings` (round-trip incl. model/rate/bit-depth + defaults), `PlotMath` (freq→x log mapping + dB→y at known points + clamps), `RateMenu` (rate-menu-from-`supportedSampleRates`, combine-mode ordering/recommended, `numTapsForRate`), plus all Plan-1 suites still green.
- [ ] `EarsBridge` app target builds on Windows (Ninja+MSVC via `tools\dev.cmd`) and hosts `eb::MainComponent`.
- [ ] All nine MANUAL VERIFICATION blocks pass against a connected EARS or EARS Pro: dark theme, model-tagged pickers, device-native rate menu with resample warning, 16/24/32-bit output selector (24-bit default; full set on EARS Pro, 24-only on EARS), drag-drop cal slots with thumbnail + HEQ badge, rigor-ordered combine selector with Recommended badge, live meters + clip LEDs + clean-capture light, working Start/Stop + status, advanced disclosure (FIR length defaulting to Auto/rate-scaled), and full settings persistence across relaunch.
- [ ] No new shared types invented: the GUI talks to the engine ONLY through the SPINE `eb::AudioEngine` API and consumes `eb::DeviceId`/`eb::EarsModel`/`eb::Levels`/`eb::Health`/`eb::EngineStatus` (Plan 2) and `eb::CalFile`/`eb::FirDesigner`/`eb::CombineMode` (Plan 1) verbatim.
- [ ] FIRs are (re)designed off the message thread at the active rate (`FirDesignParams.sampleRate = active rate`; taps default to **Auto** = `numTapsForRate(rate)` so they scale with the rate — 8192 @ 48 k, 16384 @ 96 k, 32768 @ 192 k — and only a non-Auto advanced selection overrides this) and handed to the engine via `setLeftCalFir`/`setRightCalFir`; no allocation/heavy work on the audio or message thread blocks the UI.
