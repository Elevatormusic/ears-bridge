# Update Notification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On launch, check GitHub for a newer EARS Bridge release in the background and, if one exists, show a subtle non-modal "Update available · vX.Y.Z →" link in the title bar that opens the release page — on by default, with an Advanced opt-out, never auto-downloading.

**Architecture:** A small isolated `UpdateChecker` unit (`src/net/`) does the work off the message thread: fetch GitHub's `releases/latest`, parse the tag, compare to the running version (a pure, unit-tested `isNewer`), and report back on the message thread via a cancel-safe `MessageManager::callAsync`. `MainComponent` owns it, gates it on a Settings toggle + a 24 h throttle, and shows/positions the link. Two new Settings keys persist the opt-out and the last-check timestamp.

**Tech Stack:** C++20, JUCE 8.0.4 (`juce::URL`/`WebInputStream`/`JSON`/`Thread`/`MessageManager`/`HyperlinkButton`), Catch2 v3, CMake. Networking uses the native Win (WinINet) / mac (NSURLSession) backends, so `JUCE_USE_CURL=0` is fine — no new module or CMake networking change.

---

## File structure

- **`src/net/UpdateChecker.h`** (new) — public interface: `isNewer()` (pure compare), `UpdateInfo` (result POD), `parseRelease()` (pure JSON→UpdateInfo), and the `UpdateChecker` class (async network fetch).
- **`src/net/UpdateChecker.cpp`** (new) — implementations.
- **`tests/test_updatecheck.cpp`** (new) — Catch2 tests for `isNewer()` and `parseRelease()` (the pure logic; the live network call is not unit-tested).
- **`src/state/Settings.{h,cpp}`** (modify) — add `autoCheckUpdates()` and `lastUpdateCheck()` accessors.
- **`src/gui/MainComponent.{h,cpp}`** (modify) — title-bar `updateLink`, Advanced `autoUpdateToggle`, throttled kickoff + result callback, layout.
- **`CMakeLists.txt`** (modify, additive) — add `UpdateChecker.cpp` to `eb_gui`; add `EB_VERSION_STRING` compile def.
- **`tests/CMakeLists.txt`** (modify, additive) — add `test_updatecheck.cpp` to `eb_tests`.
- **`README.md`** (modify) — one feature-list line + a privacy note.

**Build/test commands** (run from the repo root via the MSVC-env wrapper; the `build/` dir already exists):
- Build a target: `./tools/dev.cmd cmake --build build --target <name>` (re-configures automatically when CMakeLists changes).
- Run the test binary: `build/tests/eb_tests.exe "<filter>"` (Catch2 tag/name filter), or all tests via `./tools/dev.cmd ctest --test-dir build --output-on-failure`.

---

## Task 1: `isNewer()` — pure version comparison

**Files:**
- Create: `src/net/UpdateChecker.h`
- Create: `src/net/UpdateChecker.cpp`
- Modify: `CMakeLists.txt` (add source to `eb_gui` + `EB_VERSION_STRING` def)
- Modify: `tests/CMakeLists.txt` (add test file)
- Test: `tests/test_updatecheck.cpp`

- [ ] **Step 1: Create the header with `isNewer` declared**

`src/net/UpdateChecker.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Returns true iff `latestTag` is a strictly newer semantic version than `currentVersion`.
// Accepts an optional leading 'v'/'V' and ignores any pre-release/build suffix after patch
// (e.g. "v0.2.13-beta" -> 0.2.13). Malformed/empty `latestTag` -> false.
bool isNewer (juce::String currentVersion, juce::String latestTag);

} // namespace eb
```

- [ ] **Step 2: Create the .cpp with a STUB `isNewer` (so it links but fails the test)**

`src/net/UpdateChecker.cpp`:
```cpp
#include "net/UpdateChecker.h"

namespace eb {

bool isNewer (juce::String, juce::String) { return false; }   // STUB — real impl in Step 6

} // namespace eb
```

- [ ] **Step 3: Register the source + version macro in CMake (additive)**

In `CMakeLists.txt`, add `src/net/UpdateChecker.cpp` to the `eb_gui` `add_library` list (the block starting `add_library(eb_gui STATIC`, currently ending at `src/platform/DiracCompat.cpp)`):
```cmake
add_library(eb_gui STATIC
    src/state/Settings.cpp
    src/gui/Theme.cpp
    src/gui/LevelMeter.cpp
    src/gui/CurveThumbnail.cpp
    src/gui/CalSlotComponent.cpp
    src/gui/DevicePicker.cpp
    src/gui/MainComponent.cpp
    src/net/UpdateChecker.cpp
    src/platform/DiracCompat.cpp)   # Windows: Dirac shared-mode env-var helper (no-op elsewhere)
```
Then, immediately after the existing `target_compile_definitions(eb_gui PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)` block, add:
```cmake
# Single source of truth for the running version (PROJECT_VERSION from project(...) above),
# used by the update checker. eb_gui is a plain static lib, so JUCE's generated ProjectInfo
# is not available here — this macro replaces it.
target_compile_definitions(eb_gui PRIVATE EB_VERSION_STRING="${PROJECT_VERSION}")
```

- [ ] **Step 4: Register the test file in CMake (additive)**

In `tests/CMakeLists.txt`, add `test_updatecheck.cpp` to the `eb_tests` `add_executable` source list (after `test_calslot.cpp)`):
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
    test_settings.cpp
    test_plotmath.cpp
    test_ratemenu.cpp
    test_calbinder.cpp
    test_asiofallback.cpp
    test_lrverify.cpp
    test_levelmeter.cpp
    test_calslot.cpp
    test_updatecheck.cpp)
```

- [ ] **Step 5: Write the failing test for `isNewer`**

`tests/test_updatecheck.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "net/UpdateChecker.h"

TEST_CASE ("UpdateCheck isNewer compares versions numerically", "[update]") {
    CHECK (eb::isNewer ("0.2.12", "v0.2.13") == true);
    CHECK (eb::isNewer ("0.2.12", "v0.2.12") == false);
    CHECK (eb::isNewer ("0.2.12", "v0.2.11") == false);
    CHECK (eb::isNewer ("0.2.9",  "v0.2.10") == true);   // numeric, not lexical
    CHECK (eb::isNewer ("0.2.12", "v0.3.0")  == true);
    CHECK (eb::isNewer ("0.2.12", "1.0.0")   == true);   // no 'v' prefix
    CHECK (eb::isNewer ("0.2.12", "v0.2.13-beta") == true);   // suffix ignored
    CHECK (eb::isNewer ("0.2.12", "garbage") == false);
    CHECK (eb::isNewer ("0.2.12", "")        == false);
}
```

- [ ] **Step 6: Run the test and confirm it FAILS**

Run:
```bash
./tools/dev.cmd cmake --build build --target eb_tests
build/tests/eb_tests.exe "[update]"
```
Expected: FAIL — the stub returns `false`, so the `== true` cases (e.g. `isNewer("0.2.12","v0.2.13")`) fail.

- [ ] **Step 7: Implement `isNewer` for real**

Replace the stub in `src/net/UpdateChecker.cpp` with:
```cpp
#include "net/UpdateChecker.h"

namespace eb {

namespace {
    // Parse up to three integer components from "x.y.z[-suffix]" (leading 'v' stripped).
    // juce::String::getIntValue() reads the leading integer and ignores trailing non-digits,
    // so "13-beta" -> 13 and "garbage" -> 0.
    void parseVersion (juce::String s, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        s = s.trim();
        if (s.startsWithIgnoreCase ("v")) s = s.substring (1);
        auto parts = juce::StringArray::fromTokens (s, ".", "");
        for (int i = 0; i < 3 && i < parts.size(); ++i)
            out[i] = parts[i].getIntValue();
    }
}

bool isNewer (juce::String currentVersion, juce::String latestTag) {
    if (latestTag.trim().isEmpty()) return false;
    int cur[3], latest[3];
    parseVersion (currentVersion, cur);
    parseVersion (latestTag, latest);
    for (int i = 0; i < 3; ++i)
        if (latest[i] != cur[i]) return latest[i] > cur[i];
    return false;
}

} // namespace eb
```

- [ ] **Step 8: Run the test and confirm it PASSES**

Run:
```bash
./tools/dev.cmd cmake --build build --target eb_tests
build/tests/eb_tests.exe "[update]"
```
Expected: PASS (all assertions in `UpdateCheck isNewer ...`).

- [ ] **Step 9: Commit**

```bash
git add src/net/UpdateChecker.h src/net/UpdateChecker.cpp tests/test_updatecheck.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(update): pure isNewer() semantic-version compare + tests"
```

---

## Task 2: `parseRelease()` — pure JSON → result

**Files:**
- Modify: `src/net/UpdateChecker.h`
- Modify: `src/net/UpdateChecker.cpp`
- Test: `tests/test_updatecheck.cpp`

- [ ] **Step 1: Add `UpdateInfo` + `parseRelease` to the header**

In `src/net/UpdateChecker.h`, add inside `namespace eb` (after the `isNewer` declaration):
```cpp
// Result of an update check.
struct UpdateInfo {
    bool reachedServer   = false;   // true once GitHub returned any response body
    bool updateAvailable = false;
    juce::String latestVersion;     // e.g. "0.2.13" (no leading 'v')
    juce::String releaseUrl;        // the release's html_url, for the browser
};

// Parse a GitHub `releases/latest` JSON body and decide whether it is newer than
// `currentVersion`. Pure (no network). A body with no "tag_name" (e.g. a rate-limit
// error) or unparseable JSON yields { reachedServer=true, updateAvailable=false }.
UpdateInfo parseRelease (const juce::String& jsonBody, const juce::String& currentVersion);
```

- [ ] **Step 2: Add a STUB `parseRelease` to the .cpp**

In `src/net/UpdateChecker.cpp`, add inside `namespace eb` (after `isNewer`):
```cpp
UpdateInfo parseRelease (const juce::String&, const juce::String&) { return {}; }   // STUB
```

- [ ] **Step 3: Write the failing test for `parseRelease`**

Append to `tests/test_updatecheck.cpp`:
```cpp
TEST_CASE ("UpdateCheck parseRelease reads tag and url", "[update]") {
    const juce::String body =
        R"({"tag_name":"v0.2.13","html_url":"https://github.com/Elevatormusic/ears-bridge/releases/tag/v0.2.13"})";

    SECTION ("newer release") {
        auto info = eb::parseRelease (body, "0.2.12");
        CHECK (info.reachedServer);
        CHECK (info.updateAvailable);
        CHECK (info.latestVersion == "0.2.13");
        CHECK (info.releaseUrl == "https://github.com/Elevatormusic/ears-bridge/releases/tag/v0.2.13");
    }
    SECTION ("same version -> no update") {
        auto info = eb::parseRelease (body, "0.2.13");
        CHECK (info.reachedServer);
        CHECK_FALSE (info.updateAvailable);
    }
    SECTION ("rate-limit error JSON (no tag_name)") {
        auto info = eb::parseRelease (R"({"message":"API rate limit exceeded"})", "0.2.12");
        CHECK (info.reachedServer);
        CHECK_FALSE (info.updateAvailable);
    }
    SECTION ("garbage body") {
        auto info = eb::parseRelease ("not json", "0.2.12");
        CHECK (info.reachedServer);
        CHECK_FALSE (info.updateAvailable);
    }
}
```

- [ ] **Step 4: Run the test and confirm it FAILS**

Run:
```bash
./tools/dev.cmd cmake --build build --target eb_tests
build/tests/eb_tests.exe "[update]"
```
Expected: FAIL — the stub returns a default `UpdateInfo` (`reachedServer=false`, `updateAvailable=false`), so the `reachedServer` and `updateAvailable`/`latestVersion` checks fail.

- [ ] **Step 5: Implement `parseRelease` for real**

Replace the `parseRelease` stub in `src/net/UpdateChecker.cpp` with:
```cpp
UpdateInfo parseRelease (const juce::String& jsonBody, const juce::String& currentVersion) {
    UpdateInfo info;
    info.reachedServer = true;                      // we have a response body
    auto json = juce::JSON::parse (jsonBody);
    auto tag  = json.getProperty ("tag_name", juce::var()).toString();
    if (tag.isEmpty()) return info;                 // error/rate-limit JSON has no tag_name
    if (isNewer (currentVersion, tag)) {
        info.updateAvailable = true;
        info.latestVersion = tag.startsWithIgnoreCase ("v") ? tag.substring (1) : tag;
        auto html = json.getProperty ("html_url", juce::var()).toString();
        info.releaseUrl = html.isNotEmpty()
            ? html
            : juce::String ("https://github.com/Elevatormusic/ears-bridge/releases");
    }
    return info;
}
```

- [ ] **Step 6: Run the test and confirm it PASSES**

Run:
```bash
./tools/dev.cmd cmake --build build --target eb_tests
build/tests/eb_tests.exe "[update]"
```
Expected: PASS (both `UpdateCheck ...` test cases).

- [ ] **Step 7: Commit**

```bash
git add src/net/UpdateChecker.h src/net/UpdateChecker.cpp tests/test_updatecheck.cpp
git commit -m "feat(update): parseRelease() GitHub JSON parsing + tests"
```

---

## Task 3: `UpdateChecker` — cancel-safe async network fetch

**Files:**
- Modify: `src/net/UpdateChecker.h`
- Modify: `src/net/UpdateChecker.cpp`

(No unit test: this is the live-network/threading layer; its logic is `isNewer` + `parseRelease`, already covered. Verification is a clean compile.)

- [ ] **Step 1: Add the `UpdateChecker` class to the header**

In `src/net/UpdateChecker.h`, add these includes at the top (under the existing `#include <juce_core/juce_core.h>`):
```cpp
#include <juce_events/juce_events.h>   // juce::MessageManager (callback marshalling)
#include <atomic>
#include <functional>
#include <memory>
```
Then add inside `namespace eb` (after `parseRelease`):
```cpp
// One-shot, cancel-safe update check. start() fetches GitHub's releases/latest on a
// background thread and invokes onDone on the MESSAGE thread. If this object is destroyed
// before the fetch returns, the callback is silently dropped (so a destroyed owner is never
// called). Fire-and-forget: safe to keep as a value member.
class UpdateChecker {
public:
    UpdateChecker() : alive (std::make_shared<std::atomic<bool>> (true)) {}
    ~UpdateChecker() { alive->store (false); }

    void start (juce::String currentVersion, std::function<void (UpdateInfo)> onDone);

private:
    std::shared_ptr<std::atomic<bool>> alive;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};
```

- [ ] **Step 2: Implement `start()` in the .cpp**

In `src/net/UpdateChecker.cpp`, add inside `namespace eb` (after `parseRelease`):
```cpp
void UpdateChecker::start (juce::String currentVersion, std::function<void (UpdateInfo)> onDone) {
    auto a = alive;
    juce::Thread::launch ([currentVersion, onDone = std::move (onDone), a]() mutable {
        UpdateInfo info;
        juce::URL url ("https://api.github.com/repos/Elevatormusic/ears-bridge/releases/latest");
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (3000)
                        .withExtraHeaders ("User-Agent: EARS-Bridge/" + currentVersion
                                           + "\r\nAccept: application/vnd.github+json");
        if (auto stream = url.createInputStream (opts))
            info = parseRelease (stream->readEntireStreamAsString(), currentVersion);
        // else: offline / connect failed -> info.reachedServer stays false (retry next launch)

        juce::MessageManager::callAsync ([onDone = std::move (onDone), a, info]() mutable {
            if (a->load()) onDone (info);
        });
    });
}
```

- [ ] **Step 3: Verify it compiles (build the app)**

Run:
```bash
./tools/dev.cmd cmake --build build --target EarsBridge
```
Expected: builds with no errors/warnings (JUCE recommended-warning-flags are on).

- [ ] **Step 4: Commit**

```bash
git add src/net/UpdateChecker.h src/net/UpdateChecker.cpp
git commit -m "feat(update): UpdateChecker async GitHub fetch (cancel-safe)"
```

---

## Task 4: Settings — `autoCheckUpdates` + `lastUpdateCheck`

**Files:**
- Modify: `src/state/Settings.h`
- Modify: `src/state/Settings.cpp`
- Test: `tests/test_settings.cpp`

- [ ] **Step 1: Declare the accessors in the header**

In `src/state/Settings.h`, add to the `public:` section of `class Settings` (after `bool complexPhase() const; void setComplexPhase (bool);`):
```cpp
    bool         autoCheckUpdates() const; void setAutoCheckUpdates (bool);
    juce::int64  lastUpdateCheck() const;  void setLastUpdateCheck (juce::int64);   // unix seconds
```

- [ ] **Step 2: Write the failing test**

Append to `tests/test_settings.cpp` (uses the same temp-dir pattern as the existing cases in that file — construct `eb::Settings s { tempDir };`). Add:
```cpp
TEST_CASE ("Settings persists update-check preferences", "[update]") {
    juce::TemporaryFile tmp;
    auto dir = tmp.getFile().getParentDirectory();

    {
        eb::Settings s { dir };
        CHECK (s.autoCheckUpdates() == true);   // default ON
        CHECK (s.lastUpdateCheck() == 0);       // default 0
        s.setAutoCheckUpdates (false);
        s.setLastUpdateCheck (1750000000);
        s.flush();
    }
    {
        eb::Settings s { dir };                 // reload from disk
        CHECK (s.autoCheckUpdates() == false);
        CHECK (s.lastUpdateCheck() == 1750000000);
    }
}
```

- [ ] **Step 3: Run the test and confirm it FAILS**

Run:
```bash
./tools/dev.cmd cmake --build build --target eb_tests
build/tests/eb_tests.exe "[update]"
```
Expected: FAIL to **compile/link** — `autoCheckUpdates`/`lastUpdateCheck` are declared but not defined.

- [ ] **Step 4: Implement the accessors**

In `src/state/Settings.cpp`, add two keys to the anonymous-namespace block at the top (after `kComplex`):
```cpp
    constexpr const char* kAutoCheck  = "autoCheckUpdates";
    constexpr const char* kLastCheck  = "lastUpdateCheck";
```
Then add the definitions (after `setComplexPhase`):
```cpp
bool Settings::autoCheckUpdates() const { return file->getBoolValue (kAutoCheck, true); }
void Settings::setAutoCheckUpdates (bool b) { file->setValue (kAutoCheck, b); }

// Stored as a string (PropertiesFile has no int64 setter); getLargeIntValue() reads it back.
juce::int64 Settings::lastUpdateCheck() const { return file->getValue (kLastCheck, "0").getLargeIntValue(); }
void Settings::setLastUpdateCheck (juce::int64 secs) { file->setValue (kLastCheck, juce::String (secs)); }
```

- [ ] **Step 5: Run the test and confirm it PASSES**

Run:
```bash
./tools/dev.cmd cmake --build build --target eb_tests
build/tests/eb_tests.exe "[update]"
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/state/Settings.h src/state/Settings.cpp tests/test_settings.cpp
git commit -m "feat(update): Settings.autoCheckUpdates + lastUpdateCheck"
```

---

## Task 5: MainComponent — title-bar link, Advanced toggle, throttled kickoff

**Files:**
- Modify: `src/gui/MainComponent.h`
- Modify: `src/gui/MainComponent.cpp` (constructor ~L34, Advanced layout ~L853, title-bar `resized()` ~L805)

(No unit test — GUI wiring; verification is a clean build + launch. The logic underneath is covered by Tasks 1–4.)

- [ ] **Step 1: Add the include + members to the header**

In `src/gui/MainComponent.h`, add the include (after `#include "gui/RateMenu.h"`):
```cpp
#include "net/UpdateChecker.h"
```
In the `private:` section, after `juce::Label statusLine;`, add:
```cpp
    // Non-modal "Update available" link shown in the title bar when a newer release exists.
    juce::HyperlinkButton updateLink;
    UpdateChecker         updateChecker;
```
And in the Advanced disclosure group (after `juce::ToggleButton complexPhaseToggle { ... };`), add:
```cpp
    juce::ToggleButton autoUpdateToggle { "Automatically check for updates" };
```

- [ ] **Step 2: Wire the link, toggle, and kickoff in the constructor**

In `src/gui/MainComponent.cpp`, in `MainComponent::MainComponent()` (starts L34), after `addAndMakeVisible (statusLine);` (L51) add:
```cpp
    // Update link: hidden until a newer release is found; opens the release page in the browser.
    updateLink.setColour (juce::HyperlinkButton::textColourId, theme.accent());
    updateLink.setJustificationType (juce::Justification::centredRight);
    updateLink.setVisible (false);
    addAndMakeVisible (updateLink);
```
Where the Advanced controls are set up (near `advancedToggle.onClick` at L137), add:
```cpp
    autoUpdateToggle.setToggleState (settings.autoCheckUpdates(), juce::dontSendNotification);
    autoUpdateToggle.onClick = [this] {
        settings.setAutoCheckUpdates (autoUpdateToggle.getToggleState());
        settings.flush();
        if (! autoUpdateToggle.getToggleState() && updateLink.isVisible()) {
            updateLink.setVisible (false);
            resized();
        }
    };
    addAndMakeVisible (autoUpdateToggle);
```
At the END of the constructor body (after all other setup), add the throttled check:
```cpp
    // Background update check: once per launch, but at most one successful check per 24 h.
    {
        const juce::int64 nowSecs = juce::Time::getCurrentTime().toMilliseconds() / 1000;
        const juce::int64 kDaySecs = 24 * 60 * 60;
        if (settings.autoCheckUpdates() && nowSecs - settings.lastUpdateCheck() >= kDaySecs) {
            updateChecker.start (juce::String (EB_VERSION_STRING),
                [this, nowSecs] (UpdateInfo info) {
                    if (info.reachedServer) {
                        settings.setLastUpdateCheck (nowSecs);
                        settings.flush();
                    }
                    if (info.updateAvailable) {
                        updateLink.setButtonText ("Update available · v" + info.latestVersion + "  →");
                        updateLink.setURL (juce::URL (info.releaseUrl));
                        updateLink.setVisible (true);
                        resized();
                    }
                });
        }
    }
```

- [ ] **Step 3: Lay out the title-bar link in `resized()`**

In `MainComponent::resized()` (L797), in the title-bar row (where `startStop` and `statusLine` are placed, L805–809), replace:
```cpp
        startStop.setBounds (x.removeFromRight (120).withSizeKeepingCentre (120, 34));
        x.removeFromRight (14);
        statusLine.setBounds (x.withSizeKeepingCentre (x.getWidth(), 22));
```
with:
```cpp
        startStop.setBounds (x.removeFromRight (120).withSizeKeepingCentre (120, 34));
        x.removeFromRight (14);
        if (updateLink.isVisible()) {
            const int w = juce::jmin (230, x.getWidth());
            updateLink.setBounds (x.removeFromRight (w).withSizeKeepingCentre (w, 22));
            x.removeFromRight (12);
        }
        statusLine.setBounds (x.withSizeKeepingCentre (x.getWidth(), 22));
```

- [ ] **Step 4: Lay out the Advanced checkbox in `resized()`**

In `MainComponent::resized()`, in the Advanced block (near L854 where `complexPhaseToggle.setVisible (adv)` and the advanced rows are laid out), add `autoUpdateToggle` to that block. Set its visibility with the others and give it a row inside the `if (adv)` layout, e.g. directly after the `complexPhaseToggle` row is positioned:
```cpp
        autoUpdateToggle.setVisible (adv);
        if (adv)
            autoUpdateToggle.setBounds (rr.removeFromTop (26));
```
(Place this alongside the other `setVisible (adv)` / `removeFromTop (26)` advanced rows so it stacks with `complexPhaseToggle`, `firLenBox`, `trimSlider`, and `verifyButton`. Use the same local rectangle name the surrounding advanced code uses — `rr` per L853.)

- [ ] **Step 5: Build and run the app; verify it launches and behaves**

Run:
```bash
./tools/dev.cmd cmake --build build --target EarsBridge
```
Expected: clean build. Launch the app (e.g. `Start-Process` the built exe, detached, per the project's PrintWindow capture technique) and confirm:
- The window opens normally; no "Update available" link is shown when running the latest version (current `0.2.12` == latest release).
- Opening **Advanced** shows the new "Automatically check for updates" checkbox, ticked.
- No hang on startup (the check is off-thread) and no crash on close (cancel-safe).

To positively exercise the "update available" path without publishing a release, temporarily lower the project version (e.g. set `project(EarsBridge VERSION 0.2.0 ...)` in `CMakeLists.txt`), rebuild, launch, and confirm the title-bar link appears as "Update available · v0.2.12 →" and opens the release page on click; then revert the version.

- [ ] **Step 6: Commit**

```bash
git add src/gui/MainComponent.h src/gui/MainComponent.cpp
git commit -m "feat(update): title-bar update link + Advanced toggle + throttled check"
```

---

## Task 6: Docs + full verification

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add the feature + privacy note to the README**

In `README.md`, add a line to the feature list (near the other health/UX features), e.g.:
```markdown
- **Update check** — on launch the app quietly checks GitHub for a newer release and shows a small
  "Update available" link in the title bar if one exists. It never downloads anything itself; turn it
  off under **Advanced → Automatically check for updates**.
```
And add a short privacy sentence wherever the README discusses what the app contacts (or in the troubleshooting/notes area):
```markdown
The update check is the only network request EARS Bridge makes: a single call to GitHub's public
releases API at launch (at most once per 24 h), sending nothing but a standard request. Disable it
under Advanced.
```

- [ ] **Step 2: Build everything and run the FULL test suite**

Run:
```bash
./tools/dev.cmd cmake --build build
./tools/dev.cmd ctest --test-dir build --output-on-failure
```
Expected: all targets build clean; every test passes (the existing suite plus the new `UpdateCheck ...` and `Settings ... update-check` cases).

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(update): document the update check + privacy note"
```

---

## Self-review notes (author)

- **Spec coverage:** subtle title-bar link (Task 5), on-by-default + Advanced opt-out (Tasks 4–5), never auto-download — link opens the browser (Task 5, `HyperlinkButton.setURL`), once-per-launch + 24 h throttle (Task 5, `lastUpdateCheck`), fail-silent (Task 3 — null stream / no tag → no link), drafts/pre-releases excluded (GitHub `/latest` behaviour), numeric compare (Task 1), `isNewer` test matrix (Task 1), privacy note (Task 6). All present.
- **Version source:** `EB_VERSION_STRING` from CMake `PROJECT_VERSION` (Task 1, Step 3) — single source of truth; avoids `ProjectInfo` (absent in the `eb_gui` static lib) and the separately-hardcoded `Main.cpp` string.
- **Type consistency:** `UpdateInfo` (fields `reachedServer`, `updateAvailable`, `latestVersion`, `releaseUrl`), `isNewer(current, latest)`, `parseRelease(jsonBody, currentVersion)`, `UpdateChecker::start(currentVersion, onDone)` — used identically across Tasks 1–5.
- **No new module / CMake networking change:** `juce::URL` works via native Win/mac backends with `JUCE_USE_CURL=0`; EARS Bridge ships Win+mac only.
