# EARS Bridge — Plan 1: Scaffold + Calibration DSP Core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the JUCE/CMake project and build a fully unit-tested calibration DSP core — parse miniDSP EARS cal files, design inverse-cal minimum-phase FIRs, and apply per-channel correction + mono combine — with no audio-device or GUI dependencies.

**Architecture:** A header-light C++ core in `src/cal` and `src/audio` compiled into a static library `eb_core`, linked by both a (placeholder) GUI app and a Catch2 test runner. Pure, deterministic, headlessly testable: `CalFile` (FRD parsing), `FirDesigner` (real-cepstrum min-phase from the inverted cal curve), `ProcessingGraph` (two `juce::dsp::Convolution` + combine). Device I/O, the clock bridge, and the GUI are later plans.

**Tech Stack:** C++20, JUCE ≥ 7 (via CMake `FetchContent`), CMake ≥ 3.22, Catch2 v3 (via `FetchContent`), `ctest`. Modules used: `juce_core`, `juce_audio_basics`, `juce_dsp`, `juce_gui_extra` (app shell only).

**Plan series (context):** Plan 1 = scaffold + DSP core (this doc). Plan 2 = audio engine (device layer, two-clock FIFO+ASRC bridge, passthrough). Plan 3 = GUI. Plan 4 = health monitoring, settings, macOS aggregate device, bench-validation harness.

**Spec:** `docs/superpowers/specs/2026-06-13-ears-bridge-design.md` (§5.2, §5.3, §7 implemented here).

---

## Build environment

This machine has **CMake 3.30**, **Git**, and **Visual Studio Build Tools 2026 (MSVC v18, cl 19.51)**, but **no Ninja on PATH**, and **CMake 3.30 ships no generator for VS 2026**. So the build uses the **Ninja generator + MSVC**, entered through a committed helper that loads `vcvars64.bat` and puts the VS-bundled Ninja on PATH.

- Helper: `tools/dev.ps1` (already in the repo) runs any command inside the MSVC x64 dev environment.
- **On Windows, prefix every `cmake` and `ctest` command in this plan** with `powershell -ExecutionPolicy Bypass -File tools/dev.ps1`, and pass **`-G Ninja`** on the first configure:
  - Configure: `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release`
  - Build:     `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake --build build --target eb_tests`
  - Test:      `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ctest --test-dir build --output-on-failure`
- **On macOS** (later plans), run the same `cmake`/`ctest` commands directly — no wrapper needed.

Verified working: `tools/dev.ps1` exposes `cl 19.51`, `ninja 1.13.2`, `cmake 3.30`.

---

## File structure (created by this plan)

```
EARS_program/
  CMakeLists.txt                       # top-level: JUCE+Catch2 fetch, app + lib + tests
  .gitignore
  tools/dev.ps1                        # Windows MSVC+Ninja dev-shell wrapper (committed)
  src/
    Main.cpp                           # placeholder GUI app entry (empty window)
    cal/
      CalFile.h        CalFile.cpp      # FRD parse, type tag, serial, validation
      FirDesigner.h    FirDesigner.cpp  # inverse-cal min-phase / complex FIR design
    audio/
      CombineMode.h                     # enum CombineMode
      ProcessingGraph.h ProcessingGraph.cpp  # per-channel convolution + combine
  tests/
    CMakeLists.txt                      # Catch2 runner target
    test_calfile.cpp
    test_firdesigner.cpp
    test_processinggraph.cpp
    data/                               # cal-file fixtures (copied from the user's files)
      L_HPN_0000000.txt  R_HPN_0000000.txt
      L_HEQ_0000000.txt  R_HEQ_0000000.txt
```

All core code lives in `namespace eb`.

### Shared contract (types every task must use verbatim)

```cpp
// src/cal/CalFile.h
namespace eb {
enum class CalType { Unknown, Raw, Hpn, Heq, Idf };
struct CalPoint { double freqHz; double splDb; double phaseDeg; };
struct CalParseError : std::runtime_error { using std::runtime_error::runtime_error; };

struct CalFile {
    juce::String serial;            // e.g. "000-0000"
    CalType type = CalType::Unknown;
    std::vector<CalPoint> points;   // ascending by freqHz, validated
    static CalFile parse (const juce::String& text);   // throws CalParseError
};
}
```
```cpp
// src/cal/FirDesigner.h
namespace eb {
enum class FirMode { MinPhaseMagnitude, ComplexWithPhase };
struct FirDesignParams {
    double sampleRate = 48000.0;
    int    numTaps    = 8192;       // power of two; FIR length returned
    int    designFftOrder = 16;     // design FFT = 2^16 = 65536 (>= 4..8x numTaps)
    FirMode mode      = FirMode::MinPhaseMagnitude;
    bool   invert     = true;       // pre-filter = inverse (-dB) of the cal curve
    double maxBoostDb = 12.0;       // clamp boosts (where inverted target would boost)
    double bandLowHz  = 10.0;
    double bandHighHz = 20000.0;    // outside band: hold endpoint level
};
class FirDesigner {
public:
    static juce::AudioBuffer<float> design (const CalFile& cal, const FirDesignParams& p);
};
}
```
```cpp
// src/audio/CombineMode.h
namespace eb { enum class CombineMode { TwoPassLeft, TwoPassRight, Average, Sum }; }
```
```cpp
// src/audio/ProcessingGraph.h
namespace eb {
class ProcessingGraph {
public:
    void prepare (double sampleRate, int maxBlockSize);
    void setFir  (int channel /*0=L,1=R*/, juce::AudioBuffer<float> ir); // hot-swap
    void setCombineMode (CombineMode mode);
    // inL/inR: raw EARS channels; outMono: combined calibrated mono. All length numSamples.
    void process (const float* inL, const float* inR, float* outMono, int numSamples);
    void reset();
private:
    juce::dsp::Convolution convL, convR;
    std::atomic<int> combine { (int) CombineMode::TwoPassLeft };
    juce::AudioBuffer<float> scratch; // [2][maxBlock]
    double sr = 48000.0; int maxBlock = 0;
};
}
```

> Tolerances used in tests: FIR magnitude match within **±1.0 dB** at in-band cal frequencies (≥ 40 Hz, ≤ 18 kHz); combine levels exact to **1e-5**.

---

## Task 1: Project scaffold (JUCE + Catch2, building app shell + empty test runner)

**Files:**
- Create: `CMakeLists.txt`, `.gitignore`, `src/Main.cpp`, `tests/CMakeLists.txt`, `tests/test_smoke.cpp`

- [ ] **Step 1: Write `.gitignore`**

Create `.gitignore`:
```
/build/
/Builds/
/.cache/
*.user
.DS_Store
```

- [ ] **Step 2: Write top-level `CMakeLists.txt`**

Create `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.22)
project(EarsBridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4)        # JUCE >= 7 required (Convolution realtime-safe)
FetchContent_MakeAvailable(juce)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0)
FetchContent_MakeAvailable(Catch2)
# Catch2's CMake helpers (Catch.cmake -> catch_discover_tests) are NOT registered by
# FetchContent; add its extras dir to the module path. Note the lowercase catch2_SOURCE_DIR
# (CMake lowercases the declared name). Without this, include(Catch) fails at configure time.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

# ---- Core DSP library (no devices, no GUI) ----
add_library(eb_core STATIC
    src/cal/CalFile.cpp
    src/cal/FirDesigner.cpp
    src/audio/ProcessingGraph.cpp)
target_include_directories(eb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eb_core PUBLIC cxx_std_20)
# Modules whose types appear in eb_core's PUBLIC headers (FirDesigner.h / ProcessingGraph.h
# include <juce_dsp/..> and <juce_audio_basics/..>) MUST be PUBLIC so their include dirs/defines
# reach consumers (eb_tests); juce_core is implementation-only -> PRIVATE (avoids recompiling
# its sources into every consumer). Link the recommended config/warning flags PUBLIC — every
# official JUCE example links them to the module-consuming target.
target_link_libraries(eb_core
    PUBLIC
        juce::juce_audio_basics juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    PRIVATE
        juce::juce_core)
target_compile_definitions(eb_core PUBLIC
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# ---- Placeholder GUI app (proves JUCE links + builds on both OSes) ----
juce_add_gui_app(EarsBridge PRODUCT_NAME "EARS Bridge")
target_sources(EarsBridge PRIVATE src/Main.cpp)
# Set these on the app directly: juce_gui_extra (curl / WebBrowserComponent) links to the app,
# not through eb_core, so don't rely on transitive defines reaching it.
target_compile_definitions(EarsBridge PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(EarsBridge PRIVATE
    eb_core juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 3: Write `src/Main.cpp` (minimal JUCE app, empty window)**

Create `src/Main.cpp`:
```cpp
#include <juce_gui_extra/juce_gui_extra.h>

class EarsBridgeApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return "EARS Bridge"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String&) override {
        window = std::make_unique<juce::DocumentWindow>(
            "EARS Bridge", juce::Colours::black,
            juce::DocumentWindow::allButtons);
        window->setContentOwned (new juce::Component(), true);
        window->centreWithSize (640, 400);
        window->setVisible (true);
    }
    void shutdown() override { window = nullptr; }

private:
    std::unique_ptr<juce::DocumentWindow> window;
};

START_JUCE_APPLICATION (EarsBridgeApp)
```

- [ ] **Step 4: Write `tests/CMakeLists.txt`**

Create `tests/CMakeLists.txt`:
```cmake
add_executable(eb_tests
    test_smoke.cpp)
# Link the JUCE modules the test sources include directly (test_firdesigner.cpp / the
# integration test #include <juce_dsp/..> and the eb_core headers that pull in juce_audio_basics),
# plus the recommended config flags, so eb_tests compiles regardless of eb_core's link visibility.
target_link_libraries(eb_tests PRIVATE
    eb_core
    juce::juce_audio_basics juce::juce_dsp
    juce::juce_recommended_config_flags
    Catch2::Catch2WithMain)
target_compile_definitions(eb_tests PRIVATE
    EB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

include(Catch)
catch_discover_tests(eb_tests)
```

- [ ] **Step 5: Write a smoke test**

Create `tests/test_smoke.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("smoke: test runner links and runs") {
    REQUIRE(1 + 1 == 2);
}
```

- [ ] **Step 6: Configure and build** (Windows commands shown; see Build environment)

Run: `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release`
Expected: configures, fetches JUCE 8.0.4 + Catch2 v3.6.0 (first run is slow), ends with "Generating done".

Run: `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake --build build --target eb_tests`
Expected: builds `eb_tests` with no errors.

> Note: `eb_core/CMakeLists.txt` lists `CalFile.cpp`, `FirDesigner.cpp`, `ProcessingGraph.cpp` which don't exist yet. Create empty stubs so Task 1 builds: add the next step.

- [ ] **Step 7: Add empty source stubs so the library compiles**

Create `src/cal/CalFile.cpp`, `src/cal/FirDesigner.cpp`, `src/audio/ProcessingGraph.cpp`, each containing only:
```cpp
// placeholder; implemented in later tasks
```
Create matching headers `src/cal/CalFile.h`, `src/cal/FirDesigner.h`, `src/audio/CombineMode.h`, `src/audio/ProcessingGraph.h` with the **exact** declarations from the "Shared contract" section above, each wrapped in `#pragma once` and including `<juce_audio_basics/juce_audio_basics.h>` (CalFile/ProcessingGraph) / `<juce_dsp/juce_dsp.h>` (FirDesigner/ProcessingGraph) and `<vector>`, `<stdexcept>`, `<atomic>` as needed.

- [ ] **Step 8: Re-build and run the smoke test**

Run: `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release`
then: `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 cmake --build build --target eb_tests`
Expected: builds cleanly.

Run: `powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ctest --test-dir build --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 9: Copy the cal-file fixtures into the repo**

Copy the four files from `C:\Users\Shaya\OneDrive\Documents\` into `tests/data/`:
`L_HPN_0000000.txt`, `R_HPN_0000000.txt`, `L_HEQ_0000000.txt`, `R_HEQ_0000000.txt`.

- [ ] **Step 10: Commit**

```bash
git add CMakeLists.txt .gitignore src/ tests/
git commit -m "feat: scaffold JUCE+Catch2 project with eb_core lib and app shell"
```

---

## Task 2: `CalFile` — parse FRD data rows

**Files:**
- Modify: `src/cal/CalFile.cpp`
- Test: `tests/test_calfile.cpp` (Create), `tests/CMakeLists.txt` (Modify: add source)

- [ ] **Step 1: Register the new test file**

In `tests/CMakeLists.txt`, change the `add_executable` to:
```cmake
add_executable(eb_tests
    test_smoke.cpp
    test_calfile.cpp)
```

- [ ] **Step 2: Write the failing test (data-row parsing)**

Create `tests/test_calfile.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cal/CalFile.h"

using Catch::Matchers::WithinAbs;

static juce::String sampleFrd() {
    return
        "\"Sens Factor =0.9dB, EARS Serial 000-0000, compensation HPN V1\"\n"
        "\"Use this file on the LEFT channel. Your sensitive side is RIGHT.\"\n"
        "* HPN: Default headphone compensation curve for miniDSP EARS\n"
        "* Freq(Hz) SPL(dB) Phase(degrees)\n"
        "    10.0     -6.4      3.8\n"
        "    20.0     -2.8      2.2\n"
        "  1000.0     -2.1     35.6\n"
        " 20000.0     -2.3      0.6\n";
}

TEST_CASE("CalFile parses numeric data rows") {
    auto cal = eb::CalFile::parse (sampleFrd());
    REQUIRE(cal.points.size() == 4);
    CHECK_THAT(cal.points.front().freqHz, WithinAbs(10.0, 1e-9));
    CHECK_THAT(cal.points.front().splDb,  WithinAbs(-6.4, 1e-9));
    CHECK_THAT(cal.points.front().phaseDeg, WithinAbs(3.8, 1e-9));
    CHECK_THAT(cal.points.back().freqHz,  WithinAbs(20000.0, 1e-9));
    CHECK_THAT(cal.points[2].splDb,       WithinAbs(-2.1, 1e-9));
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R CalFile --output-on-failure`
Expected: FAIL — link error / empty `parse` returns no points (currently a stub).

- [ ] **Step 4: Implement `CalFile::parse` (data rows only)**

Replace `src/cal/CalFile.cpp` with:
```cpp
#include "cal/CalFile.h"

namespace eb {

CalFile CalFile::parse (const juce::String& text) {
    CalFile out;
    auto lines = juce::StringArray::fromLines (text);
    for (auto raw : lines) {
        auto line = raw.trim();
        if (line.isEmpty()) continue;
        if (line.startsWith ("*")) continue;          // comment
        if (line.startsWithChar ('"')) continue;       // header strings (handled in Task 3)

        auto toks = juce::StringArray::fromTokens (line, " \t", "");
        toks.removeEmptyStrings();
        if (toks.size() < 2) continue;                 // not a data row
        if (! toks[0].containsOnly ("0123456789.+-eE")) continue;

        CalPoint p;
        p.freqHz   = toks[0].getDoubleValue();
        p.splDb    = toks[1].getDoubleValue();
        p.phaseDeg = toks.size() >= 3 ? toks[2].getDoubleValue() : 0.0;
        out.points.push_back (p);
    }
    if (out.points.empty())
        throw CalParseError ("No calibration data rows found");
    return out;
}

} // namespace eb
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R CalFile --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/cal/CalFile.cpp tests/test_calfile.cpp tests/CMakeLists.txt
git commit -m "feat: parse FRD data rows in CalFile"
```

---

## Task 3: `CalFile` — serial, type tag, and validation

**Files:**
- Modify: `src/cal/CalFile.cpp`
- Test: `tests/test_calfile.cpp`

- [ ] **Step 1: Write the failing tests (serial, type, validation)**

Append to `tests/test_calfile.cpp`:
```cpp
TEST_CASE("CalFile extracts serial and HPN type from header") {
    auto cal = eb::CalFile::parse (sampleFrd());
    CHECK(cal.serial == juce::String("000-0000"));
    CHECK(cal.type == eb::CalType::Hpn);
}

TEST_CASE("CalFile detects HEQ type") {
    juce::String t =
        "\"Sens Factor =0.9dB, EARS Serial 000-0000, compensation HEQ V2\"\n"
        "* Freq(Hz) SPL(dB) Phase(degrees)\n"
        "   100.0   0.4   4.4\n   200.0   0.5   8.1\n";
    auto cal = eb::CalFile::parse (t);
    CHECK(cal.type == eb::CalType::Heq);
}

TEST_CASE("CalFile rejects non-monotonic frequency") {
    juce::String t =
        "* Freq SPL Phase\n   100.0 0.0 0.0\n   50.0 0.0 0.0\n";
    REQUIRE_THROWS_AS (eb::CalFile::parse (t), eb::CalParseError);
}

TEST_CASE("CalFile reads the real R_HPN fixture") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_0000000.txt");
    REQUIRE(f.existsAsFile());
    auto cal = eb::CalFile::parse (f.loadFileAsString());
    CHECK(cal.type == eb::CalType::Hpn);
    CHECK(cal.serial == juce::String("000-0000"));
    REQUIRE(cal.points.size() >= 130);
    CHECK_THAT(cal.points.front().freqHz, Catch::Matchers::WithinAbs(10.0, 1e-9));
    CHECK_THAT(cal.points.back().freqHz, Catch::Matchers::WithinAbs(20000.0, 1e-9));
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R CalFile --output-on-failure`
Expected: FAIL (serial empty, type Unknown, no monotonic check).

- [ ] **Step 3: Implement header parsing + validation**

In `src/cal/CalFile.cpp`, add a header-line handler and a validation pass. Replace the file with:
```cpp
#include "cal/CalFile.h"

namespace eb {

static CalType detectType (const juce::String& upper) {
    if (upper.contains ("HEQ")) return CalType::Heq;
    if (upper.contains ("HPN")) return CalType::Hpn;
    if (upper.contains ("IDF")) return CalType::Idf;
    if (upper.contains ("RAW")) return CalType::Raw;
    return CalType::Unknown;
}

CalFile CalFile::parse (const juce::String& text) {
    CalFile out;
    auto lines = juce::StringArray::fromLines (text);
    for (auto raw : lines) {
        auto line = raw.trim();
        if (line.isEmpty()) continue;

        if (line.startsWithChar ('"') || line.startsWith ("*")) {
            auto upper = line.toUpperCase();
            if (out.type == CalType::Unknown) out.type = detectType (upper);
            auto idx = upper.indexOf ("SERIAL");
            if (out.serial.isEmpty() && idx >= 0) {
                // take the token after "SERIAL", strip trailing punctuation
                auto after = line.substring (idx + 6).trim();
                auto tok = juce::StringArray::fromTokens (after, " ,\"", "")[0];
                out.serial = tok.trim();
            }
            continue;
        }

        auto toks = juce::StringArray::fromTokens (line, " \t", "");
        toks.removeEmptyStrings();
        if (toks.size() < 2) continue;
        if (! toks[0].containsOnly ("0123456789.+-eE")) continue;

        CalPoint p;
        p.freqHz   = toks[0].getDoubleValue();
        p.splDb    = toks[1].getDoubleValue();
        p.phaseDeg = toks.size() >= 3 ? toks[2].getDoubleValue() : 0.0;
        out.points.push_back (p);
    }

    if (out.points.empty())
        throw CalParseError ("No calibration data rows found");

    for (size_t i = 1; i < out.points.size(); ++i)
        if (out.points[i].freqHz <= out.points[i - 1].freqHz)
            throw CalParseError ("Calibration frequencies must increase monotonically");

    return out;
}

} // namespace eb
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R CalFile --output-on-failure`
Expected: PASS (all CalFile tests).

- [ ] **Step 5: Commit**

```bash
git add src/cal/CalFile.cpp tests/test_calfile.cpp
git commit -m "feat: parse serial/type and validate monotonic freq in CalFile"
```

---

## Task 4: `FirDesigner` — inverted, interpolated target magnitude

**Files:**
- Modify: `src/cal/FirDesigner.cpp`
- Test: `tests/test_firdesigner.cpp` (Create), `tests/CMakeLists.txt` (Modify)

This task builds the **target complex spectrum** (linear FFT grid) used by both FIR modes, and verifies the magnitude is the log-interpolated **inverse** of the cal curve with endpoint hold + boost clamp. We expose it via a static helper for testing.

- [ ] **Step 1: Add the helper to the header**

In `src/cal/FirDesigner.h`, inside `class FirDesigner`, add a public static declaration after `design`:
```cpp
    // Linear-frequency target spectrum of size (fftSize/2 + 1), magnitude only
    // (phase 0). Exposed for testing the interpolation/inversion/clamp.
    static std::vector<float> targetMagnitudeLinear (const CalFile& cal,
                                                     const FirDesignParams& p,
                                                     int fftSize);
```

- [ ] **Step 2: Register the test file**

In `tests/CMakeLists.txt`, add `test_firdesigner.cpp` to `add_executable(eb_tests ...)`.

- [ ] **Step 3: Write the failing test**

Create `tests/test_firdesigner.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"

using Catch::Matchers::WithinAbs;

// cal curve: flat 0 dB at 1k, +20 dB at 4k -> inverted target = 0 dB, -20 dB
static eb::CalFile twoPointCal() {
    eb::CalFile c;
    c.points = { {20.0, 0.0, 0.0}, {1000.0, 0.0, 0.0},
                 {4000.0, 20.0, 0.0}, {20000.0, 0.0, 0.0} };
    return c;
}

static double binToHz (int bin, int fftSize, double sr) {
    return (double) bin * sr / (double) fftSize;
}
static int hzToBin (double hz, int fftSize, double sr) {
    return (int) std::lround (hz * (double) fftSize / sr);
}

TEST_CASE("FirDesigner target magnitude is the inverted, interpolated cal curve") {
    eb::FirDesignParams p; p.sampleRate = 48000.0; p.invert = true; p.maxBoostDb = 12.0;
    const int fftSize = 65536;
    auto mag = eb::FirDesigner::targetMagnitudeLinear (twoPointCal(), p, fftSize);
    REQUIRE((int) mag.size() == fftSize/2 + 1);

    auto dbAt = [&](double hz) {
        int bin = hzToBin (hz, fftSize, p.sampleRate);
        return 20.0 * std::log10 (std::max (1e-9f, mag[(size_t) bin]));
    };
    CHECK_THAT(dbAt(1000.0), WithinAbs(0.0, 0.5));     // inverse of 0 dB
    CHECK_THAT(dbAt(4000.0), WithinAbs(-20.0, 0.5));   // inverse of +20 dB
}

TEST_CASE("FirDesigner clamps excessive boost") {
    // cal -30 dB at 100 Hz -> inverted +30 dB, clamped to +12 dB
    eb::CalFile c; c.points = { {20.0,-30.0,0.0}, {100.0,-30.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.invert = true; p.maxBoostDb = 12.0;
    const int fftSize = 65536;
    auto mag = eb::FirDesigner::targetMagnitudeLinear (c, p, fftSize);
    int bin = hzToBin (100.0, fftSize, p.sampleRate);
    double db = 20.0 * std::log10 (std::max (1e-9f, mag[(size_t) bin]));
    CHECK(db <= 12.0 + 0.5);
    CHECK(db >= 12.0 - 0.5);
}
```

- [ ] **Step 4: Run to verify failure**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R FirDesigner --output-on-failure`
Expected: FAIL (helper unimplemented / link error).

- [ ] **Step 5: Implement `targetMagnitudeLinear`**

Replace `src/cal/FirDesigner.cpp` with:
```cpp
#include "cal/FirDesigner.h"
#include <cmath>
#include <algorithm>

namespace eb {

// log-frequency linear interpolation of the cal magnitude (dB) at an arbitrary Hz,
// holding endpoint values outside [first,last].
static double interpCalDb (const std::vector<CalPoint>& pts, double hz) {
    if (hz <= pts.front().freqHz) return pts.front().splDb;
    if (hz >= pts.back().freqHz)  return pts.back().splDb;
    // binary search for the bracketing pair
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (pts[mid].freqHz <= hz) lo = mid; else hi = mid;
    }
    const double lf = std::log10 (pts[lo].freqHz), hf = std::log10 (pts[hi].freqHz);
    const double t  = (std::log10 (hz) - lf) / (hf - lf);
    return pts[lo].splDb + t * (pts[hi].splDb - pts[lo].splDb);
}

std::vector<float> FirDesigner::targetMagnitudeLinear (const CalFile& cal,
                                                       const FirDesignParams& p,
                                                       int fftSize) {
    const int nBins = fftSize / 2 + 1;
    std::vector<float> mag ((size_t) nBins, 1.0f);
    for (int k = 0; k < nBins; ++k) {
        double hz = (double) k * p.sampleRate / (double) fftSize;
        if (hz < 1.0) hz = 1.0; // avoid log(0) at DC; endpoint-hold covers it
        double db = interpCalDb (cal.points, hz);
        if (p.invert) db = -db;
        if (db > p.maxBoostDb) db = p.maxBoostDb;   // clamp boosts
        mag[(size_t) k] = (float) std::pow (10.0, db / 20.0);
    }
    return mag;
}

juce::AudioBuffer<float> FirDesigner::design (const CalFile&, const FirDesignParams& p) {
    // implemented in Task 5/6
    return juce::AudioBuffer<float> (1, p.numTaps);
}

} // namespace eb
```

- [ ] **Step 6: Run to verify pass**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R FirDesigner --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/cal/FirDesigner.h src/cal/FirDesigner.cpp tests/test_firdesigner.cpp tests/CMakeLists.txt
git commit -m "feat: inverted log-interpolated target magnitude with boost clamp"
```

---

## Task 5: `FirDesigner::design` — minimum-phase FIR via real cepstrum

**Files:**
- Modify: `src/cal/FirDesigner.cpp`
- Test: `tests/test_firdesigner.cpp`

- [ ] **Step 1: Write the failing test (designed FIR magnitude matches inverted cal)**

Append to `tests/test_firdesigner.cpp`:
```cpp
#include <juce_dsp/juce_dsp.h>

// Measure the magnitude (dB) of an impulse response at a given frequency,
// via zero-padded FFT.
static double irMagnitudeDb (const juce::AudioBuffer<float>& ir, double hz, double sr) {
    const int order = 16, fftSize = 1 << order; // 65536
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
    const int n = juce::jmin (ir.getNumSamples(), fftSize);
    for (int i = 0; i < n; ++i) buf[(size_t) i] = ir.getSample (0, i);
    fft.performRealOnlyForwardTransform (buf.data());
    int bin = (int) std::lround (hz * (double) fftSize / sr);
    float re = buf[(size_t) bin * 2], im = buf[(size_t) bin * 2 + 1];
    return 20.0 * std::log10 (std::max (1e-9f, std::sqrt (re*re + im*im)));
}

TEST_CASE("Min-phase FIR magnitude matches the inverted cal within 1 dB") {
    eb::CalFile c; c.points = { {20.0,0.0,0.0}, {1000.0,0.0,0.0},
                                {4000.0,20.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.sampleRate = 48000.0; p.numTaps = 8192;
    p.mode = eb::FirMode::MinPhaseMagnitude; p.invert = true; p.maxBoostDb = 12.0;
    auto ir = eb::FirDesigner::design (c, p);
    REQUIRE(ir.getNumSamples() == 8192);
    CHECK_THAT(irMagnitudeDb (ir, 1000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(0.0, 1.0));
    CHECK_THAT(irMagnitudeDb (ir, 4000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(-20.0, 1.0));
}

TEST_CASE("Min-phase FIR is causal (energy concentrated near the front)") {
    eb::CalFile c; c.points = { {20.0,0.0,0.0}, {4000.0,20.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.numTaps = 8192; p.mode = eb::FirMode::MinPhaseMagnitude;
    auto ir = eb::FirDesigner::design (c, p);
    double total = 0, firstQuarter = 0;
    for (int i = 0; i < ir.getNumSamples(); ++i) {
        double e = (double) ir.getSample(0,i) * ir.getSample(0,i);
        total += e; if (i < ir.getNumSamples()/4) firstQuarter += e;
    }
    CHECK(firstQuarter / total > 0.9); // min-phase: front-loaded
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R "Min-phase" --output-on-failure`
Expected: FAIL (design returns zeros).

- [ ] **Step 3: Implement `design` for the min-phase path**

In `src/cal/FirDesigner.cpp`, replace the `design` stub with the real-cepstrum implementation:
```cpp
juce::AudioBuffer<float> FirDesigner::design (const CalFile& cal, const FirDesignParams& p) {
    const int order   = p.designFftOrder;          // e.g. 16
    const int fftSize = 1 << order;                 // 65536
    const int nBins   = fftSize / 2 + 1;
    auto mag = targetMagnitudeLinear (cal, p, fftSize); // size nBins, linear gain

    juce::dsp::FFT fft (order);

    if (p.mode == FirMode::MinPhaseMagnitude) {
        // 1) log-magnitude over the full spectrum (mirror to negative freqs)
        std::vector<float> logmag ((size_t) fftSize * 2, 0.0f); // interleaved complex
        for (int k = 0; k < fftSize; ++k) {
            int kk = (k <= fftSize/2) ? k : fftSize - k;        // mirror
            float lm = std::log (std::max (1e-7f, mag[(size_t) kk]));
            logmag[(size_t) k * 2]     = lm;                    // real
            logmag[(size_t) k * 2 + 1] = 0.0f;                  // imag
        }
        // 2) real cepstrum = IFFT(logmag)
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (logmag.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (logmag.data()), true);
        // 3) fold to minimum-phase cepstrum
        std::vector<float> cep ((size_t) fftSize * 2, 0.0f);
        cep[0] = logmag[0]; cep[1] = 0.0f;
        for (int n = 1; n < fftSize/2; ++n) {
            cep[(size_t) n * 2]     = 2.0f * logmag[(size_t) n * 2];
            cep[(size_t) n * 2 + 1] = 0.0f;
        }
        cep[(size_t)(fftSize/2) * 2] = logmag[(size_t)(fftSize/2) * 2];
        // 4) min-phase log-spectrum = FFT(cep); then exponentiate
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (cep.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (cep.data()), false);
        std::vector<float> spec ((size_t) fftSize * 2, 0.0f);
        for (int k = 0; k < fftSize; ++k) {
            float re = cep[(size_t) k*2], im = cep[(size_t) k*2+1];
            float e = std::exp (re);
            spec[(size_t) k*2]   = e * std::cos (im);
            spec[(size_t) k*2+1] = e * std::sin (im);
        }
        // 5) IFFT -> causal min-phase impulse
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (spec.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (spec.data()), true);

        juce::AudioBuffer<float> ir (1, p.numTaps);
        auto* d = ir.getWritePointer (0);
        for (int i = 0; i < p.numTaps; ++i) d[i] = spec[(size_t) i * 2]; // real part
        // tail taper (last 1/8) to avoid truncation ripple
        const int fade = p.numTaps / 8;
        for (int i = 0; i < fade; ++i) {
            float w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) i / (float) fade));
            d[p.numTaps - 1 - i] *= w;
        }
        return ir;
    }

    // ComplexWithPhase path: implemented in Task 6
    return juce::AudioBuffer<float> (1, p.numTaps);
}
```

> `juce::dsp::FFT::perform(in, out, inverse)` operates on `Complex<float>` arrays of length `fftSize`; the interleaved `float` buffers above are reinterpreted as `Complex<float>` (JUCE's `Complex<float>` is `std::complex<float>`, so the memory layout is safe). In JUCE 8, `perform(..., /*inverse=*/true)` **is** 1/N-normalized and the forward transform is unscaled, so the cepstrum chain (inverse → forward → inverse) is a consistently-normalized DFT pair and the final IR is **already** correctly scaled. **Do NOT** divide the IR by `fftSize`. A uniform ~−96 dB (= −20·log10(65536)) offset across *all* frequencies is the *symptom* of an erroneous extra `fftSize` divide (or a double inverse-normalization) — remove it, never add one. Verify against the 1 kHz = 0 dB anchor.

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R "Min-phase" --output-on-failure`
Expected: PASS. JUCE's inverse FFT is already 1/N-normalized, so no scaling line is needed. A uniform ~−96 dB ≈ −20·log10(65536) offset across all frequencies would mean a stray `fftSize` divide crept in — **remove** it (do not add one).

- [ ] **Step 5: Commit**

```bash
git add src/cal/FirDesigner.cpp tests/test_firdesigner.cpp
git commit -m "feat: minimum-phase FIR design via real cepstrum"
```

---

## Task 6: `FirDesigner` — complex (phase-accurate) mode

**Files:**
- Modify: `src/cal/FirDesigner.cpp`
- Test: `tests/test_firdesigner.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_firdesigner.cpp`:
```cpp
TEST_CASE("Complex-mode FIR matches inverted-cal magnitude within 1 dB") {
    eb::CalFile c; c.points = { {20.0,0.0,0.0}, {1000.0,0.0,0.0},
                                {4000.0,20.0,45.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.numTaps = 8192;
    p.mode = eb::FirMode::ComplexWithPhase; p.invert = true; p.maxBoostDb = 12.0;
    auto ir = eb::FirDesigner::design (c, p);
    REQUIRE(ir.getNumSamples() == 8192);
    CHECK_THAT(irMagnitudeDb (ir, 1000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(0.0, 1.0));
    CHECK_THAT(irMagnitudeDb (ir, 4000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(-20.0, 1.0));
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R "Complex-mode" --output-on-failure`
Expected: FAIL (returns zeros).

- [ ] **Step 3: Implement the complex path**

In `src/cal/FirDesigner.cpp`, add a phase-interpolation helper near `interpCalDb`:
```cpp
static double interpCalPhaseDeg (const std::vector<CalPoint>& pts, double hz) {
    if (hz <= pts.front().freqHz) return pts.front().phaseDeg;
    if (hz >= pts.back().freqHz)  return pts.back().phaseDeg;
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) { size_t mid=(lo+hi)/2; if (pts[mid].freqHz<=hz) lo=mid; else hi=mid; }
    const double lf=std::log10(pts[lo].freqHz), hf=std::log10(pts[hi].freqHz);
    const double t=(std::log10(hz)-lf)/(hf-lf);
    return pts[lo].phaseDeg + t*(pts[hi].phaseDeg - pts[lo].phaseDeg);
}
```
Then replace the `// ComplexWithPhase path` return with:
```cpp
    {
        std::vector<float> spec ((size_t) fftSize * 2, 0.0f);
        for (int k = 0; k < nBins; ++k) {
            double hz = (double) k * p.sampleRate / (double) fftSize;
            double ph = juce::degreesToRadians (interpCalPhaseDeg (cal.points, std::max (1.0, hz)));
            if (p.invert) ph = -ph;                 // inverse phase too
            float m = mag[(size_t) k];
            spec[(size_t) k*2]   = m * std::cos ((float) ph);
            spec[(size_t) k*2+1] = m * std::sin ((float) ph);
            if (k > 0 && k < fftSize/2) {           // Hermitian mirror
                spec[(size_t)(fftSize-k)*2]   =  m * std::cos ((float) ph);
                spec[(size_t)(fftSize-k)*2+1] = -m * std::sin ((float) ph);
            }
        }
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (spec.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (spec.data()), true);
        // zero-phase IR is centered at 0 (wraps); rotate so the bulk sits inside numTaps
        juce::AudioBuffer<float> ir (1, p.numTaps);
        auto* d = ir.getWritePointer (0);
        const int half = p.numTaps / 2;
        for (int i = 0; i < p.numTaps; ++i) {
            int src = (i - half + fftSize) % fftSize;   // linear-phase center
            d[i] = spec[(size_t) src * 2];
        }
        // symmetric window
        for (int i = 0; i < p.numTaps; ++i) {
            float w = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (p.numTaps - 1)));
            d[i] *= w;
        }
        return ir;
    }
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R "Complex-mode" --output-on-failure`
Expected: PASS. The single `perform(spec, /*inverse=*/true)` is already 1/N-normalized, so `forward_DFT(IR)[k] == M[k]` — no `fftSize` scaling is needed in complex mode either.

- [ ] **Step 5: Commit**

```bash
git add src/cal/FirDesigner.cpp tests/test_firdesigner.cpp
git commit -m "feat: complex phase-accurate FIR design mode"
```

---

## Task 7: `ProcessingGraph` — per-channel convolution + combine

**Files:**
- Modify: `src/audio/ProcessingGraph.cpp`
- Test: `tests/test_processinggraph.cpp` (Create), `tests/CMakeLists.txt` (Modify)

- [ ] **Step 1: Register the test file**

In `tests/CMakeLists.txt`, add `test_processinggraph.cpp` to `add_executable(eb_tests ...)`.

- [ ] **Step 2: Write the failing test (combine math via identity FIRs)**

Create `tests/test_processinggraph.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/ProcessingGraph.h"

using Catch::Matchers::WithinAbs;

static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("ProcessingGraph combine modes with identity FIRs") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);

    SECTION("Average") {
        g.setCombineMode (eb::CombineMode::Average);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.4f, 1e-4)); // (0.5+0.3)/2
    }
    SECTION("Sum") {
        g.setCombineMode (eb::CombineMode::Sum);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.8f, 1e-4));
    }
    SECTION("TwoPassLeft") {
        g.setCombineMode (eb::CombineMode::TwoPassLeft);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.5f, 1e-4));
    }
    SECTION("TwoPassRight") {
        g.setCombineMode (eb::CombineMode::TwoPassRight);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.3f, 1e-4));
    }
}
```

> Steady-state DC input through an identity (unit-impulse) FIR yields the input value once the convolution latency has filled — checking `out[N-1]` avoids the initial latency ramp.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R ProcessingGraph --output-on-failure`
Expected: FAIL (graph unimplemented).

- [ ] **Step 4: Implement `ProcessingGraph`**

Replace `src/audio/ProcessingGraph.cpp` with:
```cpp
#include "audio/ProcessingGraph.h"

namespace eb {

void ProcessingGraph::prepare (double sampleRate, int maxBlockSize) {
    sr = sampleRate; maxBlock = maxBlockSize;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
    convL.prepare (spec); convR.prepare (spec);
    scratch.setSize (2, maxBlockSize);
}

void ProcessingGraph::setFir (int channel, juce::AudioBuffer<float> ir) {
    auto& conv = (channel == 0) ? convL : convR;
    conv.loadImpulseResponse (std::move (ir), sr,
        juce::dsp::Convolution::Stereo::no,
        juce::dsp::Convolution::Trim::no,
        juce::dsp::Convolution::Normalise::no);
}

void ProcessingGraph::setCombineMode (CombineMode mode) {
    combine.store ((int) mode);
}

void ProcessingGraph::process (const float* inL, const float* inR,
                               float* outMono, int numSamples) {
    auto* l = scratch.getWritePointer (0);
    auto* r = scratch.getWritePointer (1);
    juce::FloatVectorOperations::copy (l, inL, numSamples);
    juce::FloatVectorOperations::copy (r, inR, numSamples);

    { juce::dsp::AudioBlock<float> b (&l, 1, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (b); convL.process (ctx); }
    { juce::dsp::AudioBlock<float> b (&r, 1, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (b); convR.process (ctx); }

    switch ((CombineMode) combine.load()) {
        case CombineMode::TwoPassLeft:
            juce::FloatVectorOperations::copy (outMono, l, numSamples); break;
        case CombineMode::TwoPassRight:
            juce::FloatVectorOperations::copy (outMono, r, numSamples); break;
        case CombineMode::Sum:
            juce::FloatVectorOperations::copy (outMono, l, numSamples);
            juce::FloatVectorOperations::add  (outMono, r, numSamples); break;
        case CombineMode::Average:
            for (int i = 0; i < numSamples; ++i) outMono[i] = 0.5f * (l[i] + r[i]); break;
    }
}

void ProcessingGraph::reset() { convL.reset(); convR.reset(); }

} // namespace eb
```

> `juce::dsp::AudioBlock<float>` over a single channel needs a `float* const*`; the `&l` / `&r` above provide that. `Convolution::process` is allocation-free after `loadImpulseResponse`.

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R ProcessingGraph --output-on-failure`
Expected: PASS (all four sections).

- [ ] **Step 6: Commit**

```bash
git add src/audio/ProcessingGraph.cpp tests/test_processinggraph.cpp tests/CMakeLists.txt
git commit -m "feat: per-channel convolution + combine modes in ProcessingGraph"
```

---

## Task 8: End-to-end DSP integration test (real cal file → graph)

**Files:**
- Test: `tests/test_processinggraph.cpp`

- [ ] **Step 1: Write the failing integration test**

Append to `tests/test_processinggraph.cpp`:
```cpp
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>

TEST_CASE("Real R_HPN cal cuts the ~4 kHz EARS resonance after convolution") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_0000000.txt");
    REQUIRE(f.existsAsFile());
    auto cal = eb::CalFile::parse (f.loadFileAsString());

    eb::FirDesignParams p; p.sampleRate = 48000.0; p.numTaps = 8192;
    p.mode = eb::FirMode::MinPhaseMagnitude; p.invert = true; p.maxBoostDb = 12.0;
    auto ir = eb::FirDesigner::design (cal, p);

    // Measure the filter's response at 4 kHz (cal there is ~ +21.7 dB -> expect a deep cut)
    const int order = 16, fftSize = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
    for (int i = 0; i < juce::jmin (ir.getNumSamples(), fftSize); ++i)
        buf[(size_t) i] = ir.getSample (0, i);
    fft.performRealOnlyForwardTransform (buf.data());
    int bin = (int) std::lround (4000.0 * fftSize / p.sampleRate);
    float re = buf[(size_t) bin*2], im = buf[(size_t) bin*2+1];
    double db = 20.0 * std::log10 (std::max (1e-9f, std::sqrt (re*re + im*im)));
    CHECK(db < -10.0);   // inverse of a large positive bump = a strong cut
}
```

- [ ] **Step 2: Run to verify it passes (design already implemented)**

Run: `cmake --build build --target eb_tests && ctest --test-dir build -R "Real R_HPN" --output-on-failure`
Expected: PASS. (A constant magnitude offset means a stray `fftSize` divide was introduced — remove it; JUCE's inverse FFT normalizes automatically.)

- [ ] **Step 3: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/test_processinggraph.cpp
git commit -m "test: end-to-end cal-file to convolved-response integration"
```

---

## Self-review notes (for the executor)
- JUCE's inverse FFT (`perform(..., true)`) is **already** 1/N-normalized — never add an `fftSize` divide on the final IR. If magnitudes are off by a constant dB across **all** frequencies (~−96 dB), look for a double inverse-normalization or a stray `fftSize` divide (or a mismatched measurement transform) and remove it.
- The combine tests check `out[N-1]` to skip convolution latency; do not assert on `out[0]`.
- `numTaps` must be a power of two for the partitioned convolution to be efficient; keep 8192/4096.

## Done criteria for Plan 1
- `ctest --test-dir build` is green: CalFile (parse/serial/type/validate/real-fixture), FirDesigner (target/clamp/min-phase magnitude+causality/complex), ProcessingGraph (4 combine modes + real-cal integration).
- `cmake --build build` builds the `EarsBridge` app shell on Windows and macOS.
- No device or GUI code yet — that's Plan 2 (audio engine: device layer + two-clock FIFO+ASRC bridge) and Plan 3 (GUI).
