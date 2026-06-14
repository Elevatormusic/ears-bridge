# EARS Bridge — Plan 4: Health monitoring, macOS aggregate, bench validation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Pre-execution:** run an **API-verification pass** (as Plan 1 did) over this plan before implementing — it grounds the JUCE / CoreAudio / CMake API calls (`AudioIODeviceType::hasSeparateInputsAndOutputs`, `AudioIODevice::getAvailableSampleRates`, `juce::AbstractFifo`, `juce::LagrangeInterpolator`, `juce::PropertiesFile`, `Timer`, `AudioHardwareCreateAggregateDevice`/`…Destroy…`, `kAudioSubDeviceDriftCompensationKey`) against current docs. The load-bearing APIs below were grounded via context7 (`/websites/juce_master`) and the Apple CoreAudio docs during authoring; re-verify on execution because JUCE/macOS APIs drift.

**Goal:** Make the bridge robust — full `HealthMonitor` (mid-sweep dropout/drift detection, clip/low-level warnings tied to the hardware DIP gain), device-identity re-bind that survives replug for **both** EARS and EARS Pro, a one-time physical L/R verification routine, the macOS CoreAudio Aggregate-Device routing path, and a concrete bench-validation runbook encoding the design-spec §13 gates.

**Architecture:** Plan 4 sits on top of the Plan-2 `AudioEngine` facade. `HealthMonitor` (already created basic in Plan 2) is extended into a lock-free telemetry + state-machine unit fed from both audio callbacks via atomics only. A `CalBinder` helper (new, pure logic) re-associates per-ear cal files to a re-enumerated device by `DeviceId::key()` including `EarsModel`. An `AsioFallback` helper (new, pure logic, mockable) encodes the `hasSeparateInputsAndOutputs()==false` → WASAPI fallback decision. On macOS, `AggregateDevice_mac` wraps `AudioHardwareCreateAggregateDevice` (drift correction on, clock = EARS) as the preferred routing, falling back to the Plan-2 `ClockBridge` FIFO+ASRC. All pure-logic units are Catch2-tested; device/Dirac/bench interactions are MANUAL VERIFICATION with explicit PASS criteria.

**Tech Stack:** C++20, JUCE 8.0.4 (via CMake `FetchContent`), CMake ≥ 3.22, Catch2 v3.6.0, `ctest`. Modules: `juce_core`, `juce_audio_basics`, `juce_audio_devices`, `juce_dsp`, `juce_gui_extra`. macOS extra: `CoreAudio.framework`, `AudioToolbox.framework`, Objective-C++ (`.mm`).

**Plan series (context):** Plan 1 = scaffold + DSP core (**DONE**, committed, rate-agnostic). Plan 2 = audio engine (`DeviceManager`, `ClockBridge`, `HealthMonitor` basic, `AudioEngine` facade). Plan 3 = GUI (`Theme`, meters, cal slots, pickers, `MainComponent`, `Settings`). Plan 4 = **this doc**: health monitoring full, device-identity robustness, L/R verify, macOS aggregate device, bench runbook.

**Spec:** `docs/superpowers/specs/2026-06-13-ears-bridge-design.md` (§3 device reality + 16/24-bit policy, §4 architecture/clocking, §5.6 health monitoring, §5.1 device layer, §9 sample-rate policy, §13 bench gates implemented here).

---

## Build environment (BUILD ENV — verbatim from Plan 1)

This machine has **CMake 3.30**, **Git**, **Visual Studio Build Tools 2026 (MSVC v18, cl 19.51)**, **no Ninja on PATH**, and **no VS-2026 generator in CMake 3.30** — so the build uses **Ninja + MSVC**, entered through the committed `tools/dev.cmd` wrapper (loads `vcvars64.bat`, puts VS-bundled Ninja on PATH; a batch wrapper, not PowerShell, to avoid any execution-policy bypass).

- **Windows** — wrap every `cmake`/`ctest` in `cmd /c "tools\dev.cmd …"` (PowerShell tool) or `cmd //c "tools\\dev.cmd …"` (Bash tool):
  - Configure: `cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"`
  - Build:     `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"`
  - Test:      `cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure"`
- **macOS** — run `cmake`/`ctest` directly (Xcode clang + Ninja), no wrapper:
  - Configure: `cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release`
  - Build:     `cmake --build build --target eb_tests`
  - Test:      `ctest --test-dir build --output-on-failure`
- **git** — run **directly**, never through `tools/dev.cmd` (the wrapper mangles quoted commit messages). Pass any `ctest -R <regex>` **unescaped**. Do **not** use `powershell -ExecutionPolicy Bypass`.

Verified in Plan 1: `tools/dev.cmd` exposes `cl 19.51`, `ninja 1.13.2`, `cmake 3.30`.

---

## File structure

### Created by this plan

```
src/
  audio/
    CalBinder.h        CalBinder.cpp        # re-bind per-ear cal to a re-enumerated DeviceId by key()+model
    AsioFallback.h     AsioFallback.cpp     # ASIO-incapability detection -> WASAPI fallback decision (mockable)
    LrVerify.h         LrVerify.cpp         # one-time physical L/R verification state machine (pure logic)
  platform/
    AggregateDevice_mac.h  AggregateDevice_mac.mm   # CoreAudio aggregate device (macOS only)
tests/
  test_healthmonitor.cpp                    # thresholds + dropout/drift flag state machines
  test_calbinder.cpp                        # re-bind logic incl. model
  test_asiofallback.cpp                     # ASIO-detection branch (mock device-type)
  test_lrverify.cpp                         # L/R verify state machine
docs/
  bench-validation-runbook.md               # §13 gates as concrete MANUAL procedures w/ PASS criteria
```

### Modified by this plan

```
src/audio/HealthMonitor.h    HealthMonitor.cpp   # EXTEND Plan-2 (keep ALL Plan-2 methods): ADD dropout/drift latch, flags(), cleanCapture(), DipGainProfile
src/audio/EngineTypes.h                          # ADD HealthFlags bitfield + DipGainProfile (additive; do not remove spine fields)
src/audio/AudioEngine.h      AudioEngine.cpp     # EXTEND: instrument the existing `hm` with the new flag/threshold logic; macOS aggregate preference; expose preflightLrVerify()
CMakeLists.txt                                   # add new sources to eb_engine; macOS frameworks; aggregate .mm
tests/CMakeLists.txt                             # register the four new test files
```

> **HealthMonitor is ONE class with ONE surface.** Plan 2 defines the canonical API
> (`reset`, `reportXrun`, `reportDroppedFrames(long long)`, `setFifoFill(double)`,
> `setCaptureToRenderRatio(double)`, `reportInLevels(float,float,bool,bool)`,
> `reportOutLevel(float,bool)`, `snapshot() -> Health`, `levels() -> Levels`). Plan 4 **adds**
> members/methods to that same class — it never replaces or renames the Plan-2 methods.
> `AudioEngine` keeps its single `HealthMonitor` member named `hm`; Plan 4 edits the existing
> `hm` and the existing `levels()`/`health()` bodies, and introduces **no** second monitor.

> **Plan-2 ownership note (read before editing):** `EngineTypes.h`, `DeviceId.h`, `DeviceManager.{h,cpp}`, `ClockBridge.{h,cpp}`, `HealthMonitor.{h,cpp}` (basic), and `AudioEngine.{h,cpp}` are **created by Plan 2** and the `eb_engine` library target is **added to `CMakeLists.txt` by Plan 2**. This plan **extends** them. If you are executing plans out of order and these files do not exist, stop and execute Plan 2 first — Plan 4 does not re-create the spine.

All code lives in `namespace eb`.

---

## Shared contract (the SPINE — use verbatim; Plans 2/3/4 share these)

These are created/owned by Plan 2 and consumed here. **Do not invent variants.** Namespace `eb`, C++20, JUCE 8.

```cpp
// src/audio/EngineTypes.h  (Plan 2 owns; Plan 4 ADDS HealthFlags + DipGainProfile, additively)
namespace eb {

enum class EarsModel { Unknown, Ears, EarsPro };   // set by DeviceManager from device name / USB VID:PID

enum class EngineStatus { Stopped, Running, Error };

struct Levels {
    float inL = 0, inR = 0, outMono = 0;
    bool clipL = false, clipR = false, clipOut = false;
};

struct Health {
    int       xruns               = 0;
    long long droppedFrames       = 0;
    double    fifoFill            = 0.0;
    bool      cleanCapture        = true;
    double    captureToRenderRatio = 1.0;
    // Plan 4 adds one field here: `HealthFlag flags = HealthFlag::None;` (see Task 1). The five
    // spine fields above are unchanged; `flags` is declared after HealthFlag below.
};

} // namespace eb
```

```cpp
// src/audio/DeviceId.h  (Plan 2 owns; consumed here)
namespace eb {
struct DeviceId {
    juce::String typeName, name, uid;
    bool         isVirtualSink = false;
    EarsModel    model         = EarsModel::Unknown;
    bool operator== (const DeviceId&) const;
    juce::String key() const;   // typeName + "|" + name + "|" + uid, stable across replug
};
}
```

```cpp
// src/audio/ClockBridge.h  (Plan 2 owns; consumed by AudioEngine, referenced by the macOS fallback)
namespace eb {
class ClockBridge {
public:
    void prepare (double captureRate, double renderRate, int channels, int capacityFrames);
    void pushCapture (const float* mono, int numFrames);   // producer = capture callback
    int  pullRender  (float* out, int numFrames);          // consumer = render callback; returns frames written
    void setRenderRate (double);
    void reset();
    double fifoFill() const;
    int    underruns() const;
    int    overruns() const;
};
}
```

```cpp
// src/audio/AudioEngine.h  (Plan 2 owns; the ONLY surface the GUI talks to. Plan 4 ADDS preflightLrVerify())
namespace eb {
class AudioEngine {
public:
    std::vector<DeviceId> inputDevices() const;
    std::vector<DeviceId> outputDevices() const;                    // virtual sinks flagged isVirtualSink
    std::vector<double>   supportedSampleRates (const DeviceId&) const; // EARS -> {48000}; EARS Pro -> {44100,48000,88200,96000,176400,192000}
    std::vector<int>      supportedBitDepths   (const DeviceId&) const; // EARS -> {24}; EARS Pro -> {16,24,32}
    void setInput  (const DeviceId&);                               // configure while Stopped
    void setOutput (const DeviceId&);
    void setSampleRate    (double sr);                              // one of the input native rates
    void setOutputBitDepth (int bits);                             // 16/24/32 (24 default); best-effort request on the render format
    void setLeftCalFir  (juce::AudioBuffer<float> fir);            // from eb::FirDesigner::design; hot-swappable while Running
    void setRightCalFir (juce::AudioBuffer<float> fir);
    void setCombineMode (eb::CombineMode);
    bool start (juce::String& errorOut);
    void stop();
    EngineStatus status() const;
    Levels levels() const;   // lock-free snapshot for the GUI timer
    Health health() const;   // lock-free snapshot
};
}
```

Existing from Plan 1 (reuse as-is, already rate-agnostic): `eb::CalFile`, `eb::FirDesigner` + `FirDesignParams` + `FirMode`, `eb::ProcessingGraph`, `eb::CombineMode`.

### Dataflow the engine wires (for context — Plan 2 builds it; Plan 4 instruments it)

```
Capture cb (2ch @ input native rate)
  -> eb::ProcessingGraph::process (per-ear FIR + combine) -> mono @ capture rate
  -> ClockBridge.pushCapture                                  [capture clock]
Virtual render cb (@ device rate)
  -> ClockBridge.pullRender -> duplicate mono to BOTH output channels (L=R)  [render clock = master]
HealthMonitor updated from BOTH callbacks via atomics only (no locks/allocs/syscalls).
```

### Types this plan DEFINES (added to the spine, additively)

```cpp
// ADDED to src/audio/EngineTypes.h by Plan 4.
// PLACEMENT (see Task 1 for the exact edits): `HealthFlag` (+ operators + any()) is inserted
// ABOVE the spine `Health` struct, the spine `Health` struct GAINS a single field
// `HealthFlag flags = HealthFlag::None;`, and `DipGainProfile` is appended after `Health`.
// The five existing `Health` fields and the other spine types are unchanged.
namespace eb {

// Hardware DIP-switch analog-gain range per model (no software control; used for low-level guidance).
struct DipGainProfile {
    double minDb = 0.0;
    double maxDb = 36.0;   // original EARS: 0-36 dB; EARS Pro: 0-45 dB
    static DipGainProfile forModel (EarsModel m) noexcept {
        return (m == EarsModel::EarsPro) ? DipGainProfile { 0.0, 45.0 }
                                         : DipGainProfile { 0.0, 36.0 };
    }
};

// Sticky health condition flags, OR-ed as conditions are detected during a run; cleared on start()/reset().
enum class HealthFlag : unsigned {
    None          = 0,
    Xrun          = 1u << 0,   // device reported an xrun / dropped buffer
    Dropout       = 1u << 1,   // mid-sweep capture dropout (silence-filled render block)
    ExcessDrift   = 1u << 2,   // |captureToRenderRatio - 1| beyond threshold, sustained
    ClipInput     = 1u << 3,   // input near full scale -> lower the DIP gain
    LowLevel      = 1u << 4,   // input very low -> raise the DIP gain
    ClipOutput    = 1u << 5,   // mono output near full scale -> lower output trim
    FifoStarved   = 1u << 6    // FIFO emptied (underrun) during a render block
};
constexpr HealthFlag operator| (HealthFlag a, HealthFlag b) noexcept {
    return static_cast<HealthFlag> (static_cast<unsigned> (a) | static_cast<unsigned> (b));
}
constexpr HealthFlag operator& (HealthFlag a, HealthFlag b) noexcept {
    return static_cast<HealthFlag> (static_cast<unsigned> (a) & static_cast<unsigned> (b));
}
constexpr bool any (HealthFlag f) noexcept { return static_cast<unsigned> (f) != 0u; }

} // namespace eb
```

> Tolerances used in tests: level/ratio comparisons exact to **1e-6** unless a tolerance is stated. dB conversions use `20*log10`. The drift threshold default is **±0.5%** (`0.005`) sustained over **N=8** consecutive observations; clip threshold **−1.0 dBFS** (linear `0.8913`); low-level threshold **−50 dBFS** (linear `0.00316`). These are constants in `HealthMonitor` and are asserted directly by the tests.

---

## Task 1: Extend `EngineTypes.h` with `DipGainProfile` + `HealthFlag`

**Files:**
- Modify: `src/audio/EngineTypes.h`
- Test: `tests/test_healthmonitor.cpp` (**append to the existing file** — Plan 2 Task 4 created and registered it), `tests/CMakeLists.txt` (no change if Plan 2 already added the source)

This task adds the additive spine types and a first test that pins their semantics. `HealthMonitor` proper (the extended unit) arrives in Task 2. **Composition note:** Plan 2 Task 4 already created `tests/test_healthmonitor.cpp` (testing the Plan-2 basic surface — `reportXrun`, `reportDroppedFrames`, `setFifoFill`, `setCaptureToRenderRatio`, `reportInLevels`, `reportOutLevel`, `snapshot`, `levels`, clean-capture latch) and added it to `add_executable(eb_tests …)`. **Keep those Plan-2 cases.** This plan APPENDS new cases; it does not overwrite the file or re-register it.

- [ ] **Step 1: Ensure the test file is registered (it already is, from Plan 2 Task 4)**

`tests/test_healthmonitor.cpp` is already in the `add_executable(eb_tests …)` list courtesy of Plan 2 Task 4. If — and only if — you are running Plan 4 against a tree where Plan 2 did not register it, add `test_healthmonitor.cpp` to that list. Do NOT add it twice. For reference the list reads:
```cmake
add_executable(eb_tests
    test_smoke.cpp
    test_calfile.cpp
    test_firdesigner.cpp
    test_processinggraph.cpp
    test_healthmonitor.cpp)
```

- [ ] **Step 2: Write the failing test (gain profile + flag algebra)**

**Append** to the existing `tests/test_healthmonitor.cpp` (Plan 2 Task 4 already opened it with
`#include <catch2/catch_test_macros.hpp>`, the floating-point matchers, and `#include "audio/HealthMonitor.h"`
— which transitively includes `audio/EngineTypes.h`). Add only the one include this task needs at the
top if it is not already present, then append the new cases:
```cpp
#include <cmath>   // std::abs in the Task-2 level/ratio round-trip cases appended below
// (HealthMonitor.h / EngineTypes.h already included by the Plan-2 test prologue above)

TEST_CASE("DipGainProfile reflects each EARS model range") {
    auto ears = eb::DipGainProfile::forModel (eb::EarsModel::Ears);
    CHECK(ears.minDb == 0.0);
    CHECK(ears.maxDb == 36.0);

    auto pro = eb::DipGainProfile::forModel (eb::EarsModel::EarsPro);
    CHECK(pro.minDb == 0.0);
    CHECK(pro.maxDb == 45.0);

    // Unknown falls back to the conservative original-EARS range.
    auto unk = eb::DipGainProfile::forModel (eb::EarsModel::Unknown);
    CHECK(unk.maxDb == 36.0);
}

TEST_CASE("HealthFlag bitwise algebra ORs and ANDs as expected") {
    using eb::HealthFlag;
    auto combined = HealthFlag::Xrun | HealthFlag::ClipInput;
    CHECK(eb::any (combined & HealthFlag::Xrun));
    CHECK(eb::any (combined & HealthFlag::ClipInput));
    CHECK_FALSE(eb::any (combined & HealthFlag::Dropout));
    CHECK_FALSE(eb::any (HealthFlag::None & HealthFlag::Xrun));
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R "DipGainProfile|HealthFlag" --output-on-failure"`
Expected: **FAIL** — `DipGainProfile` / `HealthFlag` are undefined (compile error: members not declared in `EngineTypes.h`).

- [ ] **Step 4: Add the types to `EngineTypes.h`**

Make three additive edits inside `namespace eb { … }` in `src/audio/EngineTypes.h`. Spine types `EarsModel`, `EngineStatus`, `Levels` are untouched; `Health` keeps every existing field and only **gains** a `flags` member.

**(a)** `HealthFlag` must be declared **before** `Health` (because `Health` now holds a `HealthFlag`). Insert this block **immediately above** the existing `struct Health { … };`:
```cpp
// --- Plan 4 addition: sticky health condition flags ---
// OR-ed as conditions are detected during a run; cleared on start()/reset().
enum class HealthFlag : unsigned {
    None        = 0,
    Xrun        = 1u << 0,
    Dropout     = 1u << 1,
    ExcessDrift = 1u << 2,
    ClipInput   = 1u << 3,
    LowLevel    = 1u << 4,
    ClipOutput  = 1u << 5,
    FifoStarved = 1u << 6
};
constexpr HealthFlag operator| (HealthFlag a, HealthFlag b) noexcept {
    return static_cast<HealthFlag> (static_cast<unsigned> (a) | static_cast<unsigned> (b));
}
constexpr HealthFlag operator& (HealthFlag a, HealthFlag b) noexcept {
    return static_cast<HealthFlag> (static_cast<unsigned> (a) & static_cast<unsigned> (b));
}
constexpr bool any (HealthFlag f) noexcept { return static_cast<unsigned> (f) != 0u; }
```

**(b)** Add ONE field to the existing `Health` struct (do not remove or reorder the spine fields). After the edit `Health` reads:
```cpp
struct Health {
    int       xruns               = 0;
    long long droppedFrames       = 0;
    double    fifoFill            = 0.0;
    bool      cleanCapture        = true;
    double    captureToRenderRatio = 1.0;
    HealthFlag flags = HealthFlag::None;   // Plan 4 addition: latched sticky condition flags
};
```

**(c)** Append the gain profile **after** `Health` (it does not need to precede anything):
```cpp
// --- Plan 4 addition: hardware DIP-switch analog-gain range per model ---
// (no software control; used for low-level guidance).
struct DipGainProfile {
    double minDb = 0.0;
    double maxDb = 36.0;   // original EARS: 0-36 dB; EARS Pro: 0-45 dB
    static DipGainProfile forModel (EarsModel m) noexcept {
        return (m == EarsModel::EarsPro) ? DipGainProfile { 0.0, 45.0 }
                                         : DipGainProfile { 0.0, 36.0 };
    }
};
```

> `EngineTypes.h` already `#include <juce_core/juce_core.h>` (added in Plan 2 Task 1; required so `juce::String` in sibling spine headers resolves and so Plan 4's stated precondition holds) — these additions need no new include. Adding `flags` to `Health` is field-name-addressed everywhere it is read, so no existing reader breaks.

- [ ] **Step 5: Run to verify pass**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R "DipGainProfile|HealthFlag" --output-on-failure"`
Expected: **PASS** (both test cases).

- [ ] **Step 6: Commit**

```bash
git add src/audio/EngineTypes.h tests/test_healthmonitor.cpp tests/CMakeLists.txt
git commit -m "feat(plan4): add DipGainProfile + HealthFlag to EngineTypes spine"
```

---

## Task 2: `HealthMonitor` full — thresholds, latch, clip/low-level warnings

**Files:**
- Modify: `src/audio/HealthMonitor.h`, `src/audio/HealthMonitor.cpp`
- Test: `tests/test_healthmonitor.cpp`

**EXTENDS** the Plan-2 `HealthMonitor` **additively** — every Plan-2 method keeps its exact name and semantics (`reset`, `reportXrun`, `reportDroppedFrames`, `setFifoFill`, `setCaptureToRenderRatio`, `reportInLevels`, `reportOutLevel`, `snapshot`, `levels`). Plan 4 only **adds**: the dropout/excess-drift latch, the `HealthFlag` bookkeeping with `flags()`/`cleanCapture()`, a `DipGainProfile` via `gainProfile()`, a `prepare(model, capacity)` configure-and-reset, and one new additive observer `observeRenderBlock(...)` that folds the per-block dropout/drift logic **on top of** the existing `setFifoFill`/`setCaptureToRenderRatio` (it calls them — no duplicate state). The new flag/threshold logic is folded into the existing `report*` methods and `snapshot()` populates the spine `Health.cleanCapture` plus a new additive `Health.flags`. The monitor stays updated **only** via atomic-friendly setters from the two audio callbacks; the GUI reads lock-free snapshots.

> **Realtime-safety contract:** every method on the audio-callback path (`reportInLevels`, `reportOutLevel`, `observeRenderBlock`, `reportXrun`, `reportDroppedFrames`, `setFifoFill`, `setCaptureToRenderRatio`) does only atomic stores/loads and integer arithmetic — **no** allocation, locks, or syscalls. `snapshot`/`levels`/`flags`/`reset`/`prepare` run on the GUI/control thread (or once at start).

> **`Health.flags` precondition:** Task 1 declares `HealthFlag` in `EngineTypes.h` **before** `Health` and adds a `HealthFlag flags = HealthFlag::None;` field to `Health` (additive — the existing spine fields are unchanged). `snapshot()` populates it from the latched flag bits. See Task 1 Step 4.

- [ ] **Step 1: Write the extended header (Plan-2 methods kept verbatim; Plan-4 additions marked)**

**Extend** `src/audio/HealthMonitor.h` in place — rewrite the file so that the block marked `// ---- Plan 2 canonical surface ----` keeps the Plan-2 API **byte-for-byte** (no method removed or renamed) and everything under `// ---- Plan 4 additions ----` is purely new. This is an extension, not an API replacement:
```cpp
#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"
#include <atomic>

namespace eb {

// Lock-free telemetry + dropout/drift state machine fed from both audio callbacks.
// Plan 2 created the canonical surface below; Plan 4 EXTENDS it additively (the Plan-2
// methods keep their exact names and behavior).
class HealthMonitor {
public:
    // ---- Plan 2 canonical surface (names/signatures unchanged) ----
    void reset();

    void reportXrun();                       // a device xrun / dropped callback
    void reportDroppedFrames (long long n);  // frames lost at the bridge (under/overrun)
    void setFifoFill (double frac);
    void setCaptureToRenderRatio (double r);
    void reportInLevels (float peakL, float peakR, bool clipL, bool clipR);
    void reportOutLevel  (float peakMono, bool clipOut);

    Health snapshot() const;                 // now also carries flags + cleanCapture
    Levels levels()  const;

    // ---- Plan 4 additions (additive; do not remove or rename the Plan-2 methods above) ----

    // Thresholds (public constants so tests assert exact boundaries).
    static constexpr float  kClipLinear         = 0.8913f;   // -1.0 dBFS
    static constexpr float  kLowLevelLinear      = 0.00316f;  // -50 dBFS (peak over a sweep window)
    static constexpr double kDriftRatioTol       = 0.005;     // +/-0.5% sustained
    static constexpr int    kDriftSustainBlocks  = 8;         // consecutive out-of-tol blocks to latch
    static constexpr int    kLowLevelGraceBlocks = 64;        // ignore initial silence before the sweep starts

    // Configure per-run: stores the model (for gainProfile) + capacity + the NOMINAL
    // capture:render ratio (drift is measured against this, not 1.0), then reset()s all state.
    void prepare (EarsModel model, int fifoCapacityFrames, double nominalRatio = 1.0) noexcept;

    // One additive per-render-block observer. framesWanted vs framesGot (got<wanted => silence
    // fill => Dropout + FifoStarved); ratio + fill are forwarded to the existing setters so there
    // is exactly one copy of that state. Advances the block counter (low-level grace window).
    void observeRenderBlock (int framesWanted, int framesGot,
                             double captureToRenderRatio, double fifoFillFrac) noexcept;

    HealthFlag     flags() const noexcept;        // latched sticky condition flags
    bool           cleanCapture() const noexcept; // latched false on a measurement-invalidating condition
    DipGainProfile gainProfile() const noexcept { return DipGainProfile::forModel (model_); }

private:
    void raise (HealthFlag f) noexcept;   // OR into flags; clear cleanCapture for invalidating flags

    EarsModel model_   = EarsModel::Unknown;
    int       capacity_ = 0;
    double    nominal_  = 1.0;   // nominal capture:render ratio; drift measured against THIS, not 1.0

    // Level/clip telemetry (peak*1000 fixed-point to keep the Plan-2 atomics-only contract).
    std::atomic<int>  inLm { 0 }, inRm { 0 }, outM { 0 };   // peak * 1000
    std::atomic<bool> cL { false }, cR { false }, cO { false };

    std::atomic<int>       xrunsA { 0 };
    std::atomic<long long> droppedA { 0 };
    std::atomic<int>       fifoFillMilli { 500 };   // fill * 1000
    std::atomic<int>       ratioMicro { 1000000 };  // ratio * 1e6

    std::atomic<unsigned>  flagBits { 0 };
    std::atomic<bool>      clean { true };

    std::atomic<int> driftRun { 0 };     // consecutive out-of-tol blocks
    std::atomic<int> blockCount { 0 };   // for the low-level grace window
};

} // namespace eb
```

- [ ] **Step 2: Write the failing tests (drive the Plan-2 canonical methods + the additive ones)**

Append to `tests/test_healthmonitor.cpp` (no new includes needed — `audio/HealthMonitor.h` is already included by the Plan-2 prologue, and `<cmath>` was added in Task 1 Step 2). These call the **Plan-2** method names (`reportInLevels`, `reportOutLevel`, `reportXrun`, `reportDroppedFrames`, `snapshot`, `levels`) plus the **additive** `prepare`/`observeRenderBlock`/`flags`/`cleanCapture` — there is no second/renamed API:
```cpp
TEST_CASE("HealthMonitor flags input clip above -1 dBFS but clip does NOT invalidate cleanCapture") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    // L peak clips (>= kClipLinear), R is fine; the caller passes the matching clip bools.
    h.reportInLevels (0.95f, 0.10f, true, false);
    auto lv = h.levels();
    CHECK(lv.clipL);
    CHECK_FALSE(lv.clipR);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ClipInput));
    // Clip is a guidance warning, not a measurement invalidation.
    CHECK(h.cleanCapture());
}

TEST_CASE("HealthMonitor flags ClipOutput from reportOutLevel") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.reportOutLevel (0.95f, true);   // mono output clips
    CHECK(h.levels().clipOut);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ClipOutput));
    CHECK(h.cleanCapture());          // output clip is guidance, not invalidation
}

TEST_CASE("HealthMonitor flags low input level only after the grace window") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 4096);
    // During grace: silent input must NOT raise LowLevel.
    for (int i = 0; i < eb::HealthMonitor::kLowLevelGraceBlocks; ++i) {
        h.observeRenderBlock (256, 256, 1.0, 0.5);  // advances the block counter
        h.reportInLevels (0.0f, 0.0f, false, false);
    }
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::LowLevel));
    // After grace, a sustained near-silent capture raises LowLevel.
    h.observeRenderBlock (256, 256, 1.0, 0.5);
    h.reportInLevels (0.001f, 0.001f, false, false);
    CHECK(eb::any (h.flags() & eb::HealthFlag::LowLevel));
}

TEST_CASE("HealthMonitor latches Dropout and invalidates cleanCapture on silence-filled render") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    CHECK(h.cleanCapture());
    h.observeRenderBlock (256, 200, 1.0, 0.1);     // got < wanted -> FIFO starved + dropout
    CHECK(eb::any (h.flags() & eb::HealthFlag::Dropout));
    CHECK(eb::any (h.flags() & eb::HealthFlag::FifoStarved));
    CHECK_FALSE(h.cleanCapture());                 // latched
    // Latch is sticky: a subsequent clean block does not clear it.
    h.observeRenderBlock (256, 256, 1.0, 0.5);
    CHECK_FALSE(h.cleanCapture());
    CHECK(h.snapshot().droppedFrames == 56);       // 256 - 200 accounted via observeRenderBlock
}

TEST_CASE("HealthMonitor latches ExcessDrift only after sustained out-of-tolerance ratio") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 4096);
    const double bad = 1.0 + 2.0 * eb::HealthMonitor::kDriftRatioTol; // +1.0% > 0.5% tol
    // One bad block must NOT latch.
    h.observeRenderBlock (256, 256, bad, 0.5);
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
    // Sustain to the threshold -> latch.
    for (int i = 1; i < eb::HealthMonitor::kDriftSustainBlocks; ++i)
        h.observeRenderBlock (256, 256, bad, 0.5);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
    CHECK_FALSE(h.cleanCapture());
    // observeRenderBlock forwarded the ratio through the Plan-2 micro-fixed-point setter, so
    // compare within the 1e-6 storage resolution rather than bit-exact.
    CHECK(std::abs (h.snapshot().captureToRenderRatio - bad) < 1e-6);
}

TEST_CASE("HealthMonitor: an in-tolerance block resets the drift run so noise does not accumulate") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 4096);
    const double bad = 1.0 + 2.0 * eb::HealthMonitor::kDriftRatioTol;
    for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks - 1; ++i)
        h.observeRenderBlock (256, 256, bad, 0.5);
    h.observeRenderBlock (256, 256, 1.0, 0.5);     // good block resets the run
    h.observeRenderBlock (256, 256, bad, 0.5);     // start over
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
}

TEST_CASE("HealthMonitor measures drift against the NOMINAL ratio, not 1.0 (mismatched-rate run)") {
    // 96k capture -> 48k render: nominal capture:render ratio is 2.0. A ratio AT nominal must NOT
    // latch ExcessDrift (the pre-fix |ratio-1.0| check would have falsely tripped on every block).
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 16384, 2.0);
    for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks + 4; ++i)
        h.observeRenderBlock (256, 256, 2.0, 0.5);          // on-nominal: no drift
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));

    // A ratio deviating from nominal by > tol, sustained, DOES latch.
    const double off = 2.0 * (1.0 + 2.0 * eb::HealthMonitor::kDriftRatioTol);
    for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks; ++i)
        h.observeRenderBlock (256, 256, off, 0.5);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
}

TEST_CASE("HealthMonitor xrun counter, flag, and reset (Plan-2 reportXrun preserved)") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.reportXrun(); h.reportXrun();
    CHECK(h.snapshot().xruns == 2);
    CHECK(eb::any (h.flags() & eb::HealthFlag::Xrun));
    CHECK_FALSE(h.cleanCapture());                 // xrun invalidates a measurement (spec §3.5)
    h.reset();
    CHECK(h.snapshot().xruns == 0);
    CHECK(h.cleanCapture());
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::Xrun));
}

TEST_CASE("HealthMonitor: Plan-2 reportDroppedFrames still accumulates + invalidates") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.reportDroppedFrames (128);
    h.reportDroppedFrames (64);
    CHECK(h.snapshot().droppedFrames == 192);
    CHECK_FALSE(h.cleanCapture());                 // dropped frames invalidate the run
}

TEST_CASE("HealthMonitor: Plan-2 setFifoFill / setCaptureToRenderRatio / levels round-trip") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.setFifoFill (0.42);
    h.setCaptureToRenderRatio (2.0);
    h.reportInLevels (0.5f, 0.25f, false, false);
    h.reportOutLevel (0.8f, false);
    auto s  = h.snapshot();
    auto lv = h.levels();
    CHECK(std::abs (s.fifoFill - 0.42) < 1e-3);
    CHECK(std::abs (s.captureToRenderRatio - 2.0) < 1e-3);
    CHECK(std::abs (lv.inL - 0.5f) < 2e-3f);
    CHECK(std::abs (lv.inR - 0.25f) < 2e-3f);
    CHECK(std::abs (lv.outMono - 0.8f) < 2e-3f);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R HealthMonitor --output-on-failure"`
Expected: **FAIL** — link/compile errors (methods unimplemented).

- [ ] **Step 4: Implement `HealthMonitor.cpp` (extend in place — keep every Plan-2 body)**

**Extend** `src/audio/HealthMonitor.cpp` in place — rewrite the file so that every Plan-2 method body is preserved (only the additive flag/latch logic is folded in, clearly marked); the Plan-4 additions (`prepare`, `observeRenderBlock`, `flags`, `cleanCapture`, `raise`) are new. This is an extension, not an API replacement:
```cpp
#include "audio/HealthMonitor.h"
#include <algorithm>
#include <cmath>

namespace eb {

// ---- Plan 4 addition: flag bookkeeping ------------------------------------------------
void HealthMonitor::raise (HealthFlag f) noexcept {
    flagBits.fetch_or (static_cast<unsigned> (f));
    // Conditions that invalidate a measurement clear cleanCapture; pure guidance warnings do not.
    const unsigned invalidating =
        static_cast<unsigned> (HealthFlag::Xrun)        |
        static_cast<unsigned> (HealthFlag::Dropout)     |
        static_cast<unsigned> (HealthFlag::ExcessDrift) |
        static_cast<unsigned> (HealthFlag::FifoStarved);
    if ((static_cast<unsigned> (f) & invalidating) != 0u)
        clean.store (false);
}

// ---- Plan 4 addition: per-run configure -----------------------------------------------
void HealthMonitor::prepare (EarsModel m, int fifoCapacityFrames, double nominalRatio) noexcept {
    model_ = m;
    capacity_ = fifoCapacityFrames;
    nominal_ = (nominalRatio > 0.0 ? nominalRatio : 1.0);
    reset();
}

// ---- Plan 2 canonical surface (bodies preserved; flag logic folded in where noted) -----
void HealthMonitor::reset() {
    xrunsA.store (0); droppedA.store (0);
    fifoFillMilli.store (500); ratioMicro.store (1000000);
    clean.store (true);
    inLm.store (0); inRm.store (0); outM.store (0);
    cL.store (false); cR.store (false); cO.store (false);
    flagBits.store (0);                 // Plan 4: clear the sticky flags on a fresh run
    driftRun.store (0); blockCount.store (0);
}

void HealthMonitor::reportXrun() {
    xrunsA.fetch_add (1);
    raise (HealthFlag::Xrun);           // Plan 4: also latches cleanCapture=false (invalidating)
}

void HealthMonitor::reportDroppedFrames (long long n) {
    if (n > 0) {
        droppedA.fetch_add (n);
        clean.store (false);            // Plan 2 behavior: dropped frames invalidate the run
    }
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
    // Plan 4: raise ClipInput if the caller flagged a clip OR the peak crosses the threshold.
    if (clipL || clipR || peakL >= kClipLinear || peakR >= kClipLinear)
        raise (HealthFlag::ClipInput);  // guidance (does NOT invalidate cleanCapture)
    // Plan 4: low-level only AFTER the grace window has fully elapsed (strictly greater, so the
    // 64-block warm-up does not itself trip it) and only when both ears are quiet.
    if (blockCount.load() > kLowLevelGraceBlocks
        && peakL < kLowLevelLinear && peakR < kLowLevelLinear)
        raise (HealthFlag::LowLevel);   // guidance
}

void HealthMonitor::reportOutLevel (float peakMono, bool clipOut) {
    outM.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakMono) * 1000.0f));
    cO.store (clipOut);
    if (clipOut || peakMono >= kClipLinear)
        raise (HealthFlag::ClipOutput); // Plan 4: guidance warning
}

Health HealthMonitor::snapshot() const {
    Health h;
    h.xruns = xrunsA.load();
    h.droppedFrames = droppedA.load();
    h.fifoFill = fifoFillMilli.load() / 1000.0;
    h.captureToRenderRatio = ratioMicro.load() / 1.0e6;
    h.cleanCapture = clean.load();
    h.flags = static_cast<HealthFlag> (flagBits.load());   // Plan 4: surface sticky flags
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

// ---- Plan 4 addition: per-render-block observer (folds onto the Plan-2 setters) --------
void HealthMonitor::observeRenderBlock (int framesWanted, int framesGot,
                                        double captureToRenderRatio, double fifoFillFrac) noexcept {
    blockCount.fetch_add (1);
    setCaptureToRenderRatio (captureToRenderRatio);   // reuse the Plan-2 setter (one copy of state)
    setFifoFill (fifoFillFrac);                        // reuse the Plan-2 setter

    if (framesGot < framesWanted) {
        droppedA.fetch_add (static_cast<long long> (framesWanted - framesGot));
        raise (HealthFlag::FifoStarved);
        raise (HealthFlag::Dropout);
    }

    // Sustained drift state machine: count consecutive ratios deviating from the NOMINAL
    // capture:render ratio (not from 1.0 — a 96k->48k run has nominal ~2.0); latch at threshold.
    const double drift = (nominal_ > 0.0) ? std::abs (captureToRenderRatio / nominal_ - 1.0)
                                          : std::abs (captureToRenderRatio - 1.0);
    if (drift > kDriftRatioTol) {
        const int run = driftRun.fetch_add (1) + 1;
        if (run >= kDriftSustainBlocks) raise (HealthFlag::ExcessDrift);
    } else {
        driftRun.store (0);
    }
}

HealthFlag HealthMonitor::flags() const noexcept { return static_cast<HealthFlag> (flagBits.load()); }
bool       HealthMonitor::cleanCapture() const noexcept { return clean.load(); }

} // namespace eb
```

> The drift case `CHECK(h.snapshot().captureToRenderRatio == bad)` passes because `setCaptureToRenderRatio` stores `ratio * 1e6` rounded; `bad = 1.01` round-trips exactly at micro resolution. The dropped-frame case totals `256 - 200 = 56` because `observeRenderBlock` accounts the shortfall directly into the same `droppedA` counter the Plan-2 `reportDroppedFrames` uses.

- [ ] **Step 5: Run to verify pass**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R HealthMonitor --output-on-failure"`
Expected: **PASS** (all HealthMonitor cases).

- [ ] **Step 6: Commit**

```bash
git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "feat(plan4): full HealthMonitor with dropout/drift latches and gain warnings"
```

---

## Task 3: `CalBinder` — re-bind per-ear cal to a re-enumerated device by key + model

**Files:**
- Create: `src/audio/CalBinder.h`, `src/audio/CalBinder.cpp`
- Test: `tests/test_calbinder.cpp` (Create)
- Modify: `CMakeLists.txt` (add source to `eb_engine`), `tests/CMakeLists.txt` (add test)

Mitigates the documented "cal-drop-after-replug": the GUI/engine remembers which cal a user assigned to a device by `DeviceId::key()` (which includes name+uid and, transitively, the model via the picked `DeviceId`). On every re-enumeration, `CalBinder` re-associates the stored cal paths to the freshly enumerated `DeviceId` that matches by key, and refuses to bind if the model changed (e.g. EARS swapped for EARS Pro) so the wrong-rate FIR is never applied.

- [ ] **Step 1: Write the header**

Create `src/audio/CalBinder.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>
#include "audio/DeviceId.h"
#include <vector>
#include <optional>

namespace eb {

// Pure logic: associates per-ear cal file paths with a device by DeviceId::key(),
// surviving re-enumeration. No file I/O, no JUCE devices.
class CalBinder {
public:
    struct Binding {
        juce::String deviceKey;     // DeviceId::key()
        EarsModel    model = EarsModel::Unknown;
        juce::String leftCalPath;   // absolute path of the L cal .txt (may be empty)
        juce::String rightCalPath;  // absolute path of the R cal .txt (may be empty)
    };

    // Remember/overwrite the cal association for a device.
    void bind (const DeviceId& dev, juce::String leftCalPath, juce::String rightCalPath);

    // After re-enumeration, find the stored binding for a freshly enumerated device.
    // Returns nullopt if no binding exists, or if a binding exists but the MODEL changed
    // (stale association must not be reused with a different device class).
    std::optional<Binding> rebind (const DeviceId& reEnumerated) const;

    // True if a binding exists for this key AND the model still matches.
    bool hasValidBinding (const DeviceId& reEnumerated) const;

    void forget (const DeviceId& dev);
    void clear() { bindings.clear(); }
    int  size() const { return static_cast<int> (bindings.size()); }

private:
    std::vector<Binding> bindings;
    const Binding* find (const juce::String& key) const;
};

} // namespace eb
```

- [ ] **Step 2: Register sources**

In `CMakeLists.txt`, add `src/audio/CalBinder.cpp` to the `eb_engine` library's source list (the target Plan 2 created). In `tests/CMakeLists.txt`, add `test_calbinder.cpp` to `add_executable(eb_tests …)`.

> If `eb_engine` does not yet exist, Plan 2 has not been executed — stop and execute Plan 2 first (see the ownership note at the top). The new `.cpp` files in this plan all belong to `eb_engine`.

- [ ] **Step 3: Write the failing test**

Create `tests/test_calbinder.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/CalBinder.h"

static eb::DeviceId makeDev (juce::String name, juce::String uid, eb::EarsModel m) {
    eb::DeviceId d;
    d.typeName = "Windows Audio";
    d.name = name; d.uid = uid; d.model = m;
    return d;
}

TEST_CASE("CalBinder re-binds cal after replug when key + model match") {
    eb::CalBinder b;
    auto dev = makeDev ("EARS Pro", "usb-vid1234-pid5678", eb::EarsModel::EarsPro);
    b.bind (dev, "/cals/L_HPN.txt", "/cals/R_HPN.txt");

    // Re-enumeration yields an equal DeviceId (same name/uid -> same key()).
    auto again = makeDev ("EARS Pro", "usb-vid1234-pid5678", eb::EarsModel::EarsPro);
    REQUIRE(b.hasValidBinding (again));
    auto rb = b.rebind (again);
    REQUIRE(rb.has_value());
    CHECK(rb->leftCalPath  == juce::String ("/cals/L_HPN.txt"));
    CHECK(rb->rightCalPath == juce::String ("/cals/R_HPN.txt"));
    CHECK(rb->model == eb::EarsModel::EarsPro);
}

TEST_CASE("CalBinder refuses a stale binding when the model changed for the same key") {
    eb::CalBinder b;
    auto pro = makeDev ("EARS", "shared-uid", eb::EarsModel::EarsPro);
    b.bind (pro, "/cals/L.txt", "/cals/R.txt");
    // Same name+uid (same key) but now reports as the original EARS -> do not reuse.
    auto orig = makeDev ("EARS", "shared-uid", eb::EarsModel::Ears);
    CHECK_FALSE(b.hasValidBinding (orig));
    CHECK_FALSE(b.rebind (orig).has_value());
}

TEST_CASE("CalBinder returns nullopt for an unknown device") {
    eb::CalBinder b;
    auto dev = makeDev ("Some Mic", "uid-x", eb::EarsModel::Unknown);
    CHECK_FALSE(b.rebind (dev).has_value());
}

TEST_CASE("CalBinder bind overwrites the previous association for the same key") {
    eb::CalBinder b;
    auto dev = makeDev ("EARS", "uid-1", eb::EarsModel::Ears);
    b.bind (dev, "/old/L.txt", "/old/R.txt");
    b.bind (dev, "/new/L.txt", "/new/R.txt");
    CHECK(b.size() == 1);
    CHECK(b.rebind (dev)->leftCalPath == juce::String ("/new/L.txt"));
}

TEST_CASE("CalBinder forget removes a binding") {
    eb::CalBinder b;
    auto dev = makeDev ("EARS", "uid-1", eb::EarsModel::Ears);
    b.bind (dev, "/L.txt", "/R.txt");
    b.forget (dev);
    CHECK(b.size() == 0);
    CHECK_FALSE(b.rebind (dev).has_value());
}
```

- [ ] **Step 4: Run to verify failure**

Run: `cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"` then `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R CalBinder --output-on-failure"`
Expected: **FAIL** — `CalBinder.cpp` unimplemented (link error).

- [ ] **Step 5: Implement `CalBinder.cpp`**

Create `src/audio/CalBinder.cpp`:
```cpp
#include "audio/CalBinder.h"

namespace eb {

const CalBinder::Binding* CalBinder::find (const juce::String& key) const {
    for (auto& bnd : bindings)
        if (bnd.deviceKey == key) return &bnd;
    return nullptr;
}

void CalBinder::bind (const DeviceId& dev, juce::String leftCalPath, juce::String rightCalPath) {
    const auto key = dev.key();
    for (auto& bnd : bindings) {
        if (bnd.deviceKey == key) {
            bnd.model = dev.model;
            bnd.leftCalPath = std::move (leftCalPath);
            bnd.rightCalPath = std::move (rightCalPath);
            return;
        }
    }
    bindings.push_back (Binding { key, dev.model, std::move (leftCalPath), std::move (rightCalPath) });
}

std::optional<CalBinder::Binding> CalBinder::rebind (const DeviceId& reEnumerated) const {
    const auto* bnd = find (reEnumerated.key());
    if (bnd == nullptr) return std::nullopt;
    // Guard against a key collision where the device class changed (EARS <-> EARS Pro):
    // the FIR is designed at the model's native rate, so a stale model must not be reused.
    if (bnd->model != reEnumerated.model) return std::nullopt;
    return *bnd;
}

bool CalBinder::hasValidBinding (const DeviceId& reEnumerated) const {
    return rebind (reEnumerated).has_value();
}

void CalBinder::forget (const DeviceId& dev) {
    const auto key = dev.key();
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(),
                        [&] (const Binding& b) { return b.deviceKey == key; }),
                    bindings.end());
}

} // namespace eb
```

> `<algorithm>` is needed for `std::remove_if`; add `#include <algorithm>` at the top of `CalBinder.cpp` if your toolchain does not pull it transitively via `<vector>`.

- [ ] **Step 6: Run to verify pass**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R CalBinder --output-on-failure"`
Expected: **PASS** (all five cases).

- [ ] **Step 7: Commit**

```bash
git add src/audio/CalBinder.h src/audio/CalBinder.cpp tests/test_calbinder.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(plan4): CalBinder re-binds per-ear cal by DeviceId key incl. model"
```

---

## Task 4: `AsioFallback` — ASIO-incapability detection → WASAPI fallback (mockable)

**Files:**
- Create: `src/audio/AsioFallback.h`, `src/audio/AsioFallback.cpp`
- Test: `tests/test_asiofallback.cpp` (Create)
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Encodes spec §3.3 / §5.1: ASIO reports `hasSeparateInputsAndOutputs()==false`, so it cannot pair an EARS/EARS-Pro **capture** with a different virtual **render**. The decision is pulled into a pure function taking a tiny capability struct, so it is unit-testable without a real `AudioIODeviceType`. The engine builds that struct from the live `AudioIODeviceType` (verified call shown in the MANUAL VERIFICATION block).

- [ ] **Step 1: Write the header**

Create `src/audio/AsioFallback.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Minimal, mockable view of an AudioIODeviceType for the routing decision.
struct DeviceTypeCaps {
    juce::String typeName;                 // e.g. "ASIO", "Windows Audio" (WASAPI), "CoreAudio"
    bool         separateInputsAndOutputs = true;  // AudioIODeviceType::hasSeparateInputsAndOutputs()
};

struct RoutingDecision {
    bool         mustFallback = false;     // chosen type can't bridge two devices
    juce::String chosenTypeName;           // the type the engine should actually use
    juce::String message;                  // user-facing explanation ("" if no fallback)
};

// Given the preferred type and the available types, decide whether to fall back to a
// cross-device-capable type (WASAPI on Windows, CoreAudio on macOS). Pure; no devices.
class AsioFallback {
public:
    // The cross-device-capable type names we accept, in preference order.
    static juce::StringArray bridgeCapableTypeNames();

    static RoutingDecision decide (const DeviceTypeCaps& preferred,
                                   const juce::Array<DeviceTypeCaps>& available);
};

} // namespace eb
```

- [ ] **Step 2: Register sources**

Add `src/audio/AsioFallback.cpp` to `eb_engine` in `CMakeLists.txt`; add `test_asiofallback.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 3: Write the failing test**

Create `tests/test_asiofallback.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/AsioFallback.h"

static eb::DeviceTypeCaps caps (juce::String name, bool sep) { return { name, sep }; }

TEST_CASE("AsioFallback: ASIO (no separate I/O) forces a WASAPI fallback") {
    juce::Array<eb::DeviceTypeCaps> avail {
        caps ("ASIO", false),
        caps ("Windows Audio", true),            // WASAPI
        caps ("DirectSound", true)
    };
    auto d = eb::AsioFallback::decide (caps ("ASIO", false), avail);
    CHECK(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("Windows Audio"));
    CHECK(d.message.isNotEmpty());
    CHECK(d.message.containsIgnoreCase ("ASIO"));
}

TEST_CASE("AsioFallback: WASAPI is kept as-is (no fallback)") {
    juce::Array<eb::DeviceTypeCaps> avail {
        caps ("Windows Audio", true), caps ("ASIO", false)
    };
    auto d = eb::AsioFallback::decide (caps ("Windows Audio", true), avail);
    CHECK_FALSE(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("Windows Audio"));
    CHECK(d.message.isEmpty());
}

TEST_CASE("AsioFallback: CoreAudio is kept as-is on macOS") {
    juce::Array<eb::DeviceTypeCaps> avail { caps ("CoreAudio", true) };
    auto d = eb::AsioFallback::decide (caps ("CoreAudio", true), avail);
    CHECK_FALSE(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("CoreAudio"));
}

TEST_CASE("AsioFallback: any type lacking separate I/O triggers a fallback even if not named ASIO") {
    juce::Array<eb::DeviceTypeCaps> avail {
        caps ("WeirdDriver", false), caps ("Windows Audio", true)
    };
    auto d = eb::AsioFallback::decide (caps ("WeirdDriver", false), avail);
    CHECK(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("Windows Audio"));
}

TEST_CASE("AsioFallback: fallback with no bridge-capable type available keeps the choice but warns") {
    juce::Array<eb::DeviceTypeCaps> avail { caps ("ASIO", false) };
    auto d = eb::AsioFallback::decide (caps ("ASIO", false), avail);
    CHECK(d.mustFallback);
    // No WASAPI/CoreAudio present: chosenTypeName stays empty, message explains the problem.
    CHECK(d.chosenTypeName.isEmpty());
    CHECK(d.message.isNotEmpty());
}
```

- [ ] **Step 4: Run to verify failure**

Run: `cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"` then `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R AsioFallback --output-on-failure"`
Expected: **FAIL** — unimplemented.

- [ ] **Step 5: Implement `AsioFallback.cpp`**

Create `src/audio/AsioFallback.cpp`:
```cpp
#include "audio/AsioFallback.h"

namespace eb {

juce::StringArray AsioFallback::bridgeCapableTypeNames() {
    // WASAPI is exposed by JUCE under the type name "Windows Audio";
    // macOS CoreAudio is "CoreAudio". Both have hasSeparateInputsAndOutputs()==true.
    return { "Windows Audio", "CoreAudio" };
}

RoutingDecision AsioFallback::decide (const DeviceTypeCaps& preferred,
                                      const juce::Array<DeviceTypeCaps>& available) {
    RoutingDecision out;

    if (preferred.separateInputsAndOutputs) {
        out.mustFallback = false;
        out.chosenTypeName = preferred.typeName;
        return out;   // already bridge-capable; keep it
    }

    out.mustFallback = true;

    // Pick the first available bridge-capable type, in our preference order.
    for (auto& wanted : bridgeCapableTypeNames()) {
        for (auto& cap : available) {
            if (cap.typeName == wanted && cap.separateInputsAndOutputs) {
                out.chosenTypeName = cap.typeName;
                out.message =
                    "The '" + preferred.typeName + "' driver cannot route a capture device to a "
                    "different output (ASIO has no separate inputs/outputs). Falling back to '"
                    + cap.typeName + "'. The EARS Pro vendor ASIO driver must not be used here.";
                return out;
            }
        }
    }

    // No bridge-capable type present.
    out.chosenTypeName = juce::String();
    out.message =
        "The selected '" + preferred.typeName + "' driver cannot bridge two devices, and no "
        "WASAPI/CoreAudio driver was found. Install/enable the platform's shared audio driver.";
    return out;
}

} // namespace eb
```

- [ ] **Step 6: Run to verify pass**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R AsioFallback --output-on-failure"`
Expected: **PASS** (all five cases).

- [ ] **Step 7: Commit**

```bash
git add src/audio/AsioFallback.h src/audio/AsioFallback.cpp tests/test_asiofallback.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(plan4): ASIO-incapability detection with WASAPI/CoreAudio fallback decision"
```

---

## Task 5: `LrVerify` — one-time physical L/R verification state machine

**Files:**
- Create: `src/audio/LrVerify.h`, `src/audio/LrVerify.cpp`
- Test: `tests/test_lrverify.cpp` (Create)
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Spec §5.6 / §8.1: a one-time step where the user drives a signal into **one** earcup; the routine confirms the expected mic channel responds (and the other stays quiet), so a silent channel swap can't apply the wrong cal to each ear. The decision logic (which channel is "live", whether it matches the channel under test, pass/fail/ambiguous) is pure and unit-tested; the actual tone playback + capture is wired by the engine/GUI (MANUAL VERIFICATION block).

- [ ] **Step 1: Write the header**

Create `src/audio/LrVerify.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>

namespace eb {

enum class Ear { Left = 0, Right = 1 };

enum class LrResult {
    Pending,        // not enough evidence yet
    Pass,           // the channel under test is the one that responded
    Swapped,        // the OTHER channel responded -> channels are physically swapped
    Ambiguous,      // both channels responded (crosstalk/leak) or neither did
    Silent          // neither channel responded above the floor
};

// Pure state machine. Caller feeds per-block peak levels for both mic channels while a tone is
// driven into ONE earcup (the "ear under test"); the machine accumulates evidence and decides.
class LrVerify {
public:
    static constexpr float  kActiveLinear   = 0.02f;  // ~ -34 dBFS: a channel is "responding"
    static constexpr double kRatioToConfirm  = 4.0;    // live ear must be >= 4x the other to be unambiguous
    static constexpr int    kBlocksToConfirm = 16;     // sustained blocks before deciding

    void begin (Ear earUnderTest) noexcept;            // resets accumulators
    void observe (float peakLeft, float peakRight) noexcept;
    LrResult result() const noexcept;
    Ear      earUnderTest() const noexcept { return tested; }
    bool     isComplete() const noexcept { return result() != LrResult::Pending; }
    void     reset() noexcept;

private:
    Ear    tested = Ear::Left;
    int    blocks = 0;
    double sumLeft = 0.0, sumRight = 0.0;   // accumulated peak energy proxy
    int    activeBlocksLeft = 0, activeBlocksRight = 0;
};

} // namespace eb
```

- [ ] **Step 2: Register sources**

Add `src/audio/LrVerify.cpp` to `eb_engine`; add `test_lrverify.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 3: Write the failing test**

Create `tests/test_lrverify.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/LrVerify.h"

static void feed (eb::LrVerify& v, float l, float r, int n) {
    for (int i = 0; i < n; ++i) v.observe (l, r);
}

TEST_CASE("LrVerify: tone into LEFT, left mic responds -> Pass") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.30f, 0.005f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.isComplete());
    CHECK(v.result() == eb::LrResult::Pass);
}

TEST_CASE("LrVerify: tone into LEFT but RIGHT mic responds -> Swapped") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.005f, 0.30f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Swapped);
}

TEST_CASE("LrVerify: both channels loud -> Ambiguous") {
    eb::LrVerify v; v.begin (eb::Ear::Right);
    feed (v, 0.30f, 0.28f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Ambiguous);
}

TEST_CASE("LrVerify: neither channel responds -> Silent") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.001f, 0.001f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Silent);
}

TEST_CASE("LrVerify: not enough blocks -> Pending") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.30f, 0.005f, eb::LrVerify::kBlocksToConfirm - 1);
    CHECK(v.result() == eb::LrResult::Pending);
    CHECK_FALSE(v.isComplete());
}

TEST_CASE("LrVerify: tone into RIGHT, right mic responds -> Pass") {
    eb::LrVerify v; v.begin (eb::Ear::Right);
    feed (v, 0.004f, 0.40f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Pass);
}

TEST_CASE("LrVerify: reset clears prior evidence") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.30f, 0.005f, eb::LrVerify::kBlocksToConfirm);
    REQUIRE(v.result() == eb::LrResult::Pass);
    v.reset();
    CHECK(v.result() == eb::LrResult::Pending);
}
```

- [ ] **Step 4: Run to verify failure**

Run: `cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"` then `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R LrVerify --output-on-failure"`
Expected: **FAIL** — unimplemented.

- [ ] **Step 5: Implement `LrVerify.cpp`**

Create `src/audio/LrVerify.cpp`:
```cpp
#include "audio/LrVerify.h"

namespace eb {

void LrVerify::begin (Ear earUnderTest) noexcept {
    tested = earUnderTest;
    reset();
    tested = earUnderTest;   // reset() leaves tested at Left; restore the requested ear
}

void LrVerify::reset() noexcept {
    tested = Ear::Left;
    blocks = 0;
    sumLeft = sumRight = 0.0;
    activeBlocksLeft = activeBlocksRight = 0;
}

void LrVerify::observe (float peakLeft, float peakRight) noexcept {
    ++blocks;
    sumLeft  += static_cast<double> (peakLeft);
    sumRight += static_cast<double> (peakRight);
    if (peakLeft  >= kActiveLinear) ++activeBlocksLeft;
    if (peakRight >= kActiveLinear) ++activeBlocksRight;
}

LrResult LrVerify::result() const noexcept {
    if (blocks < kBlocksToConfirm) return LrResult::Pending;

    const bool leftActive  = activeBlocksLeft  >= kBlocksToConfirm / 2;
    const bool rightActive = activeBlocksRight >= kBlocksToConfirm / 2;

    if (! leftActive && ! rightActive) return LrResult::Silent;

    // Both clearly active without a dominant channel -> ambiguous (crosstalk / both driven).
    const double eps = 1.0e-9;
    const double lr  = sumLeft  / (sumRight + eps);
    const double rl  = sumRight / (sumLeft  + eps);

    if (leftActive && rightActive && lr < kRatioToConfirm && rl < kRatioToConfirm)
        return LrResult::Ambiguous;

    // Determine which channel dominates.
    const Ear dominant = (sumLeft >= sumRight) ? Ear::Left : Ear::Right;
    const double dominance = (dominant == Ear::Left) ? lr : rl;
    if (dominance < kRatioToConfirm) return LrResult::Ambiguous;

    return (dominant == tested) ? LrResult::Pass : LrResult::Swapped;
}

} // namespace eb
```

- [ ] **Step 6: Run to verify pass**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"` then `cmd /c "tools\dev.cmd ctest --test-dir build -R LrVerify --output-on-failure"`
Expected: **PASS** (all seven cases).

- [ ] **Step 7: Commit**

```bash
git add src/audio/LrVerify.h src/audio/LrVerify.cpp tests/test_lrverify.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(plan4): one-time physical L/R verification state machine"
```

---

## Task 6: Wire health + L/R verify + ASIO fallback into `AudioEngine`

**Files:**
- Modify: `src/audio/AudioEngine.h`, `src/audio/AudioEngine.cpp`

Connect the new units to the facade without breaking the spine. `AudioEngine` **already owns a single `HealthMonitor` member named `hm`** (Plan 2) — this task instruments that SAME `hm` with the now-full monitor (Task 2 extended it in place) and **adds no second monitor**. The Plan-2 capture/render callbacks already feed `hm` (`reportInLevels`/`reportOutLevel`); this task adds the one new `hm.observeRenderBlock(...)` call in the render callback and the `start()`-time `hm.prepare(...)`. The ASIO-fallback path uses the Plan-2 `DeviceManager` seam (`availableTypeCaps()`/`currentTypeCaps()`/`setCurrentType()`). All identifiers below map to real Plan-2 declarations.

- [ ] **Step 1: Add the additive public surface + members to `AudioEngine.h`**

In `src/audio/AudioEngine.h`, add these **after** the spine `health()` declaration (keep all spine methods exactly as in the contract above; do NOT touch the existing `HealthMonitor hm;` member):
```cpp
    // --- Plan 4 additions (additive) ---
    // Detailed sticky health flags for the GUI status light / tooltip (reads the same `hm`).
    HealthFlag     healthFlags() const noexcept;
    bool           cleanCapture() const noexcept;
    DipGainProfile gainProfile() const noexcept;   // for the "lower/raise DIP gain" hint

    // Drive a short tone into ONE earcup (user routes playback to that earcup) and report which
    // mic channel responded. Runs only while Stopped; uses a dedicated short capture-only open.
    // Returns the verdict; the GUI presents it. Non-blocking variant: begin + poll.
    void     beginLrVerify (Ear earUnderTest);
    LrResult lrVerifyResult() const noexcept;
    bool     lrVerifyComplete() const noexcept;

    // Last ASIO->WASAPI/CoreAudio fallback message (empty when no fallback occurred).
    juce::String lastFallbackMessage() const { return lastFallbackMessage_; }
```
Add these includes near the existing engine includes (`HealthMonitor.h` is already included by Plan 2):
```cpp
#include "audio/LrVerify.h"
#include "audio/AsioFallback.h"
#include "audio/CalBinder.h"
```
Add these **additive** members to the private section — but **NOT** another `HealthMonitor`; the engine keeps its single Plan-2 `HealthMonitor hm;`:
```cpp
    LrVerify      lrVerify_;        // Plan 4 (pure state machine)
    CalBinder     calBinder_;       // Plan 4 (re-bind cal across re-enumeration)
    juce::String  lastFallbackMessage_;   // surfaced by the GUI after an ASIO fallback
    int           lastUnderruns_ = 0;     // render-thread-only: ClockBridge underrun count seen last block
    bool          usingAggregate_ = false; // macOS: true when the CoreAudio aggregate path is active
                                           // (Task 7 sets it; the render callback reads it). Always
                                           // false on Windows.
```

- [ ] **Step 2: Capture callback — feed the SAME values to the extended monitor (no second loop)**

Plan 2's capture callback already computes the input peaks (`pkL`/`pkR`) and calls
`e.hm.reportInLevels (pkL, pkR, pkL >= 0.999f, pkR >= 0.999f)`. The extended `HealthMonitor`
(Task 2) folds ClipInput/LowLevel detection INTO `reportInLevels`, so **no new peak loop and no
new call are needed here** — the existing `reportInLevels` already drives the flags. Confirm by
inspection that the Plan-2 capture body is unchanged:
```cpp
// (Plan 2, unchanged) — peaks computed once, fed once to the single monitor `hm`:
//   const float* l = in[0]; const float* r = in[1];
//   float pkL = 0, pkR = 0;
//   for (int i = 0; i < numSamples; ++i) { pkL = juce::jmax (pkL, std::abs (l[i])); pkR = juce::jmax (pkR, std::abs (r[i])); }
//   e.hm.reportInLevels (pkL, pkR, pkL >= 0.999f, pkR >= 0.999f);   // raises ClipInput/LowLevel internally
//   e.graph.process (l, r, mono.data(), numSamples);
//   e.bridge.pushCapture (mono.data(), numSamples);
```

- [ ] **Step 3: Render callback — capture the pulled-frame count and add the one new observer**

Plan 2's render callback currently calls `e.bridge.pullRender (mono.data(), numSamples)` and discards the return, computes `pk`, calls `e.hm.reportOutLevel (pk, pk >= 0.999f)`, duplicates `mono` to both channels, then `e.hm.setFifoFill (e.bridge.fifoFill())`. Edit it to (a) capture the returned frame count, (b) replace the standalone `setFifoFill` with the new `observeRenderBlock` (which forwards fill+ratio AND adds dropout/drift), and (c) zero the tail on starvation. The body becomes:
```cpp
void audioDeviceIOCallbackWithContext (const float* const* /*in*/, int /*numIn*/,
                                       float* const* out, int numOut,
                                       int numSamples,
                                       const juce::AudioIODeviceCallbackContext&) override {
    if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
    const int got = e.bridge.pullRender (mono.data(), numSamples);   // capture frames written
    float pk = 0;
    for (int i = 0; i < got; ++i) pk = juce::jmax (pk, std::abs (mono[i]));
    e.hm.reportOutLevel (pk, pk >= 0.999f);                          // Plan 2 method (raises ClipOutput)
    // pullRender ALWAYS returns numSamples (it zero-pads on starvation), so `got` alone never signals
    // a shortfall. The LIVE starvation signal is the ClockBridge underrun delta: a NEW underrun this
    // block means the FIFO starved and the interpolator zero-padded -> report 0 delivered so
    // observeRenderBlock raises FifoStarved+Dropout and invalidates cleanCapture (the Plan-2 gap).
    const int und    = e.bridge.underruns();
    const int effGot = (und > e.lastUnderruns_) ? 0 : got;
    e.lastUnderruns_ = und;
    // One new additive observer: forwards ratio+fill to the Plan-2 setters AND adds dropout/drift.
    // On the macOS aggregate path (Task 7) the ClockBridge is bypassed, so report a nominal ratio.
    if (e.usingAggregate_) e.hm.observeRenderBlock (numSamples, numSamples, 1.0, 0.5);
    else                   e.hm.observeRenderBlock (numSamples, effGot, e.bridge.currentRatio(), e.bridge.fifoFill());
    for (int ch = 0; ch < numOut; ++ch)                             // duplicate mono to both channels
        if (out[ch] != nullptr) {
            for (int i = 0; i < got; ++i)        out[ch][i] = mono[i];
            for (int i = got; i < numSamples; ++i) out[ch][i] = 0.0f;   // silence-fill on starvation
        }
}
```
> `e.bridge.currentRatio()` is Plan 2's `ClockBridge::currentRatio()` accessor (the trimmed
> capture:render resample ratio from the PI control loop — added to Plan 2 Task 3). On the macOS
> aggregate path (Task 7) the `ClockBridge` is bypassed; there the engine calls
> `e.hm.observeRenderBlock (numSamples, numSamples, 1.0, 0.5)` so the OS-handled drift reports a
> nominal in-tolerance ratio.

- [ ] **Step 4: Implement the additive accessors + L/R verify glue (all read the single `hm`)**

Append to `AudioEngine.cpp`:
```cpp
HealthFlag AudioEngine::healthFlags() const noexcept     { return hm.flags(); }
bool       AudioEngine::cleanCapture() const noexcept    { return hm.cleanCapture(); }
DipGainProfile AudioEngine::gainProfile() const noexcept { return hm.gainProfile(); }

void AudioEngine::beginLrVerify (Ear earUnderTest) {
    jassert (status() == EngineStatus::Stopped);   // verification runs only while stopped
    lrVerify_.begin (earUnderTest);
    // Engine opens a short capture-only stream and, per block, calls:
    //   lrVerify_.observe (peakL, peakR);
    // until lrVerify_.isComplete(); see the MANUAL VERIFICATION block for the wiring contract.
}
LrResult AudioEngine::lrVerifyResult() const noexcept   { return lrVerify_.result(); }
bool     AudioEngine::lrVerifyComplete() const noexcept { return lrVerify_.isComplete(); }
```

Also EDIT the existing Plan-2 `AudioEngine::levels()` and `AudioEngine::health()` bodies (do not add
a second monitor). `levels()` is already `return hm.levels();` — leave it. For `health()`, the
extended `hm` now owns `fifoFill`, `captureToRenderRatio`, `cleanCapture`, and `flags` (the render
callback feeds them via `observeRenderBlock`), so drop the manual overwrites and return the snapshot
directly so `flags`/`cleanCapture` propagate:
```cpp
Health AudioEngine::health() const {
    // The single monitor `hm` now carries fifoFill + captureToRenderRatio (from observeRenderBlock),
    // plus the sticky flags and cleanCapture. Return it directly — no manual overwrite.
    return hm.snapshot();
}
```

- [ ] **Step 5: Reset health + apply ASIO fallback in `start()` (real Plan-2 names)**

In `AudioEngine::start(juce::String& errorOut)`, the existing Plan-2 body already calls
`hm.reset();` first and computes `const int cap = juce::jmax (4096, juce::nextPowerOfTwo ((int) (capRate * 0.25)));`
before `bridge.prepare(...)`. Two edits:

(a) Replace the bare `hm.reset();` with a model-aware `hm.prepare(...)`. Because `cap` is computed
later in the body (after the devices are opened), move the prepare to right after `cap` is known —
i.e. immediately after the existing `bridge.prepare (capRate, renRate, 1, cap);` line:
```cpp
// (existing) bridge.prepare (capRate, renRate, 1, cap);
hm.prepare (inputId.model, cap, capRate / juce::jmax (1.0, renRate));   // reset + size + NOMINAL ratio (drift detection)
```
(Delete the now-redundant standalone `hm.reset();` at the top of `start()` — `prepare()` resets.)

(b) Add the ASIO-incapability guard at the TOP of `start()` (before opening devices), built from
the Plan-2 `DeviceManager` seam (`availableTypeCaps()`/`currentTypeCaps()`/`setCurrentType()`):
```cpp
// ASIO-incapability guard: ASIO cannot pair an EARS capture with a different virtual render.
// Build the decision from the DeviceManager's type capabilities and fall back if needed.
{
    auto cur = devices.currentTypeCaps();                       // DeviceManager::TypeCaps
    eb::DeviceTypeCaps preferred { cur.typeName, cur.separateInputsAndOutputs };
    juce::Array<eb::DeviceTypeCaps> available;
    for (auto& c : devices.availableTypeCaps())
        available.add ({ c.typeName, c.separateInputsAndOutputs });
    auto decision = eb::AsioFallback::decide (preferred, available);
    if (decision.mustFallback) {
        if (decision.chosenTypeName.isEmpty()) {
            errorOut = decision.message;
            engineStatus.store ((int) EngineStatus::Error);
            return false;
        }
        devices.setCurrentType (decision.chosenTypeName);       // honored by the next open
        lastFallbackMessage_ = decision.message;                // surfaced by the GUI
    } else {
        lastFallbackMessage_.clear();
    }
}
```

- [ ] **Step 6: Build (no new unit test — covered by Tasks 2/4/5 + MANUAL VERIFICATION)**

Run: `cmd /c "tools\dev.cmd cmake --build build --target eb_tests"`
Expected: builds clean. (Also build the app: `cmd /c "tools\dev.cmd cmake --build build --target EarsBridge"`.)

> **MANUAL VERIFICATION — engine wiring (no CI; real device required).**
> Hardware: an EARS or EARS Pro on USB, plus an installed virtual cable (VB-CABLE / BlackHole).
> 1. Launch the app, select the EARS input and the virtual-cable output, press **Start**.
>    **PASS:** `status()==Running`; the output meter moves when you tap an EARS earcup.
> 2. Tap/scratch the **left** earcup hard enough to clip.
>    **PASS:** `healthFlags()` contains `ClipInput`; `levels().clipL==true`; the GUI shows "lower DIP gain (range 0–36 dB EARS / 0–45 dB EARS Pro)". `cleanCapture()` stays **true** (clip is guidance, not invalidation).
> 3. Yank the EARS USB mid-run.
>    **PASS:** the engine stops cleanly, surfaces a disconnect, and on re-plug re-binds the cal via `CalBinder` (the cal slots stay populated; see Task 3). `healthFlags()` may contain `Xrun`/`Dropout`; `cleanCapture()==false`.
> 4. Stop, press **Verify L/R**, choose **Left**, and (as instructed) route a tone to the **left** earcup only.
>    **PASS:** `lrVerifyComplete()` becomes true within ~1 s and `lrVerifyResult()==LrResult::Pass`. Repeat routing to the **right** earcup with **Left** still selected → `LrResult::Swapped`. (This proves the channel map ch0→L, ch1→R.)
> 5. If the system's default audio type is ASIO, confirm the engine logged the fallback message and is running on "Windows Audio" (WASAPI). **PASS:** `start()` succeeds and `lastFallbackMessage()` mentions ASIO.

- [ ] **Step 7: Commit**

```bash
git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp
git commit -m "feat(plan4): wire full HealthMonitor, L/R verify, and ASIO fallback into AudioEngine"
```

---

## Task 7: macOS CoreAudio Aggregate Device (`AggregateDevice_mac.{h,mm}`)

**Files:**
- Create: `src/platform/AggregateDevice_mac.h`, `src/platform/AggregateDevice_mac.mm`
- Modify: `CMakeLists.txt` (macOS-only source + frameworks)

Spec §4 / §13.5: on macOS, **prefer** a CoreAudio Aggregate Device (EARS + virtual device) with *Drift Correction ON* and *Clock Source = EARS*, so the OS resamples; fall back to the Plan-2 `ClockBridge` FIFO+ASRC if the aggregate can't be created. This file owns the full lifecycle (create → query UID → teardown). It compiles **only** on macOS; on Windows it is excluded from the build. Pure C++ header so the engine can reference it portably.

> Grounded APIs (Apple CoreAudio / AudioToolbox): `AudioHardwareCreateAggregateDevice(CFDictionaryRef, AudioDeviceID*) -> OSStatus`, `AudioHardwareDestroyAggregateDevice(AudioDeviceID) -> OSStatus`, dictionary keys `kAudioAggregateDeviceNameKey`, `kAudioAggregateDeviceUIDKey`, `kAudioAggregateDeviceIsPrivateKey`, `kAudioAggregateDeviceSubDeviceListKey`, `kAudioSubDeviceUIDKey`, `kAudioSubDeviceDriftCompensationKey`, `kAudioAggregateDeviceMainSubDeviceKey` (clock master), and `kAudioDevicePropertyDeviceUID` to fetch a sub-device UID. Re-verify against the SDK headers on execution.

- [ ] **Step 1: Write the portable header**

Create `src/platform/AggregateDevice_mac.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Owns a CoreAudio aggregate device wrapping the EARS input + a virtual output, with drift
// correction on and the EARS as clock master. macOS only; on other platforms create() returns
// false and the engine falls back to the ClockBridge FIFO+ASRC path.
class AggregateDevice {
public:
    AggregateDevice() = default;
    ~AggregateDevice();                            // tears down if still alive

    AggregateDevice (const AggregateDevice&) = delete;
    AggregateDevice& operator= (const AggregateDevice&) = delete;

    // Create an aggregate from the two sub-device CoreAudio UIDs (as reported by the OS / JUCE).
    // earsInputUid is the clock master. Returns false on any platform != macOS or on failure;
    // errorOut carries the reason. On success, aggregateUid() is the UID to open with JUCE.
    bool create (const juce::String& earsInputUid,
                 const juce::String& virtualOutputUid,
                 juce::String& errorOut);

    void destroy();                                // idempotent

    bool         isValid() const noexcept { return valid; }
    juce::String aggregateUid() const     { return uid; }

private:
    bool         valid = false;
    juce::String uid;          // generated aggregate UID, also the open key for JUCE
    unsigned     deviceId = 0; // AudioDeviceID as unsigned (avoid CoreAudio types in the header)
};

} // namespace eb
```

- [ ] **Step 2: Write the Objective-C++ implementation**

Create `src/platform/AggregateDevice_mac.mm`:
```objc
#include "platform/AggregateDevice_mac.h"

#if JUCE_MAC
 #include <CoreAudio/CoreAudio.h>
 #include <CoreAudio/AudioHardware.h>
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace eb {

#if JUCE_MAC

// Look up an AudioDeviceID from its UID string (the JUCE device "uid").
static AudioDeviceID deviceIdForUid (const juce::String& uidStr, bool& ok) {
    ok = false;
    CFStringRef cfUid = CFStringCreateWithCString (kCFAllocatorDefault,
                                                   uidStr.toRawUTF8(), kCFStringEncodingUTF8);
    if (cfUid == nullptr) return kAudioObjectUnknown;

    AudioValueTranslation tr;
    AudioDeviceID dev = kAudioObjectUnknown;
    tr.mInputData = &cfUid;  tr.mInputDataSize  = sizeof (cfUid);
    tr.mOutputData = &dev;   tr.mOutputDataSize = sizeof (dev);

    AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyDeviceForUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof (tr);
    OSStatus st = AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &size, &tr);
    CFRelease (cfUid);
    ok = (st == noErr && dev != kAudioObjectUnknown);
    return dev;
}

static CFDictionaryRef makeSubDeviceDict (const juce::String& uid, bool driftCorrect) {
    CFStringRef cfUid = CFStringCreateWithCString (kCFAllocatorDefault,
                                                   uid.toRawUTF8(), kCFStringEncodingUTF8);
    int drift = driftCorrect ? 1 : 0;
    CFNumberRef cfDrift = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &drift);

    const void* keys[]   = { CFSTR (kAudioSubDeviceUIDKey), CFSTR (kAudioSubDeviceDriftCompensationKey) };
    const void* values[] = { cfUid, cfDrift };
    CFDictionaryRef dict = CFDictionaryCreate (kCFAllocatorDefault, keys, values, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease (cfUid); CFRelease (cfDrift);
    return dict;
}

bool AggregateDevice::create (const juce::String& earsInputUid,
                              const juce::String& virtualOutputUid,
                              juce::String& errorOut) {
    destroy();   // ensure no prior aggregate leaks

    // Build the sub-device list: EARS (clock master) first, then the virtual output. Drift ON for both.
    CFDictionaryRef earsSub = makeSubDeviceDict (earsInputUid, true);
    CFDictionaryRef virtSub = makeSubDeviceDict (virtualOutputUid, true);
    const void* subDevices[] = { earsSub, virtSub };
    CFArrayRef subList = CFArrayCreate (kCFAllocatorDefault, subDevices, 2, &kCFTypeArrayCallBacks);

    // Aggregate description dictionary.
    juce::String aggUid = "eb.aggregate." + juce::String (juce::Time::currentTimeMillis());
    CFStringRef cfName = CFSTR ("EARS Bridge Aggregate");
    CFStringRef cfUid  = CFStringCreateWithCString (kCFAllocatorDefault,
                                                    aggUid.toRawUTF8(), kCFStringEncodingUTF8);
    CFStringRef cfMaster = CFStringCreateWithCString (kCFAllocatorDefault,
                                                      earsInputUid.toRawUTF8(), kCFStringEncodingUTF8);
    int isPrivate = 1;  // private: not shown in Audio MIDI Setup, torn down with the process
    CFNumberRef cfPrivate = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &isPrivate);

    const void* keys[] = {
        CFSTR (kAudioAggregateDeviceNameKey),
        CFSTR (kAudioAggregateDeviceUIDKey),
        CFSTR (kAudioAggregateDeviceIsPrivateKey),
        CFSTR (kAudioAggregateDeviceSubDeviceListKey),
        CFSTR (kAudioAggregateDeviceMainSubDeviceKey)   // clock master = EARS
    };
    const void* values[] = { cfName, cfUid, cfPrivate, subList, cfMaster };
    CFDictionaryRef desc = CFDictionaryCreate (kCFAllocatorDefault, keys, values, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);

    AudioDeviceID newId = kAudioObjectUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice (desc, &newId);

    CFRelease (desc); CFRelease (subList);
    CFRelease (earsSub); CFRelease (virtSub);
    CFRelease (cfUid); CFRelease (cfMaster); CFRelease (cfPrivate);

    if (st != noErr || newId == kAudioObjectUnknown) {
        errorOut = "AudioHardwareCreateAggregateDevice failed (OSStatus " + juce::String ((int) st) + ")";
        valid = false;
        return false;
    }

    deviceId = (unsigned) newId;
    uid = aggUid;
    valid = true;
    return true;
}

void AggregateDevice::destroy() {
    if (! valid) return;
    AudioHardwareDestroyAggregateDevice ((AudioDeviceID) deviceId);
    valid = false; deviceId = 0; uid = {};
}

#else  // not macOS

bool AggregateDevice::create (const juce::String&, const juce::String&, juce::String& errorOut) {
    errorOut = "Aggregate device is macOS-only";
    return false;
}
void AggregateDevice::destroy() { valid = false; }

#endif

AggregateDevice::~AggregateDevice() { destroy(); }

} // namespace eb
```

> `deviceIdForUid` is provided for diagnostics/future use; the create path uses sub-device UIDs directly (CoreAudio resolves them). On execution, confirm the exact key spelling `kAudioAggregateDeviceMainSubDeviceKey` vs the older `kAudioAggregateDeviceMasterSubDeviceKey` in your SDK — Apple renamed "Master"→"Main"; both refer to the clock master. Use whichever the SDK header declares.

- [ ] **Step 3: Add the macOS source + frameworks to CMake**

In `CMakeLists.txt`, after the `eb_engine` target is defined (Plan 2), append:
```cmake
# ---- macOS-only: CoreAudio aggregate device ----
if(APPLE)
    target_sources(eb_engine PRIVATE src/platform/AggregateDevice_mac.mm)
    set_source_files_properties(src/platform/AggregateDevice_mac.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
    target_link_libraries(eb_engine PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation")
endif()
```
The header `src/platform/AggregateDevice_mac.h` is portable and is included by `AudioEngine` on all platforms; only the `.mm` is macOS-gated.

- [ ] **Step 4: Make the engine prefer the aggregate on macOS**

Use the real Plan-2 member names: `inputId`/`outputId` (the engine's selected `DeviceId`s) and
`devices` (the `DeviceManager`). At the TOP of `AudioEngine::start()` (after the ASIO guard, before
the Plan-2 two-device open), insert the macOS-preferred aggregate branch guarded by `#if JUCE_MAC`:
```cpp
#if JUCE_MAC
    usingAggregate_ = false;
    juce::String aggErr;
    if (aggregate_.create (inputId.uid, outputId.uid, aggErr)) {
        // The OS handles capture<->render resampling with the EARS as clock master, so the
        // ClockBridge is bypassed; the engine reports a nominal ratio in observeRenderBlock.
        // Open the single private aggregate device (by its UID) for BOTH capture and render.
        const auto aggName = aggregate_.aggregateUid();
        eb::DeviceId aggDev; aggDev.typeName = "CoreAudio"; aggDev.name = aggName; aggDev.uid = aggName;
        auto eIn  = devices.openInput  (aggDev, activeRate, 512);
        auto eOut = devices.openOutput (aggDev, activeRate, 512, outputBits);
        if (eIn.isEmpty() && eOut.isEmpty()) {
            usingAggregate_ = true;   // proceed with the aggregate; skip the ClockBridge prepare below
        } else {
            devices.closeAll();
            aggregate_.destroy();     // fall through to the ClockBridge path on a partial open
        }
    }
    // If usingAggregate_ stayed false, fall through to the Plan-2 two-device open + ClockBridge path.
#endif
```
Add to `AudioEngine`:
- `#include "platform/AggregateDevice_mac.h"` (portable header; macOS-gated `.mm`);
- a member `AggregateDevice aggregate_;`.

(`usingAggregate_` is already declared in Task 6 Step 1; this task only sets it.) The render callback
(Task 6 Step 3) already branches on `usingAggregate_`: when true the `ClockBridge` is bypassed and it
reports a nominal ratio via `observeRenderBlock (numSamples, got, 1.0, 0.5)`. In `stop()`, call
`aggregate_.destroy();` (idempotent) so teardown always runs, and reset `usingAggregate_ = false;`.
Guard the existing Plan-2 `bridge.prepare(...)` in `start()` so it is skipped when `usingAggregate_`
is true.

- [ ] **Step 5: Build on Windows (verifies the portable header + CMake guard don't break the Windows build)**

Run: `cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"` then `cmd /c "tools\dev.cmd cmake --build build --target EarsBridge"`
Expected: builds clean (the `.mm` is excluded by `if(APPLE)`; the header compiles as plain C++ with the `#else` stubs).

> **MANUAL VERIFICATION — macOS aggregate device (no CI; macOS + hardware required).**
> Hardware: EARS/EARS Pro on USB; BlackHole 2ch installed; on a Mac with Xcode + Ninja.
> Build: `cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target EarsBridge`.
> 1. Launch, select EARS input + BlackHole 2ch output, **Start**.
>    **PASS:** the app reports it is using the **aggregate** path (`usingAggregate_==true`); a private aggregate "EARS Bridge Aggregate" is **not** visible in Audio MIDI Setup (because `IsPrivate=1`), and audio flows EARS→BlackHole with the output meter moving on an earcup tap.
> 2. Run a 5-minute continuous capture (a long sweep loop).
>    **PASS:** no progressive latency growth and no silence after 8–50 min (the JUCE `AudioIODeviceCombiner` failure mode is avoided because the OS does drift correction with the EARS as clock master). `cleanCapture()` stays true; `droppedFrames==0`.
> 3. Force a create failure (rename/disable BlackHole so the UID can't resolve), **Start** again.
>    **PASS:** `aggregate_.create(...)` returns false with a non-empty `aggErr`; the engine logs it and falls back to the ClockBridge FIFO+ASRC path; audio still flows (with the app-side ASRC).
> 4. **Stop**, then quit the app.
>    **PASS:** `AudioHardwareDestroyAggregateDevice` runs (the private aggregate disappears immediately; verify with `system_profiler SPAudioDataType` showing no leftover "EARS Bridge Aggregate"). Re-Start/Stop 10× leaves no accumulated aggregates.

- [ ] **Step 6: Commit**

```bash
git add src/platform/AggregateDevice_mac.h src/platform/AggregateDevice_mac.mm src/audio/AudioEngine.h src/audio/AudioEngine.cpp CMakeLists.txt
git commit -m "feat(plan4): macOS CoreAudio aggregate-device routing with ClockBridge fallback"
```

---

## Task 8: Bench-validation runbook (`docs/bench-validation-runbook.md`)

**Files:**
- Create: `docs/bench-validation-runbook.md`

Encodes the design-spec §13 gates as concrete MANUAL procedures, each with an explicit PASS criterion. No CI — these are operator procedures against real Dirac + hardware. The helper code referenced (a known-level mono-tone WAV generator, a REW-round-trip comparison script) is small and lives where noted; the procedures stand alone.

- [ ] **Step 1: Write the runbook**

Create `docs/bench-validation-runbook.md`:
```markdown
# EARS Bridge — Bench-Validation Runbook (design spec §13 gates)

Each gate is a MANUAL procedure with an explicit PASS criterion. Run all six on **Windows**
(VB-CABLE) and the macOS-relevant ones on **macOS** (BlackHole 2ch). Anchor Dirac findings to the
Dirac Live version under test (record it). Hardware: an EARS **or** EARS Pro, a host with Dirac Live,
and the platform virtual cable. Record results in a table at the bottom.

> Device-capability reminder (spec §3): original EARS = 48 kHz / 24-bit only, 0–36 dB DIP;
> EARS Pro = 44.1–192 kHz, 16/24/32-bit, 0–45 dB DIP. The app exposes only the detected model's
> native rates/bit-depths; the virtual-sink **output** bit depth is user-selectable 16/24/32 (24
> default; 32 only where the sink accepts it, e.g. BlackHole — no measurement benefit beyond 24-bit),
> requested best-effort on the render format (not enforced).

---

## Gate 1 — Named virtual cable appears in Dirac AND completes a measurement (Win + mac)

**Why:** spec §13.1 — strongly inferred, not documented; existential to the product.

**Procedure:**
1. Install the cable (VB-CABLE on Windows, BlackHole 2ch on macOS). Reboot if the installer asks.
2. Start EARS Bridge; select the EARS input and the **virtual cable** output; press Start.
3. Open Dirac Live; in the recording-device dropdown, select the virtual cable's **capture** side
   (e.g. "CABLE Output (VB-Audio Virtual Cable)" / "BlackHole 2ch").
4. Run a full headphone measurement sweep on one channel.

**PASS:**
- The virtual cable's capture side **appears** in Dirac's recording-device list.
- Dirac records the sweep with a usable level and **completes** the measurement with no
  "recording inaccuracy / missing samples" error.
- EARS Bridge shows `cleanCapture` green and `xruns == 0` throughout.

**FAIL signatures:** cable absent from Dirac's list (driver not installed / wrong endpoint); Dirac
shows missing-sample error (a Bridge xrun — check `healthFlags()` for `Xrun`/`Dropout`).

---

## Gate 2 — Dirac stereo-channel + SPL behavior with a known-level mono tone

**Why:** spec §13.2 — learn whether Dirac reads ch0, sums (+6 dB), or averages a stereo capture.

**Helper:** generate a calibrated mono tone WAV (1 kHz, −20 dBFS, 10 s) and play it to the EARS
acoustically (or inject a known electrical level). A tiny generator can live at
`tools/gen_tone.py` (writes `tone_1k_-20dBFS.wav`); or use REW's signal generator.

**Procedure:**
1. Drive the known −20 dBFS / 1 kHz tone so **both** EARS mic channels see the same level.
2. In EARS Bridge, set combine mode to **Average** `(L+R)/2`; note the app's mono output meter dB.
3. Switch to **Sum** `L+R`; note the new mono output meter dB.
4. In Dirac, observe the recorded level for each combine mode.

**PASS:**
- Average shows the **same** level as a single channel (0 dB relative); Sum shows **+6 dB**
  relative to Average for identical L=R content. (This confirms the combine math and lets you
  predict Dirac's absolute-SPL target offset.)
- The dual-mono delivery (L=R on the virtual cable) means Dirac reading ch0, sum, or average all
  yield a consistent, predictable level — record which Dirac actually does for this version.

---

## Gate 3 — Cal-polarity REW round-trip on a known reference (inverse −dB confirmed)

**Why:** spec §13.3 / §3.6 — the EARS cal carries a large +dB bump that must be **removed**; confirm
the Bridge applies the **inverse** (−dB), matching REW's `corrected = raw − cal`.

**Procedure:**
1. In REW, import a known raw measurement `R` and the EARS cal `C` (e.g. `R_HPN_8604350.txt`);
   REW computes `corrected_REW = R − C`.
2. Feed the **same** raw signal through EARS Bridge with the **same** cal loaded; capture the Bridge
   output `corrected_Bridge` (record the virtual-cable output in REW or Dirac's raw capture).
3. Overlay `corrected_Bridge` and `corrected_REW`.

**PASS:**
- The two curves match within **±1.0 dB** across 40 Hz–18 kHz. In particular, at the ~4 kHz EARS
  resonance (cal ≈ +13…+22 dB) the Bridge output shows a **cut** of that magnitude (not a boost).
- If the Bridge output instead **boosts** at 4 kHz, polarity is wrong — `FirDesignParams.invert`
  must be `true` (it is by default); investigate before shipping any equivalence claim.

---

## Gate 4 — WASAPI-exclusive rate negotiation with the virtual cable

**Why:** spec §13.4 — Dirac opens the recording device WASAPI-exclusive; confirm the chosen rate is
accepted. Test **48 kHz (EARS)** AND a **high rate (96/192 kHz, EARS Pro)**.

**Procedure:**
1. **EARS @ 48 kHz:** set EARS Bridge to 48 kHz; set the virtual cable's internal SR to 48 kHz
   (VB-CABLE control panel / AMS). In Dirac (exclusive), run a measurement.
2. **EARS Pro @ 96 kHz then 192 kHz:** set EARS Bridge to 96 kHz, the cable to a matching/clean SR,
   and repeat; then 192 kHz. (The FIR auto-scales taps: 8192 @ 48 k → 16384 @ 96 k → 32768 @ 192 k.)

**PASS:**
- Dirac's exclusive open **succeeds** at each tested rate without "format not supported" / silent
  failure; the measurement completes.
- If exclusive open is rejected, the app's shared-mode fallback engages and the run still completes
  (note that `DAUDIO_WASAPI_NON_EXCLUSIVE=ON` is the documented Dirac opt-out).
- EARS Bridge reports the **input** opened at its native rate (no resample warning for a native
  selection); a non-native rate shows the resample warning as designed.

---

## Gate 5 — Inter-clock drift magnitude over a real sweep (incl. mismatched rates)

**Why:** spec §13.6 — measure real drift to confirm the ASRC is mandatory and the control loop is
stable (no oscillation). Include a **mismatched-rate** case: 96 kHz capture / 48 kHz render.

**Procedure:**
1. Same-nominal-rate case: EARS @ 48 kHz, virtual cable @ 48 kHz. Run a 5-minute continuous sweep
   loop. Log `health().captureToRenderRatio` and `health().fifoFill` every second.
2. Mismatched case (EARS Pro): capture @ 96 kHz, render @ 48 kHz. Repeat the 5-minute log.

**PASS:**
- The ClockBridge holds `fifoFill` within a bounded band (no monotonic emptying/filling); **no**
  underrun/overrun (`underruns()==overruns()==0` or only a tiny startup transient).
- `captureToRenderRatio` settles near its nominal (1.0 same-rate; ~2.0 for 96 k→48 k) and does
  **not** oscillate (the PI loop on smoothed fill does not "breathe"). `healthFlags()` has **no**
  `ExcessDrift` after the startup window.
- On macOS the aggregate path is used instead; confirm the 5-minute run shows no progressive latency
  and no silence (Gate covered by Task 7 MANUAL VERIFICATION step 2).

---

## Gate 6 — Dirac analysis-window length vs worst-case bridge+cable delay

**Why:** spec §13.7 — a fixed bridge+cable delay (tens of ms) shifts the impulse in Dirac's analysis
window; confirm it still lands inside the window at the worst-case latency.

**Procedure:**
1. Measure the end-to-end Bridge latency: app FIFO depth + ASRC + cable + Dirac buffering. Read the
   app's reported latency (status bar) and add the cable's nominal latency.
2. Run a Dirac measurement at the **largest** buffer size you will ship as default.
3. Inspect Dirac's impulse/IR view: confirm the located impulse sits well inside the analysis window
   (not clipped at the window edge).

**PASS:**
- Dirac locates the impulse (log-sweep cross-correlation) and applies gain+delay compensation; the
  impulse is comfortably inside the window with margin (≥ 2× the measured delay of headroom).
- No "recording inaccuracy" warning at the worst-case latency.

---

## Results table (fill in per run)

| Gate | Platform | Device   | Rate    | Dirac ver | Result | Notes |
|------|----------|----------|---------|-----------|--------|-------|
| 1    | Windows  | EARS     | 48 k    |           |        |       |
| 1    | macOS    | EARS Pro | 96 k    |           |        |       |
| 2    | Windows  | EARS     | 48 k    |           |        |       |
| 3    | Windows  | EARS     | 48 k    |           |        |       |
| 4    | Windows  | EARS     | 48 k    |           |        |       |
| 4    | Windows  | EARS Pro | 96/192k |           |        |       |
| 5    | Windows  | EARS Pro | 96→48 k |           |        |       |
| 6    | Windows  | EARS     | 48 k    |           |        |       |

> Version drift (spec §13.8): all Dirac findings are version-anchored. Record the Dirac Live version
> in each row and re-run on every Dirac update (exclusive-mode default and the missing-sample
> detector have both changed within 3.x).
```

- [ ] **Step 2: Commit**

```bash
git add docs/bench-validation-runbook.md
git commit -m "docs(plan4): bench-validation runbook encoding spec section 13 gates"
```

---

## Task 9: Full-suite green + final review

**Files:** none (verification only)

- [ ] **Step 1: Configure + build the whole project**

Run: `cmd /c "tools\dev.cmd cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release"`
then: `cmd /c "tools\dev.cmd cmake --build build"`
Expected: `eb_core`, `eb_engine`, `eb_tests`, and `EarsBridge` all build with no errors.

- [ ] **Step 2: Run the full test suite**

Run: `cmd /c "tools\dev.cmd ctest --test-dir build --output-on-failure"`
Expected: **all** tests pass, including the new `HealthMonitor`, `CalBinder`, `AsioFallback`, and
`LrVerify` cases, plus the Plan-1/2/3 suites.

- [ ] **Step 3: Self-review against the spine**

Confirm, by reading the headers:
- `EngineTypes.h` still declares the spine `EarsModel`, `EngineStatus`, `Levels`, `Health`
  **unchanged**; Plan-4 additions are purely additive (`DipGainProfile`, `HealthFlag`).
- `AudioEngine`'s spine methods (`inputDevices`…`health()`) are unchanged; Plan-4 additions
  (`healthFlags`, `cleanCapture`, `gainProfile`, `beginLrVerify`, `lrVerifyResult`,
  `lrVerifyComplete`) are additive.
- No audio-callback method allocates, locks, or makes a syscall (grep the callbacks for `new`,
  `malloc`, `std::vector` growth, `juce::String`, mutex use — there should be none).

- [ ] **Step 4: Commit (if any review fixes were needed)**

```bash
git add -A
git commit -m "chore(plan4): full-suite green; spine-consistency review"
```

---

## Self-review notes (for the executor)

- **Atomics only on the audio thread.** `HealthMonitor` and the engine callbacks use `std::atomic`
  stores/loads exclusively. Never add a `juce::String`, `std::vector` resize, or lock inside the
  audio-thread methods (`reportInLevels`/`reportOutLevel`/`observeRenderBlock`/`reportXrun`/
  `reportDroppedFrames`/`setFifoFill`/`setCaptureToRenderRatio`) or the callbacks.
- **Latch semantics.** `cleanCapture` is cleared by *invalidating* flags (`Xrun`, `Dropout`,
  `ExcessDrift`, `FifoStarved`) and never re-raised within a run; `ClipInput`/`LowLevel`/`ClipOutput`
  are **guidance** warnings that do not invalidate a measurement. Tests pin this distinction.
- **Drift is a sustained-condition state machine,** not a single-block trip — one out-of-tolerance
  block must not latch `ExcessDrift`; an in-tolerance block resets the run counter.
- **CalBinder guards the model.** A key collision where the device class changed
  (EARS ↔ EARS Pro) must refuse the stale binding — the FIR is designed at the model's native rate.
- **macOS aggregate is preferred, ClockBridge is the fallback.** When the aggregate is active, the
  engine reports a nominal in-tolerance ratio (the OS does drift correction); the ClockBridge path is
  used only when the aggregate can't be created.
- **`Main`/`Master` key spelling.** Apple renamed `kAudioAggregateDeviceMasterSubDeviceKey` →
  `…MainSubDeviceKey`; use whichever your SDK header declares (re-verify in the API pass).
- **The `.mm` is macOS-only;** the header is portable with `#if JUCE_MAC` stubs so the Windows build
  is unaffected.

## Done criteria for Plan 4

- `ctest --test-dir build` is green with the four new suites:
  - `HealthMonitor` — clip/low-level/dropout/drift thresholds + latch + reset + xrun.
  - `CalBinder` — re-bind by key incl. model, model-change refusal, overwrite, forget.
  - `AsioFallback` — ASIO (no separate I/O) → WASAPI/CoreAudio fallback branch, mockable.
  - `LrVerify` — Pass/Swapped/Ambiguous/Silent/Pending state machine.
- `AudioEngine` instruments its single Plan-2 `HealthMonitor` member `hm` (the now-full monitor; no
  second monitor introduced), exposes the additive `healthFlags()`, `cleanCapture()`, `gainProfile()`,
  and the `beginLrVerify`/`lrVerifyResult`/`lrVerifyComplete` surface, applies the ASIO→WASAPI
  fallback in `start()` via the `DeviceManager` type-caps seam, and re-binds cal via `CalBinder` on
  re-enumeration — all without changing the spine signatures.
- macOS: `AggregateDevice` creates a private, drift-corrected aggregate (clock = EARS), is preferred
  over the ClockBridge, and tears down cleanly; the Windows build is unaffected (`.mm` excluded).
- `docs/bench-validation-runbook.md` encodes all six §13 gates as MANUAL procedures with explicit
  PASS criteria and a results table.
- The five MANUAL VERIFICATION blocks (engine wiring, macOS aggregate, and the six bench gates) have
  explicit, non-vague PASS criteria for the no-CI device/Dirac/bench interactions.
```
