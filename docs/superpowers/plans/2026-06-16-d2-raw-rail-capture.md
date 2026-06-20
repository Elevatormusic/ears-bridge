# Raw-Rail Capture (endpoint mix-format SRC detection) Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Make the "confirmed clip" verdict trustworthy at its source by truthfully detecting whether the OS sample-rate converter resampled the EARS capture stream. When no resampling occurred, surface **raw-rail verified**; when it did (or can't be proven), flag the run **"OS-resampled — clip detection approximate"** (a guidance, non-invalidating `HealthFlag`).

**Architecture:** Query the EARS input endpoint's **shared-mode mix-format sample rate** — the rate the OS mixer actually runs the endpoint at, i.e. the rate WASAPI's `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` resamples *to* — and compare it to the rate we requested. If they match, the OS did not resample our stream. This is done with a portable native helper mirroring `src/platform/EndpointUid.*` (Windows: `IPropertyStore` `PKEY_AudioEngine_DeviceFormat`; macOS: CoreAudio `kAudioDevicePropertyNominalSampleRate`). The match/verdict logic is a **pure, unit-testable** predicate; the native query is runtime/on-device (like `EndpointUid`). No new threads, no rewrite.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`. Windows COM (`mmdeviceapi`/`functiondiscoverykeys`-style PROPERTYKEY) + macOS CoreAudio.

This plan implements audit finding **D2** / requirement **R2** (`docs/EARS_DIRAC_CLIPPING_AUDIT.md` §2, the D2 defect, §7 step 1). It supersedes the first D2 attempt, which an xhigh `/code-review` proved was a **no-op on the primary platform**: it compared `juce::AudioIODevice::getCurrentSampleRate()` to the requested rate, but in WASAPI shared mode JUCE sets `currentSampleRate` to the *requested* rate verbatim while `AUTOCONVERTPCM` resamples underneath (`build/_deps/juce-src/.../juce_WASAPI_windows.cpp:1370, 836-838, 768-784, 1338`), so `granted == requested` is a tautology and the feature reported "Raw-rail verified, no OS resampling" exactly when the OS *was* resampling. The corrected signal below is the endpoint mix-format rate, which actually differs from the request when SRC engages.

**Out of scope** (named follow-ups): D5 sweep-session machine, D6 frozen SRC ratio, D7 combine gating, D8 per-block runtime-format revalidation, exclusive-mode open. The residual case where the user has configured the EARS *endpoint* itself at a non-native rate in Windows Sound (so the OS resamples hardware→endpoint before our stream) is acknowledged in the wording, not eliminated — exclusive mode is the only full fix and stays deferred.

## Design decision

**Chosen: compare the requested capture rate to the endpoint's shared mix-format rate.** That mix rate is what the OS mixer runs the capture endpoint at; in shared mode `AUTOCONVERTPCM` resamples between *our requested rate* and that mix rate. If `requested == mixRate` (within 0.5 Hz), AUTOCONVERTPCM is a pass-through and our float samples are the endpoint's samples (raw-rail verified). If they differ — or the mix rate can't be resolved — we do **not** claim verified.

**Why not `getCurrentSampleRate()`** (the reverted attempt): proven a tautology in shared mode (it returns the requested rate). **Why not WASAPI-exclusive open** (the real raw-rails guarantee): JUCE's `"Windows Audio"` type is shared-only (`juce_WASAPI_windows.cpp:1957`); exclusive needs a separate device type and breaks the project's validated shared-mode config. Deferred.

**`HealthFlag::OsResampled` bit:** use the **next free bit in the current `EngineTypes.h` at implementation time** (1u<<9 today). NOTE for the controller: the sibling D6/D8/diag-hosttime/diag-saturation plans each also wrote `1u<<9`; whichever merges must take the next actually-free bit. The implementer MUST read the current enum and pick the next free bit, not hardcode 1u<<9.

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests` — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (loses the MSVC env), never cmd /c.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap alloc, lock, syscall, logging, or exception — plain locals + std::atomic only. (Everything in this plan runs on the message thread, NOT the audio callback — but `reportRawRail` writes the shared `flagBits` atomic.)
- The native COM/CoreAudio query runs on the message thread inside `openInput` (start path), never in a callback.
- `HealthFlag` (src/audio/EngineTypes.h) is NOT persisted — append the new enum value at the next free bit. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic values).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/platform/EndpointFormat.h` | **new** — portable endpoint mix-rate query | declare `double endpointMixSampleRateForName(const juce::String&, bool isInput)` |
| `src/platform/EndpointFormat.cpp` | **new** — Windows WASAPI impl + non-Apple stub | `PKEY_AudioEngine_DeviceFormat` → WAVEFORMATEX::nSamplesPerSec, matched by friendly name (mirrors EndpointUid.cpp); returns 0.0 on any failure |
| `src/platform/EndpointFormat_mac.mm` | **new** — CoreAudio impl | `kAudioDevicePropertyNominalSampleRate` for the matched device UID; returns 0.0 on failure |
| `CMakeLists.txt` | engine sources | add `EndpointFormat.cpp` (all platforms) + the APPLE-gated `EndpointFormat_mac.mm`, mirroring the EndpointUid split |
| `src/audio/DeviceManager.h` | rail surface | members `requestedInRate_`/`endpointMixRate_`; accessors `requestedInputSampleRate()`/`endpointMixSampleRate()`/`rawRailVerified()`; **pure static** `rawRailMatches(double requested, double endpointMixRate)` |
| `src/audio/DeviceManager.cpp` | openInput + closeAll | record requested rate; after open resolve the endpoint mix rate via `endpointMixSampleRateForName`; reset on close; `rawRailVerified()` delegates to the pure predicate |
| `src/audio/EngineTypes.h` | HealthFlag + RawRailState | append `OsResampled` (next free bit, guidance/non-invalidating); add `RawRailState{verified,requestedRate,mixRate}` (NO bit depth — JUCE WASAPI hardcodes getCurrentBitDepth to 32, so it carries no signal) |
| `src/audio/HealthMonitor.{h,cpp}` | telemetry | `reportRawRail(bool verified)`; keep OsResampled OUT of the invalidating mask |
| `src/audio/AudioEngine.{h,cpp}` | start/stop | `rawRail()` snapshot captured after open, latched after `hm.prepare()`; **reset `rawRail_ = {}` in stop() and onDeviceLost()** |
| `src/gui/RawRailStatus.h` | **new** — pure note | `rawRailNote(const RawRailState&)`: **empty when verified** (no news), a message only when OS-resampled or unverifiable — so a clean run doesn't put good-news text in the amber warning label |
| `src/gui/MainComponent.cpp` | post-start note | append the rail note to the existing `notes` StringArray (only fires when there's something to warn about) |
| `src/gui/ClipStatus.h` | clip caveat | `invalidMeasurementMessage` (juce::String) appends `" (OS-resampled - approximate)"` to a confirmed clip when OsResampled is set |
| `tests/*` | unit tests | `rawRailMatches` truth table; OsResampled flag non-invalidation; RawRailState defaults; rawRailNote (empty-when-verified / message-when-not); clip caveat; CMake-register `test_rawrailstatus.cpp` |

---

## Task 1: Portable endpoint mix-rate query (native; runtime-verified)

**Files:**
- Create: `src/platform/EndpointFormat.h`, `src/platform/EndpointFormat.cpp`, `src/platform/EndpointFormat_mac.mm`
- Modify: `CMakeLists.txt` (engine sources — mirror the EndpointUid split: `EndpointUid.cpp` line + the `if(APPLE) ... EndpointUid_mac.mm` block)

**Interfaces:**
- Produces: `double eb::endpointMixSampleRateForName(const juce::String& deviceName, bool isInput)` — the endpoint's shared mix-format sample rate (Hz), or **0.0** when it can't be resolved (unknown platform, name not matched, COM/CoreAudio failure). `isInput` selects the capture (true) vs render (false) flow on Windows; macOS ignores it.

> Note: the native query is **runtime / on-device** verified (no real audio endpoint in headless CI), exactly like `EndpointUid` was ("VERIFIED LIVE via eb_diag" per project memory). Its *contract* (returns 0.0 on failure → caller treats 0.0 as "unverifiable") is what the later pure-logic tasks unit-test. There is no unit test for the native query itself in this task — only that it compiles and links on all platforms.

- [ ] **Step 1: Create the header**

`src/platform/EndpointFormat.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// The shared-mode MIX-FORMAT sample rate the OS runs this endpoint at (Hz). In WASAPI shared mode the
// OS resampler (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) converts between OUR requested rate and this mix
// rate; if our requested rate equals this, the OS did not resample our stream (raw rails). Returns 0.0
// when it can't be resolved (unknown platform, name not matched, COM/CoreAudio failure) so the caller
// treats "unknown" as unverifiable, NOT verified. isInput selects capture (true) vs render (false) on
// Windows; CoreAudio devices are not flow-specific so macOS ignores it.
double endpointMixSampleRateForName (const juce::String& deviceName, bool isInput);

} // namespace eb
```

- [ ] **Step 2: Windows impl + non-Apple stub**

`src/platform/EndpointFormat.cpp` (mirror `EndpointUid.cpp`'s enumerate-and-match-by-friendly-name pattern, but read `PKEY_AudioEngine_DeviceFormat` and parse the `WAVEFORMATEX`):
```cpp
#include "platform/EndpointFormat.h"

#if JUCE_WINDOWS

#include <objbase.h>
#include <mmdeviceapi.h>
#include <mmreg.h>          // WAVEFORMATEX

namespace eb {

double endpointMixSampleRateForName (const juce::String& deviceName, bool isInput) {
    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0},14 (same string JUCE shows).
    const PROPERTYKEY kFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    // PKEY_AudioEngine_DeviceFormat = {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0 -> the device's shared
    // mix format (a WAVEFORMATEX blob). Its nSamplesPerSec is the rate the OS mixer runs the endpoint at.
    const PROPERTYKEY kDeviceFormat = {
        { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);
    double result = 0.0;

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        IMMDeviceCollection* coll = nullptr;
        const EDataFlow flow = isInput ? eCapture : eRender;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            for (UINT i = 0; i < count && result == 0.0; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                        PROPVARIANT name = {};
                        if (SUCCEEDED (store->GetValue (kFriendlyName, &name))
                            && name.vt == VT_LPWSTR && name.pwszVal != nullptr
                            && deviceName == juce::String (name.pwszVal)) {
                            PROPVARIANT fmt = {};
                            if (SUCCEEDED (store->GetValue (kDeviceFormat, &fmt))
                                && fmt.vt == VT_BLOB && fmt.blob.pBlobData != nullptr
                                && fmt.blob.cbSize >= sizeof (WAVEFORMATEX)) {
                                auto* wfx = reinterpret_cast<const WAVEFORMATEX*> (fmt.blob.pBlobData);
                                result = (double) wfx->nSamplesPerSec;
                            }
                            PropVariantClear (&fmt);
                        }
                        if (name.vt == VT_LPWSTR && name.pwszVal != nullptr) CoTaskMemFree (name.pwszVal);
                        store->Release();
                    }
                    dev->Release();
                }
            }
            coll->Release();
        }
        en->Release();
    }
    if (weInited) CoUninitialize();
    return result;
}

} // namespace eb

#elif ! JUCE_MAC   // Linux / other: no mix-rate side channel (macOS impl in EndpointFormat_mac.mm)

namespace eb { double endpointMixSampleRateForName (const juce::String&, bool) { return 0.0; } }

#endif
```

- [ ] **Step 3: macOS impl**

`src/platform/EndpointFormat_mac.mm` (match the device by UID/name, read its nominal rate):
```cpp
#include "platform/EndpointFormat.h"
#if JUCE_MAC
#include <CoreAudio/CoreAudio.h>

namespace eb {

// CoreAudio: the device's nominal sample rate. (The aggregate sub-device's true hardware rate; if the
// requested rate differs, CoreAudio is converting.) Matches the device whose name equals deviceName.
double endpointMixSampleRateForName (const juce::String& deviceName, bool /*isInput*/) {
    AudioObjectPropertyAddress devicesAddr {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &size) != noErr)
        return 0.0;
    const int n = (int) (size / sizeof (AudioDeviceID));
    if (n <= 0) return 0.0;
    juce::HeapBlock<AudioDeviceID> ids (n);
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &size, ids) != noErr)
        return 0.0;

    for (int i = 0; i < n; ++i) {
        AudioObjectPropertyAddress nameAddr {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef cfName = nullptr; UInt32 ns = sizeof (cfName);
        if (AudioObjectGetPropertyData (ids[i], &nameAddr, 0, nullptr, &ns, &cfName) != noErr || cfName == nullptr)
            continue;
        const juce::String nm = juce::String::fromCFString (cfName);
        CFRelease (cfName);
        if (nm != deviceName) continue;

        AudioObjectPropertyAddress rateAddr {
            kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        Float64 rate = 0.0; UInt32 rs = sizeof (rate);
        if (AudioObjectGetPropertyData (ids[i], &rateAddr, 0, nullptr, &rs, &rate) == noErr)
            return (double) rate;
    }
    return 0.0;
}

} // namespace eb
#endif
```

- [ ] **Step 4: Register in CMake (mirror EndpointUid)**

In `CMakeLists.txt`, find the EndpointUid registration:
```cmake
target_sources(eb_engine PRIVATE src/platform/EndpointUid.cpp)        # Windows impl + non-Apple stub
if(APPLE)
    target_sources(eb_engine PRIVATE src/platform/EndpointUid_mac.mm)
    set_source_files_properties(src/platform/EndpointUid_mac.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()
```
Add immediately after it:
```cmake
target_sources(eb_engine PRIVATE src/platform/EndpointFormat.cpp)     # Windows impl + non-Apple stub
if(APPLE)
    target_sources(eb_engine PRIVATE src/platform/EndpointFormat_mac.mm)
    set_source_files_properties(src/platform/EndpointFormat_mac.mm
        PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()
```

- [ ] **Step 5: Build to verify it compiles + links**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` (CMake reconfigures for the new sources).
Expected: builds + links clean (the symbol resolves on Windows from EndpointFormat.cpp). No test yet — the native query is runtime-verified.

- [ ] **Step 6: Commit**
```bash
git add src/platform/EndpointFormat.h src/platform/EndpointFormat.cpp src/platform/EndpointFormat_mac.mm CMakeLists.txt
git commit -m "feat(platform): endpoint mix-format sample-rate query (WASAPI PKEY_AudioEngine_DeviceFormat / CoreAudio)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: DeviceManager records requested rate + endpoint mix rate; pure rawRailMatches predicate

**Files:**
- Modify: `src/audio/DeviceManager.h` (members + accessors + the pure static predicate), `src/audio/DeviceManager.cpp` (openInput + closeAll + rawRailVerified body), `#include "platform/EndpointFormat.h"`
- Test: `tests/test_devicemanager.cpp`

**Interfaces:**
- Produces:
  - `static bool eb::DeviceManager::rawRailMatches(double requestedRate, double endpointMixRate) noexcept` — **pure, unit-testable**: `endpointMixRate > 0.0 && std::abs(requestedRate - endpointMixRate) <= 0.5`. (0.0 mix rate = unresolved = NOT a match.)
  - `double requestedInputSampleRate() const` (0.0 = none/closed), `double endpointMixSampleRate() const` (0.0 = unresolved/closed).
  - `bool rawRailVerified() const` — `inDev != nullptr && rawRailMatches(requestedInRate_, endpointMixRate_)`.

- [ ] **Step 1: Write the failing test (pure predicate + closed-state)**

Append to `tests/test_devicemanager.cpp`:
```cpp
TEST_CASE("DeviceManager::rawRailMatches is a pure no-SRC predicate") {
    using eb::DeviceManager;
    CHECK (DeviceManager::rawRailMatches (48000.0, 48000.0));   // requested == mix -> no SRC
    CHECK (DeviceManager::rawRailMatches (48000.0, 48000.4));   // within 0.5 Hz
    CHECK_FALSE (DeviceManager::rawRailMatches (96000.0, 48000.0)); // OS resamples 96k<->48k
    CHECK_FALSE (DeviceManager::rawRailMatches (48000.0, 0.0));     // mix rate unresolved -> not verified
    CHECK_FALSE (DeviceManager::rawRailMatches (0.0, 0.0));
}

TEST_CASE("DeviceManager raw-rail read-back is zeroed before open and after closeAll") {
    eb::DeviceManager dm;
    CHECK (dm.requestedInputSampleRate() == 0.0);
    CHECK (dm.endpointMixSampleRate()    == 0.0);
    CHECK_FALSE (dm.rawRailVerified());

    eb::DeviceId nope; nope.name = "no-such-input-device-xyz";
    auto err = dm.openInput (nope, 48000.0, 512);   // headless: device can't be created
    CHECK (err.isNotEmpty());
    CHECK (dm.requestedInputSampleRate() == 48000.0);   // request recorded even on failure
    CHECK (dm.endpointMixSampleRate()    == 0.0);        // nothing resolved
    CHECK_FALSE (dm.rawRailVerified());

    dm.closeAll();
    CHECK (dm.requestedInputSampleRate() == 0.0);
    CHECK (dm.endpointMixSampleRate()    == 0.0);
    CHECK_FALSE (dm.rawRailVerified());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: FAIL to compile — `rawRailMatches` / `requestedInputSampleRate` / `endpointMixSampleRate` / `rawRailVerified` undeclared.

- [ ] **Step 3: Header — members, accessors, pure predicate**

In `src/audio/DeviceManager.h`, after the existing output read-back accessors, add:
```cpp
    // ---- Raw-rail (D2): did the OS sample-rate converter resample our INPUT stream? ----
    // In WASAPI/CoreAudio shared mode the OS mixer runs the endpoint at its own mix-format rate and
    // AUTOCONVERTPCM resamples between THAT and our requested rate. We resolve the endpoint mix rate
    // (EndpointFormat.h) and compare: requested == mix => no SRC on our stream (raw rails). NOTE:
    // getCurrentSampleRate() is NOT used here — in shared mode JUCE reports the requested rate verbatim
    // even while the OS resamples (see docs/EARS_DIRAC_CLIPPING_AUDIT.md D2).
    double requestedInputSampleRate() const { return requestedInRate_; }   // 0.0 = none/closed
    double endpointMixSampleRate()    const { return endpointMixRate_; }    // 0.0 = unresolved/closed
    bool   rawRailVerified()          const;                                // open AND requested==mix
    // Pure, side-effect-free verdict (unit-tested without a device). 0.0 mix rate = unresolved = false.
    static bool rawRailMatches (double requestedRate, double endpointMixRate) noexcept;
```
Private members (after the output read-back members):
```cpp
    double requestedInRate_ = 0.0;   // rate last requested on openInput (0 = none/closed)
    double endpointMixRate_ = 0.0;   // endpoint shared mix-format rate resolved at the last input open
```

- [ ] **Step 4: Cpp — openInput, closeAll, predicate**

In `src/audio/DeviceManager.cpp`, add `#include "platform/EndpointFormat.h"` near the top includes, and `#include <cmath>` next to `<algorithm>` (for `std::abs(double)`).

At the TOP of `openInput`, record the request and clear the mix rate (so a failed open still reports the request and resolves nothing):
```cpp
    requestedInRate_ = sampleRate;
    endpointMixRate_ = 0.0;
```
After a SUCCESSFUL `inDev->open(...)` (past the error return), resolve the endpoint mix rate by name:
```cpp
    // Raw-rail (D2): resolve the endpoint's shared mix-format rate to detect OS resampling of our stream.
    endpointMixRate_ = endpointMixSampleRateForName (id.name, true);
```
In `closeAll`, reset both alongside the existing output reset:
```cpp
    requestedInRate_ = 0.0; endpointMixRate_ = 0.0;
```
Add the predicate + verifier (place after openInput):
```cpp
bool DeviceManager::rawRailMatches (double requestedRate, double endpointMixRate) noexcept {
    return endpointMixRate > 0.0 && std::abs (requestedRate - endpointMixRate) <= 0.5;
}
bool DeviceManager::rawRailVerified() const {
    return inDev != nullptr && rawRailMatches (requestedInRate_, endpointMixRate_);
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `./tools/dev.cmd cmake --build build --target eb_tests` then
`./tools/dev.cmd ctest --test-dir build -R "rawRailMatches|raw-rail read-back" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**
```bash
git add src/audio/DeviceManager.h src/audio/DeviceManager.cpp tests/test_devicemanager.cpp
git commit -m "feat(device): resolve endpoint mix rate + pure rawRailMatches no-SRC predicate (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: OsResampled guidance flag + HealthMonitor::reportRawRail

**Files:** `src/audio/EngineTypes.h` (HealthFlag enum), `src/audio/HealthMonitor.{h,cpp}`; Test: `tests/test_healthmonitor.cpp`

**Interfaces:**
- `HealthFlag::OsResampled` at the **next free bit** (read the current enum — it is `1u << 9` today; do not assume) — guidance, NON-invalidating.
- `void eb::HealthMonitor::reportRawRail(bool verified) noexcept` — raises OsResampled iff `!verified`; no-op when verified.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_healthmonitor.cpp`:
```cpp
TEST_CASE("HealthMonitor::reportRawRail raises OsResampled only when NOT verified, never invalidates") {
    using eb::HealthFlag; using eb::any;
    SECTION ("verified => no flag, clean") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (true);
        CHECK_FALSE (any (h.flags() & HealthFlag::OsResampled));
        CHECK (h.cleanCapture());
    }
    SECTION ("not verified => OsResampled, but still VALID (guidance)") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (false);
        CHECK (any (h.flags() & HealthFlag::OsResampled));
        CHECK (h.cleanCapture());
    }
    SECTION ("reset clears it") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (false);
        h.reset();
        CHECK_FALSE (any (h.flags() & HealthFlag::OsResampled));
    }
}
```

- [ ] **Step 2: Run test to verify it fails** — `OsResampled`/`reportRawRail` undeclared.

- [ ] **Step 3: Append the enum value (next free bit)**

Read `src/audio/EngineTypes.h`; append after the current last `HealthFlag` value (today `NonFinite = 1u << 8`), giving it the next free bit with a comment:
```cpp
    OsResampled = 1u << 9   // guidance: OS SRC resampled the INPUT (D2) -> clip detection approximate; NOT invalidating
```
(If the enum already uses 1u<<9 from a sibling slice, use the next free bit instead — verify against the file.)

- [ ] **Step 4: Declare + implement reportRawRail; leave the invalidating mask unchanged**

In `HealthMonitor.h` (after `reportOutLevel`):
```cpp
    void reportRawRail (bool verified) noexcept;   // D2: raises OsResampled (guidance) when !verified
```
In `HealthMonitor.cpp` (after `reportOutLevel`):
```cpp
void HealthMonitor::reportRawRail (bool verified) noexcept {
    if (! verified) raise (HealthFlag::OsResampled);   // guidance only; not in the invalidating mask
}
```
Confirm `raise()`'s invalidating mask still = {Xrun, Dropout, ExcessDrift, FifoStarved, ClipConfirmed, NonFinite} — do NOT add OsResampled.

- [ ] **Step 5: Run tests** — `./tools/dev.cmd ctest --test-dir build -R "reportRawRail" --output-on-failure` → PASS.

- [ ] **Step 6: Commit**
```bash
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "feat(health): OsResampled guidance flag + reportRawRail (non-invalidating) (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: AudioEngine RawRailState snapshot; latch on start; reset on stop

**Files:** `src/audio/EngineTypes.h` (RawRailState), `src/audio/AudioEngine.{h,cpp}`; Test: `tests/test_audioengine.cpp`

**Interfaces:**
- `struct eb::RawRailState { bool verified=false; double requestedRate=0.0; double mixRate=0.0; }` (NO bit depth — JUCE WASAPI hardcodes getCurrentBitDepth()=32, so it carries no signal; the previous attempt's grantedBits was dead plumbing).
- `eb::RawRailState eb::AudioEngine::rawRail() const noexcept` — captured at the last successful start(); default = unverified; **reset to `{}` in stop() and onDeviceLost()** so a stale snapshot never outlives its device.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_audioengine.cpp`:
```cpp
TEST_CASE("AudioEngine::rawRail defaults to unverified before any run") {
    eb::AudioEngine e;
    auto rr = e.rawRail();
    CHECK_FALSE (rr.verified);
    CHECK (rr.requestedRate == 0.0);
    CHECK (rr.mixRate == 0.0);
}
```

- [ ] **Step 2: Run test to verify it fails** — `AudioEngine::rawRail` / `RawRailState` undeclared.

- [ ] **Step 3: Struct (EngineTypes.h)** — after `Health`:
```cpp
// --- D2: raw-rail capture state, captured once per run by AudioEngine::start ---
// verified == true => the EARS input ran at the endpoint mix rate (no OS SRC on our stream), so the
// float samples faithfully represent the device's integer rails. verified == false => OS-resampled or
// the mix rate could not be resolved -> clip detection is approximate.
struct RawRailState {
    bool   verified      = false;
    double requestedRate = 0.0;
    double mixRate       = 0.0;
};
```

- [ ] **Step 4: Accessor, member, latch on start, reset on stop**

In `AudioEngine.h` (after `grantedOutputBitDepth()`):
```cpp
    RawRailState rawRail() const noexcept;
```
Private member:
```cpp
    RawRailState rawRail_;   // D2: captured once per successful start(); reset in stop()/onDeviceLost()
```
In `AudioEngine.cpp`, accessor:
```cpp
RawRailState AudioEngine::rawRail() const noexcept { return rawRail_; }
```
In `start()`, after the input opens and on the converged success path (near `grantedRate_ = capRate;`), capture the snapshot — but call `hm.reportRawRail` AFTER `hm.prepare()` (which resets flagBits):
```cpp
    rawRail_ = RawRailState { devices.rawRailVerified(),
                              devices.requestedInputSampleRate(),
                              devices.endpointMixSampleRate() };
```
Immediately after the existing `hm.prepare (...)` line:
```cpp
    hm.reportRawRail (rawRail_.verified);
```
In `stop()` and `onDeviceLost()`, reset the snapshot (mirroring how devices/state are torn down):
```cpp
    rawRail_ = RawRailState {};
```

- [ ] **Step 5: Run tests** — `./tools/dev.cmd ctest --test-dir build -R "rawRail defaults" --output-on-failure` → PASS; then full suite green.

- [ ] **Step 6: Commit**
```bash
git add src/audio/EngineTypes.h src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "feat(engine): RawRailState snapshot; latch on start, reset on stop/device-loss (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: GUI note (silent when verified) + confirmed-clip caveat

**Files:** Create `src/gui/RawRailStatus.h`; Modify `src/gui/MainComponent.cpp` (post-start notes), `src/gui/ClipStatus.h`; Create+register `tests/test_rawrailstatus.cpp`; append to `tests/test_clipstatus.cpp`.

**Interfaces:**
- `juce::String eb::rawRailNote(const RawRailState&)` — **EMPTY when verified** (good news doesn't belong in the amber warning label) and empty before any run (mixRate==0 AND requestedRate==0); a message only when OS-resampled (`requestedRate` and `mixRate` both > 0 and differ) or unverifiable (mixRate==0 after a run).
- `invalidMeasurementMessage(HealthFlag) -> juce::String` appends `" (OS-resampled - approximate)"` to the confirmed-clip message when OsResampled is set.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_rawrailstatus.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "gui/RawRailStatus.h"

TEST_CASE("rawRailNote is silent when verified, warns only when resampled/unverifiable") {
    using eb::RawRailState; using eb::rawRailNote;
    SECTION ("no run yet => empty") { CHECK (rawRailNote (RawRailState{}).isEmpty()); }
    SECTION ("verified => empty (no news is good news)") {
        RawRailState rr; rr.verified = true; rr.requestedRate = 48000.0; rr.mixRate = 48000.0;
        CHECK (rawRailNote (rr).isEmpty());
    }
    SECTION ("OS-resampled => approximate caveat naming both rates") {
        RawRailState rr; rr.verified = false; rr.requestedRate = 96000.0; rr.mixRate = 48000.0;
        CHECK (rawRailNote (rr)
               == juce::String ("OS-resampled to 48.0 kHz (endpoint mix rate) vs the requested 96.0 kHz "
                                "- clip detection approximate."));
    }
    SECTION ("unverifiable (mix rate unknown) => honest caveat") {
        RawRailState rr; rr.verified = false; rr.requestedRate = 48000.0; rr.mixRate = 0.0;
        CHECK (rawRailNote (rr)
               == juce::String ("Could not confirm the EARS endpoint rate - clip detection approximate."));
    }
}
```
Register `test_rawrailstatus.cpp` in `tests/CMakeLists.txt` (after `test_clipstatus.cpp`).
Append to `tests/test_clipstatus.cpp` (note: existing cases wrap the result in `.toStdString()` already if a prior slice changed the return type; if `invalidMeasurementMessage` still returns `const char*` here, this task changes it to `juce::String` and you MUST convert any `std::string(invalidMeasurementMessage(...))` wrapper to `.toStdString()`):
```cpp
TEST_CASE("invalidMeasurementMessage qualifies a confirmed clip as approximate when OS-resampled") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK (invalidMeasurementMessage (HealthFlag::ClipConfirmed | HealthFlag::OsResampled).toStdString()
           == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat. "
              "(OS-resampled - approximate)");
    CHECK (invalidMeasurementMessage (HealthFlag::Dropout | HealthFlag::OsResampled).toStdString()
           == "Dropouts detected - this measurement is invalid.");
}
```

- [ ] **Step 2: Run to verify it fails** — header missing / caveat absent.

- [ ] **Step 3: Create the note helper**

`src/gui/RawRailStatus.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"

namespace eb {

// Honest post-start note for the raw-rail (D2) state. SILENT when verified or before a run (so a clean
// run leaves the warning label blank); speaks only when the OS resampled our stream or the endpoint
// rate could not be confirmed. Header-only + pure (mirrors gui/ClipStatus.h). See AUDIT D2.
[[nodiscard]] inline juce::String rawRailNote (const RawRailState& rr) {
    if (rr.verified) return {};                       // no SRC on our stream -> no warning
    if (rr.requestedRate <= 0.0) return {};           // no run yet
    if (rr.mixRate > 0.0)
        return "OS-resampled to " + juce::String (rr.mixRate / 1000.0, 1)
             + " kHz (endpoint mix rate) vs the requested " + juce::String (rr.requestedRate / 1000.0, 1)
             + " kHz - clip detection approximate.";
    return "Could not confirm the EARS endpoint rate - clip detection approximate.";
}

} // namespace eb
```

- [ ] **Step 4: Wire the GUI note + the clip caveat**

In `src/gui/MainComponent.cpp`, add `#include "gui/RawRailStatus.h"` by `gui/ClipStatus.h`, and in the post-start `notes` block append:
```cpp
    const auto railNote = eb::rawRailNote (engine.rawRail());
    if (railNote.isNotEmpty()) notes.add (railNote);
```
In `src/gui/ClipStatus.h`, make `invalidMeasurementMessage` return `juce::String` (if not already) and append the caveat in the ClipConfirmed branch:
```cpp
[[nodiscard]] inline juce::String invalidMeasurementMessage (HealthFlag flags) {
    if (any (flags & HealthFlag::ClipConfirmed)) {
        juce::String msg = "Input reached digital full scale - this measurement is invalid. "
                           "Lower the level and repeat.";
        if (any (flags & HealthFlag::OsResampled)) msg += " (OS-resampled - approximate)";
        return msg;
    }
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    return "Dropouts detected - this measurement is invalid.";
}
```
If the return type changed from `const char*`, fix the call site (`statusLine.setText(... , ...)` binds a juce::String fine) and convert any `std::string(invalidMeasurementMessage(...))` test wrappers to `.toStdString()`.

- [ ] **Step 5: Run tests + build app** — focused `rawRailNote|qualifies a confirmed clip`, then full suite, then `./tools/dev.cmd cmake --build build --target EarsBridge`.

- [ ] **Step 6: Commit**
```bash
git add src/gui/RawRailStatus.h src/gui/ClipStatus.h src/gui/MainComponent.cpp tests/test_rawrailstatus.cpp tests/test_clipstatus.cpp tests/CMakeLists.txt
git commit -m "feat(ui): silent-when-verified raw-rail note + approximate clip caveat (D2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (run before finishing)

- [ ] Full suite green: `./tools/dev.cmd ctest --test-dir build --output-on-failure`.
- [ ] App builds: `./tools/dev.cmd cmake --build build --target EarsBridge`.
- [ ] **On-device smoke (required — the native query is not headless-testable):** with the EARS endpoint at its native rate in Windows Sound and the app requesting that rate → no rail note (silent = verified). Set the EARS endpoint to a *different* rate in Windows Sound (forcing AUTOCONVERTPCM) → the note reads "OS-resampled to NN.N kHz (endpoint mix rate) vs the requested MM.M kHz - clip detection approximate" and a confirmed clip shows the "(OS-resampled - approximate)" caveat; the status light stays green (guidance, not invalid). Optionally print `endpointMixSampleRateForName` via an eb_diag temp line to confirm it resolves a non-requested rate (the bug the reverted attempt could not see).

Then use **superpowers:finishing-a-development-branch**.

## Self-review

**Spec coverage (audit D2 / R2 / §7 step 1):** detects OS SRC by the endpoint mix rate (the signal that actually differs when AUTOCONVERTPCM engages), not `getCurrentSampleRate()` (proven a no-op in shared mode by the xhigh review that reverted the first attempt). Verified vs approximate surfaced to engine (OsResampled flag, RawRailState) and GUI (silent-when-verified note, clip caveat). Native query mirrors the validated EndpointUid portable split; runtime-verified like EndpointUid. ✓

**Review-fix coverage (from the reverted attempt's /code-review):** (1) tautology root cause → replaced with the mix-rate compare; (2) duplicate resample note → the note is now silent when verified and the resampled case keys on requested-vs-mix (distinct from the existing granted-downgrade note); (3) good-news in the amber warning label → rawRailNote returns empty when verified; (4) dead grantedBits plumbing → removed (no bit depth in RawRailState); (5) rawRail_ stale after stop → reset in stop()/onDeviceLost(); (6) bit-9 collision → use the next free bit, with a controller note that the sibling D-track plans must each take a distinct bit. ✓

**Placeholder scan:** every code step is complete + grounded in the real files (EndpointUid.cpp pattern, DeviceManager/AudioEngine/HealthMonitor/EngineTypes/ClipStatus/MainComponent, CMake EndpointUid split). No TBD/"add error handling"/"similar to Task N".

**Type consistency:** `endpointMixSampleRateForName(juce::String,bool)->double`, `rawRailMatches(double,double)->bool` (static), `requestedInputSampleRate()/endpointMixSampleRate()/rawRailVerified()`, `reportRawRail(bool)`, `RawRailState{verified,requestedRate,mixRate}` + `AudioEngine::rawRail()`, `rawRailNote(const RawRailState&)->juce::String`, `invalidMeasurementMessage(HealthFlag)->juce::String` are used identically across tasks. `HealthFlag::OsResampled` appended once at the next free bit, kept OUT of the invalidating mask.

**RT-safety:** no audio-thread additions except `reportRawRail` (a single atomic `raise()`); the native query, snapshot, and GUI note all run on the message thread.
