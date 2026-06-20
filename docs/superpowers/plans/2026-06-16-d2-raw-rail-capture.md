# Raw-Rail Capture (native-rate pin + no-SRC assert) Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Make the "confirmed clip" verdict trustworthy at its source. Today the EARS opens in WASAPI/CoreAudio *shared* mode with the OS sample-rate converter armed (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`), so the app measures OS-resampled/mixed float, not the device's true digital rails — the merged "confirmed clip" run-detector is therefore only a run-on-shared-float proxy. This plan pins the EARS input to the requested native rate, reads back the granted rate/depth at open, and **asserts no SRC engaged** (`getCurrentSampleRate() == requested`). When the rates match it surfaces a **raw-rail verified** state; on a mismatch it refuses to silently trust the run and surfaces **"OS-resampled — clip detection approximate"** (a guidance, non-invalidating `HealthFlag`), to the engine and the GUI.

**Architecture:** Extend the existing surfaces — no new threads, no new libraries, no rewrite. `DeviceManager::openInput` records the requested input rate and reads back the granted input rate + bit depth (mirroring the existing `requestedOutBits`/`grantedOutBits` output pattern) and exposes a pure `rawRailVerified()` predicate. `AudioEngine::start` consults it after the streams open, latches a non-invalidating `HealthFlag::OsResampled` via `HealthMonitor` when SRC is detected, and exposes a `RawRailState` snapshot. `MainComponent` renders "raw-rail verified" vs the approximate caveat in the existing post-start downgrade-note path. All audio-thread-reachable code stays allocation/lock/log/exception-free.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

This plan implements audit finding **D2** / requirement **R2** (see `docs/EARS_DIRAC_CLIPPING_AUDIT.md` §2, the D2 defect, and §7 step 1). It builds directly on the merged first slice (`docs/superpowers/plans/2026-06-16-confirmed-clipping-detection.md` — D1/D3/D4: `analyzeInputBlock`, `scanAndFlagNonFinite`, `HealthFlag::ClipConfirmed`/`NonFinite`, `src/gui/ClipStatus.h`). **Out of scope** (named follow-ups): D5 sweep-session machine, D6 frozen SRC ratio during the sweep, D7 combine-mode gating, D8 per-block runtime-format-change revalidation, and full `AudioIODeviceCallback` instantiation in tests.

## Design decision

**Chosen approach (audit §7 step 1): native-rate pin + read-back no-SRC assert in shared mode.** `DeviceManager::openInput` already calls `inDev->open(inCh, noOut, sampleRate, bufferSize)`; we keep shared mode but, after the open succeeds, read `getCurrentSampleRate()` / `getCurrentBitDepth()` back and compare to what was requested. If `granted == requested` (within 0.5 Hz) the OS SRC did not resample our stream and the float values faithfully represent the integer rails — **raw-rail verified**. If they differ, the stream was OS-resampled and the run-based clip detector is **approximate**; we flag it rather than silently trusting it.

**Named alternative (not chosen): open the EARS WASAPI-exclusive for the measurement** (`WASAPIDeviceMode::exclusive` / `AUDCLNT_SHAREMODE_EXCLUSIVE`). That would deliver true integer rails but JUCE's `"Windows Audio"` type is shared-only (`juce_WASAPI_windows.cpp:1957`), so it needs a separate exclusive device type and breaks the validated shared-mode config the whole app is built around (CLAUDE/MEMORY: "working config = standard VB-CABLE + shared mode"). The audit's §7 step 1 explicitly offers the native-rate-pin path as the smallest honest change ("Pin the EARS to its native rate/depth and verify `getCurrentSampleRate()==requested` … *or* open the EARS WASAPI-exclusive"), and §2's D2 nuance notes that in the matched-rate case a full-scale integer maps to ±1.0f and *is* detectable. We take the matched-rate pin and surface the residual shared-mixer caveat in the wording, deferring exclusive mode.

## Global Constraints

- Build (tests): ./tools/dev.cmd cmake --build build --target eb_tests  — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never cmd /c; it wraps Ninja + MSVC.
- Run a test: ./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure . Full suite: ./tools/dev.cmd ctest --test-dir build --output-on-failure (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + std::atomic only.
- HealthFlag (src/audio/EngineTypes.h) is NOT persisted — append enum values freely. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
- Work on a feature branch, not main.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/DeviceManager.h` | device open + format read-back surface | add `requestedInputRate_`/`grantedInputRate_`/`grantedInputBits_` members + `requestedInputSampleRate()`/`grantedInputSampleRate()`/`grantedInputBitDepth()`/`rawRailVerified()` accessors |
| `src/audio/DeviceManager.cpp` | `openInput` impl + `closeAll` | record requested input rate; read back granted input rate/depth after open; reset on close |
| `src/audio/EngineTypes.h` | `HealthFlag` enum + `RawRailState` struct | append `OsResampled` flag (guidance, non-invalidating); add a small `RawRailState` snapshot struct |
| `src/audio/HealthMonitor.h` | telemetry surface | declare `reportRawRail (bool verified) noexcept` |
| `src/audio/HealthMonitor.cpp` | telemetry impl + `raise()` mask | implement `reportRawRail`; keep `OsResampled` OUT of the invalidating mask (guidance only) |
| `src/audio/AudioEngine.h` | engine telemetry surface | add `rawRail()` snapshot accessor |
| `src/audio/AudioEngine.cpp` | `start()` open path | after open, read `DeviceManager` rail state, latch `OsResampled` when not verified, populate the snapshot |
| `src/gui/RawRailStatus.h` | **new** — pure flag/state → message helper | testable "raw-rail verified" vs "OS-resampled — approximate" text |
| `src/gui/MainComponent.cpp` | post-start downgrade-note path | append the raw-rail note to the existing `notes` StringArray |
| `tests/test_devicemanager.cpp` | DeviceManager unit tests | assert read-back accessors + `rawRailVerified()` semantics in the headless (no-device) case |
| `tests/test_healthmonitor.cpp` | HealthMonitor unit tests | assert `reportRawRail` raises `OsResampled` only when not verified, and that it does NOT invalidate |
| `tests/test_rawrailstatus.cpp` | **new** — message helper tests | register in `tests/CMakeLists.txt` |

---

## Task 1: DeviceManager records the requested input rate and reads back the granted input rate/depth

**Files:**
- Modify: `src/audio/DeviceManager.h:72` (openInput already declared), `:75-89` (accessors + members)
- Modify: `src/audio/DeviceManager.cpp:155-166` (openInput body), `:196-200` (closeAll reset)
- Test: `tests/test_devicemanager.cpp`

**Interfaces:**
- Consumes: `juce::AudioIODevice::getCurrentSampleRate()` / `getCurrentBitDepth()` (already used for output at `DeviceManager.cpp:192` and for input rate at `AudioEngine.cpp:272`).
- Produces:
  - `double eb::DeviceManager::requestedInputSampleRate() const` — the rate last passed to `openInput` (0.0 before any open / after `closeAll`).
  - `double eb::DeviceManager::grantedInputSampleRate() const` — `getCurrentSampleRate()` read back after the last successful `openInput` (0.0 = none/closed).
  - `int eb::DeviceManager::grantedInputBitDepth() const` — `getCurrentBitDepth()` read back after the last successful `openInput` (0 = unknown/none).
  - `bool eb::DeviceManager::rawRailVerified() const` — true iff an input is open AND `|granted - requested| <= 0.5` Hz (no OS SRC resampled our stream). False when closed or resampled.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_devicemanager.cpp`:
```cpp
TEST_CASE("DeviceManager raw-rail read-back is zeroed before any open and after closeAll") {
    eb::DeviceManager dm;
    CHECK (dm.requestedInputSampleRate() == 0.0);
    CHECK (dm.grantedInputSampleRate()   == 0.0);
    CHECK (dm.grantedInputBitDepth()     == 0);
    CHECK_FALSE (dm.rawRailVerified());   // nothing open => not verified

    eb::DeviceId nope; nope.name = "no-such-input-device-xyz";
    auto err = dm.openInput (nope, 48000.0, 512);   // headless CI: device cannot be created
    CHECK (err.isNotEmpty());                        // open failed (no real EARS in tests)
    // A failed open must record the request but grant nothing and stay unverified.
    CHECK (dm.requestedInputSampleRate() == 48000.0);
    CHECK (dm.grantedInputSampleRate()   == 0.0);
    CHECK_FALSE (dm.rawRailVerified());

    dm.closeAll();
    CHECK (dm.requestedInputSampleRate() == 0.0);
    CHECK (dm.grantedInputSampleRate()   == 0.0);
    CHECK (dm.grantedInputBitDepth()     == 0);
    CHECK_FALSE (dm.rawRailVerified());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `requestedInputSampleRate` / `grantedInputSampleRate` / `grantedInputBitDepth` / `rawRailVerified` are undeclared.

- [ ] **Step 3: Declare accessors + members (header)**

In `src/audio/DeviceManager.h`, after the existing output read-back accessors (`grantedOutputBitDepth()`, line 76), add:
```cpp
    // ---- Raw-rail (D2) read-back: did the OS SRC resample our INPUT stream? ----
    // openInput requests the EARS native rate; after a successful open we read getCurrentSampleRate()
    // back. If granted==requested the OS sample-rate converter (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM)
    // did NOT resample our stream, so the float values faithfully represent the device's integer rails
    // and the confirmed-clip run detector is trustworthy ("raw-rail verified"). A mismatch means the
    // run was OS-resampled and clip detection is approximate (see docs/EARS_DIRAC_CLIPPING_AUDIT.md D2).
    double requestedInputSampleRate() const { return requestedInRate_; }   // 0.0 = none/closed
    double grantedInputSampleRate()   const { return grantedInRate_; }     // 0.0 = none/closed
    int    grantedInputBitDepth()     const { return grantedInBits_; }      // 0 = unknown/none
    bool   rawRailVerified()          const;                                // open AND not OS-resampled
```
In the private members block, after `grantedOutBits` (line 88), add:
```cpp
    double requestedInRate_ = 0.0;    // rate last requested on openInput (0 = none/closed)
    double grantedInRate_   = 0.0;    // getCurrentSampleRate() read back after the last input open
    int    grantedInBits_   = 0;      // getCurrentBitDepth() read back after the last input open
```

- [ ] **Step 4: Implement the read-back + reset (cpp)**

In `src/audio/DeviceManager.cpp`, replace the whole `openInput` body (lines 155-166) with:
```cpp
juce::String DeviceManager::openInput (const DeviceId& id, double sampleRate, int bufferSize) {
    // Record the requested native rate up front so a failed open still reports what was asked for
    // (the raw-rail read-back below only fills in on success).
    requestedInRate_ = sampleRate;
    grantedInRate_   = 0.0;
    grantedInBits_   = 0;

    auto* type = findPreferredType();
    if (type == nullptr) return "No suitable audio driver type (WASAPI/CoreAudio) found";
    type->scanForDevices();
    inDev.reset (type->createDevice ({}, id.name));   // input-only
    if (inDev == nullptr) return "Could not create input device: " + id.name;
    juce::BigInteger inCh; inCh.setRange (0, 2, true);     // 2 ears
    juce::BigInteger noOut;
    auto err = inDev->open (inCh, noOut, sampleRate, bufferSize);
    if (err.isNotEmpty()) { inDev.reset(); return "Open input failed: " + err; }
    // Raw-rail (D2): read back what the driver actually granted. In WASAPI/CoreAudio SHARED mode the
    // OS SRC (AUTOCONVERTPCM) can resample to its endpoint mix format; getCurrentSampleRate() reports
    // the rate OUR stream actually runs at. granted==requested => no SRC on our stream (raw rails).
    grantedInRate_ = inDev->getCurrentSampleRate();
    grantedInBits_ = inDev->getCurrentBitDepth();
    return {};
}
```
In `closeAll` (lines 196-200), add the input read-back reset alongside the existing `grantedOutBits = 0;`:
```cpp
void DeviceManager::closeAll() {
    if (inDev)  { inDev->stop();  inDev->close();  inDev.reset(); }
    if (outDev) { outDev->stop(); outDev->close(); outDev.reset(); }
    grantedOutBits = 0;
    requestedInRate_ = 0.0; grantedInRate_ = 0.0; grantedInBits_ = 0;
}
```
Add the `rawRailVerified()` body (place it just after `openInput`, before `openOutput`):
```cpp
bool DeviceManager::rawRailVerified() const {
    // Verified only when an input is OPEN and the granted rate matches the request within 0.5 Hz
    // (so the OS SRC did not resample our stream). A closed/failed input is never "verified".
    return inDev != nullptr
        && grantedInRate_ > 0.0
        && std::abs (grantedInRate_ - requestedInRate_) <= 0.5;
}
```
(`<algorithm>` is already included at `DeviceManager.cpp:4`; `std::abs` for `double` comes via `<cmath>` transitively through JUCE — if the build reports `std::abs` ambiguous, add `#include <cmath>` next to the existing `#include <algorithm>` at line 4.)

- [ ] **Step 5: Run test to verify it passes**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "raw-rail read-back" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/audio/DeviceManager.h src/audio/DeviceManager.cpp tests/test_devicemanager.cpp
git commit -m "feat(device): read back granted input rate/depth; rawRailVerified no-SRC predicate

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: A non-invalidating OsResampled flag + HealthMonitor::reportRawRail

**Files:**
- Modify: `src/audio/EngineTypes.h:15-26` (HealthFlag enum)
- Modify: `src/audio/HealthMonitor.h:14-25` (declare `reportRawRail`)
- Modify: `src/audio/HealthMonitor.cpp:8-20` (raise mask — leave OsResampled OUT), add `reportRawRail` body
- Test: `tests/test_healthmonitor.cpp`

**Interfaces:**
- Consumes: `HealthFlag` (existing), `HealthMonitor::raise` (existing private helper).
- Produces:
  - `HealthFlag::OsResampled` enum value (`1u << 9`) — **guidance, non-invalidating**: the OS SRC resampled our input stream so clip detection is approximate. It is NOT added to the invalidating mask, so `cleanCapture` stays true.
  - `void eb::HealthMonitor::reportRawRail(bool verified) noexcept` — raises `OsResampled` iff `verified == false`; a no-op when verified. Called once per run from `AudioEngine::start` (message thread), but written through the same atomic `flagBits` as the audio path so it is publication-safe.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("HealthMonitor::reportRawRail raises OsResampled only when NOT verified, never invalidates") {
    using eb::HealthFlag; using eb::any;

    SECTION ("verified raw rail => no flag, stays clean") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (true);
        CHECK_FALSE (any (h.flags() & HealthFlag::OsResampled));
        CHECK (h.cleanCapture());
    }
    SECTION ("OS-resampled => OsResampled flag, but measurement stays VALID (guidance only)") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (false);
        CHECK (any (h.flags() & HealthFlag::OsResampled));
        CHECK (h.cleanCapture());   // approximate, not invalid
    }
    SECTION ("reset clears the flag") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (false);
        CHECK (any (h.flags() & HealthFlag::OsResampled));
        h.reset();
        CHECK_FALSE (any (h.flags() & HealthFlag::OsResampled));
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `HealthFlag::OsResampled` and `reportRawRail` are undeclared.

- [ ] **Step 3: Append the enum value**

In `src/audio/EngineTypes.h`, extend the `HealthFlag` enum (currently ending at `NonFinite = 1u << 8`):
```cpp
enum class HealthFlag : unsigned {
    None        = 0,
    Xrun        = 1u << 0,
    Dropout     = 1u << 1,
    ExcessDrift = 1u << 2,
    ClipInput   = 1u << 3,   // guidance: near full scale (-1 dBFS); does NOT invalidate
    LowLevel    = 1u << 4,
    ClipOutput  = 1u << 5,   // guidance: program output hit the clamp; does NOT invalidate
    FifoStarved = 1u << 6,
    ClipConfirmed = 1u << 7, // INVALIDATING: a consecutive near-rail run = confirmed digital clip
    NonFinite     = 1u << 8, // INVALIDATING: a NaN/Inf sample reached the path
    OsResampled   = 1u << 9  // guidance: OS SRC resampled the INPUT (D2) -> clip detection approximate; does NOT invalidate
};
```

- [ ] **Step 4: Declare + implement reportRawRail (mask unchanged)**

In `src/audio/HealthMonitor.h`, in the Plan-2 canonical surface (after `reportOutLevel`, line 21), declare:
```cpp
    // Raw-rail (D2): record whether the input opened at its native rate with NO OS resample.
    // verified==false raises HealthFlag::OsResampled (GUIDANCE, non-invalidating): clip detection is
    // approximate on an OS-resampled stream. Called once per run from AudioEngine::start.
    void reportRawRail (bool verified) noexcept;
```
In `src/audio/HealthMonitor.cpp`, add the body (place it just after `reportOutLevel`, before `scanAndFlagNonFinite`):
```cpp
void HealthMonitor::reportRawRail (bool verified) noexcept {
    if (! verified) raise (HealthFlag::OsResampled);   // guidance only; raise() does NOT clear clean for it
}
```
**Do NOT** add `OsResampled` to the `invalidating` mask in `raise()` (lines 11-17) — it is guidance, exactly like `ClipInput`/`ClipOutput`/`LowLevel`. Confirm the mask still reads:
```cpp
    const unsigned invalidating =
        static_cast<unsigned> (HealthFlag::Xrun)          |
        static_cast<unsigned> (HealthFlag::Dropout)       |
        static_cast<unsigned> (HealthFlag::ExcessDrift)   |
        static_cast<unsigned> (HealthFlag::FifoStarved)   |
        static_cast<unsigned> (HealthFlag::ClipConfirmed) |
        static_cast<unsigned> (HealthFlag::NonFinite);
```
(`reset()` already does `flagBits.store(0)` at `HealthMonitor.cpp:37`, so the reset section of the test passes with no change.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "reportRawRail" --output-on-failure`
Expected: PASS (all three sections).

- [ ] **Step 6: Commit**

```bash
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "feat(health): OsResampled guidance flag + reportRawRail (non-invalidating)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: AudioEngine exposes a RawRailState snapshot and latches OsResampled on start

**Files:**
- Modify: `src/audio/EngineTypes.h:35-42` (add `RawRailState` struct after `Health`)
- Modify: `src/audio/AudioEngine.h:86-88` (declare `rawRail()` near `grantedSampleRate()`)
- Modify: `src/audio/AudioEngine.cpp:270-278` (post-open, before `graph.prepare`) and add the accessor body
- Test: `tests/test_audioengine.cpp`

**Interfaces:**
- Consumes: `DeviceManager::rawRailVerified()`, `grantedInputSampleRate()`, `requestedInputSampleRate()`, `grantedInputBitDepth()` (Task 1); `HealthMonitor::reportRawRail` (Task 2).
- Produces:
  - `struct eb::RawRailState { bool verified=false; double requestedRate=0.0; double grantedRate=0.0; int grantedBits=0; }` — a plain value snapshot.
  - `eb::RawRailState eb::AudioEngine::rawRail() const noexcept` — the rail state captured at the last successful `start()` (default-constructed = unverified before any run). Set after the input opens, before the streams start.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_audioengine.cpp`:
```cpp
TEST_CASE("AudioEngine::rawRail defaults to unverified before any run") {
    eb::AudioEngine e;
    auto rr = e.rawRail();
    CHECK_FALSE (rr.verified);
    CHECK (rr.grantedRate == 0.0);
    CHECK (rr.requestedRate == 0.0);
    CHECK (rr.grantedBits == 0);
}
```
(`tests/test_audioengine.cpp` already includes `"audio/AudioEngine.h"` and `<vector>`; `RawRailState` lives in `audio/EngineTypes.h`, transitively included.)

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `AudioEngine::rawRail` / `eb::RawRailState` are undeclared.

- [ ] **Step 3: Add the snapshot struct (EngineTypes.h)**

In `src/audio/EngineTypes.h`, after the `Health` struct (closing brace at line 42), add:
```cpp
// --- D2 addition: raw-rail capture state, captured once per run by AudioEngine::start ---
// verified == true means the EARS input opened at the requested native rate with no OS SRC, so the
// float samples faithfully represent the device's integer rails and the confirmed-clip detector is
// trustworthy. verified == false means the OS resampled our stream -> clip detection is approximate.
struct RawRailState {
    bool   verified      = false;
    double requestedRate = 0.0;
    double grantedRate   = 0.0;
    int    grantedBits   = 0;
};
```

- [ ] **Step 4: Declare + implement the accessor; latch on start**

In `src/audio/AudioEngine.h`, after `grantedOutputBitDepth()` (line 88), declare:
```cpp
    // The raw-rail (D2) capture state captured at the last successful start(): did the EARS open at
    // its native rate with no OS resample? The GUI shows "raw-rail verified" vs an "approximate" note.
    RawRailState rawRail() const noexcept;
```
Add a private member after `grantedRate_` (line 133):
```cpp
    RawRailState rawRail_;   // D2: captured once per successful start(); default = unverified
```
In `src/audio/AudioEngine.cpp`, add the accessor body next to the other one-liners (after `grantedOutputBitDepth`, line 176):
```cpp
RawRailState AudioEngine::rawRail() const noexcept { return rawRail_; }
```
In `AudioEngine::start`, in the block that already reads the granted capture rate (lines 270-278), capture the rail state and latch the flag **right after** `grantedRate_ = capRate;` and before `graph.prepare(...)`. The `hm.prepare(...)` at line 293 resets `flagBits`, so `reportRawRail` MUST be called *after* that prepare; do it there. Concretely:

Replace lines 270-274:
```cpp
    auto* inD  = devices.inputDevice();
    auto* outD = devices.outputDevice();
    const double capRate = inD->getCurrentSampleRate();
    const double renRate = outD->getCurrentSampleRate();
    grantedRate_ = capRate;   // what the EARS actually runs at (may differ from the user's selection)
```
with:
```cpp
    auto* inD  = devices.inputDevice();
    auto* outD = devices.outputDevice();
    const double capRate = inD->getCurrentSampleRate();
    const double renRate = outD->getCurrentSampleRate();
    grantedRate_ = capRate;   // what the EARS actually runs at (may differ from the user's selection)
    // Raw-rail (D2): capture whether the EARS input opened at its native rate with no OS SRC. On the
    // aggregate path the aggregate device is the "input", so this still reflects the real open. The
    // flag is latched AFTER hm.prepare() below (which clears flagBits), not here.
    rawRail_ = RawRailState { devices.rawRailVerified(),
                              devices.requestedInputSampleRate(),
                              devices.grantedInputSampleRate(),
                              devices.grantedInputBitDepth() };
```
Then immediately after the existing `hm.prepare (inputId.model, cap, capRate / juce::jmax (1.0, renRate));` line (line 293), add:
```cpp
    hm.reportRawRail (rawRail_.verified);   // guidance OsResampled flag if the OS resampled our input
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "rawRail defaults" --output-on-failure`
Expected: PASS. Then the full suite (no behavior change for existing runs — verified-by-default snapshot is empty until a real start):
`./tools/dev.cmd ctest --test-dir build --output-on-failure`
Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add src/audio/EngineTypes.h src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "feat(engine): RawRailState snapshot + latch OsResampled on start (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Pure RawRailStatus message helper + GUI note

**Files:**
- Create: `src/gui/RawRailStatus.h`
- Modify: `src/gui/MainComponent.cpp` (the post-start `notes` StringArray, lines 576-585)
- Create + register test: `tests/test_rawrailstatus.cpp`, `tests/CMakeLists.txt:1-21`

**Interfaces:**
- Consumes: `eb::RawRailState` (`AudioEngine::rawRail()`).
- Produces: `juce::String eb::rawRailNote(const eb::RawRailState&)` — empty when nothing useful to say (no run yet), `"Raw-rail verified: EARS at NN.N kHz, no OS resampling."` when verified, or `"OS-resampled to NN.N kHz (requested MM.M kHz) - clip detection approximate."` when not. Header-only + pure so it is unit-testable without the GUI (mirrors `gui/ClipStatus.h`).

- [ ] **Step 1: Write the failing test**

Create `tests/test_rawrailstatus.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "gui/RawRailStatus.h"
#include <string>

TEST_CASE("rawRailNote reports verified vs OS-resampled honestly") {
    using eb::RawRailState; using eb::rawRailNote;

    SECTION ("no run yet (granted 0) => empty note") {
        CHECK (rawRailNote (RawRailState{}).isEmpty());
    }
    SECTION ("verified => positive confirmation with the rate") {
        RawRailState rr; rr.verified = true; rr.requestedRate = 48000.0; rr.grantedRate = 48000.0; rr.grantedBits = 24;
        CHECK (rawRailNote (rr)
               == juce::String ("Raw-rail verified: EARS at 48.0 kHz, no OS resampling."));
    }
    SECTION ("OS-resampled => approximate caveat naming both rates") {
        RawRailState rr; rr.verified = false; rr.requestedRate = 96000.0; rr.grantedRate = 48000.0; rr.grantedBits = 32;
        CHECK (rawRailNote (rr)
               == juce::String ("OS-resampled to 48.0 kHz (requested 96.0 kHz) - clip detection approximate."));
    }
}
```
Register it in `tests/CMakeLists.txt` by adding `    test_rawrailstatus.cpp` to the `add_executable(eb_tests ...)` list, after `test_clipstatus.cpp` (line 21 — change `test_clipstatus.cpp)` to `test_clipstatus.cpp\n    test_rawrailstatus.cpp)`).

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `gui/RawRailStatus.h` does not exist.

- [ ] **Step 3: Create the pure helper**

Create `src/gui/RawRailStatus.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"

namespace eb {

// Maps the raw-rail (D2) capture state to an honest one-line note for the post-start status area.
// Empty when there is nothing to report (no run yet => grantedRate == 0). Header-only + pure so it is
// unit-testable without the GUI (mirrors gui/ClipStatus.h). See docs/EARS_DIRAC_CLIPPING_AUDIT.md D2.
[[nodiscard]] inline juce::String rawRailNote (const RawRailState& rr) {
    if (rr.grantedRate <= 0.0) return {};   // nothing opened yet
    if (rr.verified)
        return "Raw-rail verified: EARS at " + juce::String (rr.grantedRate / 1000.0, 1)
             + " kHz, no OS resampling.";
    return "OS-resampled to " + juce::String (rr.grantedRate / 1000.0, 1) + " kHz (requested "
         + juce::String (rr.requestedRate / 1000.0, 1) + " kHz) - clip detection approximate.";
}

} // namespace eb
```

- [ ] **Step 4: Append the note in the GUI start path**

In `src/gui/MainComponent.cpp`, add the include near the other `gui/` includes (next to `#include "gui/ClipStatus.h"`):
```cpp
#include "gui/RawRailStatus.h"
```
In `onStartStop()`, in the successful-start branch that builds the `notes` StringArray (lines 576-585), append the raw-rail note just before `preflightLabel.setText (notes.joinIntoString (" "), ...)` at line 585:
```cpp
            // Raw-rail (D2): tell the user whether the EARS opened at its native rate with no OS
            // resampling (clip detection trustworthy) or was OS-resampled (clip detection approximate).
            const auto railNote = eb::rawRailNote (engine.rawRail());
            if (railNote.isNotEmpty()) notes.add (railNote);
```
The existing rate-downgrade note (lines 578-580) already covers the *value* of the resample; this adds the explicit raw-rail *verdict* so the user knows whether to trust the clip result.

- [ ] **Step 5: Run tests + build the app**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "rawRailNote" --output-on-failure`
Expected: PASS (all three sections).
Then confirm the app still builds: `./tools/dev.cmd cmake --build build --target EarsBridge`
Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git add src/gui/RawRailStatus.h src/gui/MainComponent.cpp tests/test_rawrailstatus.cpp tests/CMakeLists.txt
git commit -m "feat(ui): surface raw-rail verified vs OS-resampled note on start (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: ClipStatus appends the approximate caveat to a confirmed-clip verdict

**Files:**
- Modify: `src/gui/ClipStatus.h:8-14` (`invalidMeasurementMessage`)
- Modify: `src/gui/MainComponent.cpp` (the `updateStatusLine` invalid branch — wherever `invalidMeasurementMessage(h.flags)` is rendered)
- Test: `tests/test_clipstatus.cpp`

**Interfaces:**
- Consumes: `HealthFlag::OsResampled` (Task 2), `HealthFlag::ClipConfirmed` (merged slice).
- Produces: `invalidMeasurementMessage(HealthFlag)` now appends `" (OS-resampled - approximate)"` to the *confirmed-clip* message when `OsResampled` is also set, so an invalidating clip detected on a resampled stream does not overclaim certainty. Non-clip invalid messages (NonFinite, Dropout) are unchanged.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_clipstatus.cpp`:
```cpp
TEST_CASE("invalidMeasurementMessage qualifies a confirmed clip as approximate when OS-resampled") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    // Confirmed clip on a raw-rail-verified stream: the plain, confident message.
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    // Same clip but the OS resampled our input: append the approximate caveat.
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::OsResampled))
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat. "
              "(OS-resampled - approximate)");
    // OsResampled alone is guidance, never reaches this helper as an invalid cause -> NonFinite/Dropout
    // messages are unchanged when OsResampled co-occurs.
    CHECK (std::string (invalidMeasurementMessage (HealthFlag::Dropout | HealthFlag::OsResampled))
           == "Dropouts detected - this measurement is invalid.");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "qualifies a confirmed clip" --output-on-failure`
Expected: FAIL — the current helper returns the plain confirmed-clip message with no caveat.

- [ ] **Step 3: Append the caveat in the helper**

In `src/gui/ClipStatus.h`, change the return type to `juce::String` (so the caveat can be appended) and add the `OsResampled` branch. The header currently returns `const char*`; switch it to `juce::String` and update the include. Replace the whole helper body (lines 8-14):
```cpp
[[nodiscard]] inline juce::String invalidMeasurementMessage (HealthFlag flags) {
    if (any (flags & HealthFlag::ClipConfirmed)) {
        juce::String msg = "Input reached digital full scale - this measurement is invalid. "
                           "Lower the level and repeat.";
        if (any (flags & HealthFlag::OsResampled))   // D2: clip was detected on an OS-resampled stream
            msg += " (OS-resampled - approximate)";
        return msg;
    }
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    return "Dropouts detected - this measurement is invalid.";
}
```
At the top of `src/gui/ClipStatus.h`, ensure `<juce_core/juce_core.h>` is included for `juce::String` (the file currently only includes `"audio/EngineTypes.h"`, which itself includes `<juce_core/juce_core.h>` at `EngineTypes.h:2`, so `juce::String` already resolves — add the explicit include only if the build complains):
```cpp
#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"
```

- [ ] **Step 4: Fix the call site (return type changed)**

The merged slice renders this helper in `updateStatusLine` via `statusLine.setText (eb::invalidMeasurementMessage (h.flags), juce::dontSendNotification);`. `juce::Label::setText` already takes a `juce::String`, and a `juce::String` binds to it directly, so the call site compiles unchanged. Confirm by grepping the GUI:
```bash
git grep -n "invalidMeasurementMessage" src/gui
```
If any caller stored the result in a `const char*` (it should not — the merged plan passed it straight into `setText`), change that local to `juce::String`. No `std::string` wrapping is needed at the call site (the unit test wraps it explicitly).

- [ ] **Step 5: Run tests + build the app**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "invalidMeasurementMessage|qualifies a confirmed clip" --output-on-failure`
Expected: PASS — the pre-existing `invalidMeasurementMessage` cases (from the merged slice) still pass because the `std::string(...)` wrapper in those tests accepts a `juce::String` via `juce::String::toStdString()`... **note:** `std::string (juce::String)` does NOT compile. If the merged `test_clipstatus.cpp` wraps the result in `std::string(...)`, update those wrappers to `(eb::invalidMeasurementMessage(...)).toStdString()` (or compare against `juce::String` directly). Apply the same `.toStdString()` form in the new test from Step 1. Re-run until green.
Then build the app: `./tools/dev.cmd cmake --build build --target EarsBridge`
Expected: builds clean.

- [ ] **Step 6: Run the full suite + commit**

Run: `./tools/dev.cmd ctest --test-dir build --output-on-failure`
Expected: all green (≥ 98 + the new cases).
```bash
git add src/gui/ClipStatus.h src/gui/MainComponent.cpp tests/test_clipstatus.cpp
git commit -m "feat(ui): qualify a confirmed clip as approximate when OS-resampled (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (run before finishing)

- [ ] Full suite green: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (expect ≥ 98 + the new cases, 0 failed).
- [ ] App builds: `./tools/dev.cmd cmake --build build --target EarsBridge`.
- [ ] Manual smoke (optional, real hardware): start with the EARS selected at its native rate (e.g. 48 kHz) -> the post-start note reads **"Raw-rail verified: EARS at 48.0 kHz, no OS resampling."** Select a non-native rate the OS must convert -> the note reads **"OS-resampled to …"** and the status light stays green (guidance, not invalid). An over-driven sweep on a resampled stream shows the confirmed-clip invalid message **with** the "(OS-resampled - approximate)" caveat.

Then use **superpowers:finishing-a-development-branch** to verify and integrate.

---

## Self-review

**Spec coverage (against `docs/EARS_DIRAC_CLIPPING_AUDIT.md` finding D2 / requirement R2, §7 step 1):**
- R2 / D2 "see exact integer PCM rails" — **native-rate pin + no-SRC assert.** Task 1 reads `getCurrentSampleRate()` back in `openInput` and `rawRailVerified()` asserts `granted == requested` so no SRC engaged on our stream; §7 step 1 "request the EARS native rate and assert `getCurrentSampleRate()==requested`." ✓
- "store the granted depth" (§7 step 1) — Task 1 records `grantedInputBitDepth()` via `getCurrentBitDepth()`. ✓
- "refuse/flag the run as 'OS-resampled — clip detection approximate' rather than silently trusting it" (finding goal) — Task 2 adds the non-invalidating `OsResampled` flag + `reportRawRail`; Task 3 latches it on start; Task 5 appends "(OS-resampled - approximate)" to a confirmed clip. ✓
- "Surface a raw-rail verified vs approximate state to the engine/GUI" — Task 3 `AudioEngine::rawRail()` (engine) + Task 4 `rawRailNote` / MainComponent note (GUI). ✓
- Named alternative documented — WASAPI-exclusive open is stated in the Design decision with why it is deferred (JUCE "Windows Audio" is shared-only; breaks the validated shared-mode config). ✓
- **Deferred (named follow-ups, by design):** D5 sweep-session machine, D6 frozen SRC ratio during the sweep, D7 combine gating, D8 per-block runtime-format revalidation, exclusive-mode open. These do not block the D2 deliverable; the no-clipping claim still needs D5/D6 to be fully defensible. The residual shared-mixer endpoint-volume caveat (D2 nuance) is acknowledged in the wording rather than eliminated — exclusive mode is the only full fix and is explicitly deferred.

**Placeholder scan:** every code step contains complete, compilable content grounded in the real files (`DeviceManager.{h,cpp}`, `AudioEngine.{h,cpp}`, `HealthMonitor.{h,cpp}`, `EngineTypes.h`, `ClipStatus.h`, `MainComponent.cpp`, `tests/CMakeLists.txt`); no "TBD"/"add error handling"/"handle edge cases"/"similar to Task N".

**Type consistency:** `rawRailVerified()`/`requestedInputSampleRate()`/`grantedInputSampleRate()`/`grantedInputBitDepth()` (DeviceManager), `reportRawRail(bool)` (HealthMonitor), `RawRailState{verified,requestedRate,grantedRate,grantedBits}` + `AudioEngine::rawRail()`, `rawRailNote(const RawRailState&)`, and `invalidMeasurementMessage(HealthFlag)->juce::String` are used identically in every task that references them. `HealthFlag::OsResampled` (`1u << 9`) is appended once (Task 2), kept OUT of the invalidating mask, and consumed consistently in Tasks 3 and 5.

**RT-safety:** the only audio-thread-reachable addition is `HealthMonitor::reportRawRail` (a single `raise()` into an atomic, no alloc/lock/log). `analyzeInputBlock`/`scanAndFlagNonFinite` are unchanged. `DeviceManager::openInput`, `AudioEngine::start`, and the GUI note all run on the message thread, not the audio callback.
