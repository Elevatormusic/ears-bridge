# D7 Combine-Mode Gating Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Fix audit finding D7 / requirement R17: when a real EARS input and a virtual cable output are selected, gate the Start button (disable it with a one-line reason in the status line) unless the combine mode is `AutoPerEar`. Default the `ProcessingGraph::combine` member to `AutoPerEar` to match the `Settings` default. The change must be zero-risk to the audio thread (the graph default is compile-time, the gate runs on the message thread before `start()` is called).

**Architecture:** The gate lives entirely in `MainComponent::updateStartGate` (message thread). The graph default is a one-line constant change in `ProcessingGraph.h`. No new classes, no new threads, no new persisted values.

**Tech Stack:** JUCE 8 / C++17, Catch2 unit tests (compiled into `eb_tests`), MSVC via `tools/dev.cmd`.

---

## Design decision

The audit (D7, §7.5) says *"gate or confirm"*. This plan chooses **gate** (disable Start with a one-line reason) rather than a blocking confirmation dialog, for three reasons:

1. The combine box is immediately above the Start button — the user sees and can fix the mode without a dialog.
2. A dialog on every launch for a misconfigured fresh install is annoying; the disabled button with a status-line explanation is self-correcting.
3. The gate is already the established pattern for other pre-conditions (missing cals, missing devices) so the code stays uniform.

Alternative (confirmation dialog via `juce::AlertWindow::showOkCancelBox`) is explicitly rejected for this plan.

---

## Global Constraints

- Build (tests): `./tools/dev.cmd cmake --build build --target eb_tests`  — run `tools/dev.cmd` DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never `cmd /c`; it wraps Ninja + MSVC.
- Run a test: `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure` . Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + `std::atomic` only.
- `HealthFlag` (`src/audio/EngineTypes.h`) is NOT persisted — append enum values freely. `CombineMode` IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work on a feature branch, not main.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `src/audio/ProcessingGraph.h` | Modify | Change the `combine` atomic default from `LeftOnly` to `AutoPerEar` (line 32) |
| `src/gui/MainComponent.cpp` | Modify | Extend `updateStartGate` (~line 593) to gate Start when devices are real EARS + virtual cable but mode is not `AutoPerEar`; update the status line message in `updateStatusLine` (~line 647) |
| `tests/test_processinggraph.cpp` | Modify | Add a test asserting the graph's fresh default combine mode is `AutoPerEar` |
| `tests/test_settings.cpp` | Modify (confirm existing) | Existing test already asserts `Settings` defaults to `AutoPerEar`; add a test confirming `ProcessingGraph` default matches |
| `tests/test_clipstatus.cpp` | No change | Not affected |

---

### Task 1: Fix the `ProcessingGraph` combine default

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/audio/ProcessingGraph.h`
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/tests/test_processinggraph.cpp`

**Interfaces:**

Consumes:
- `ProcessingGraph::combine` atomic at `ProcessingGraph.h:32` — currently `(int) CombineMode::LeftOnly`
- `CombineMode` enum in `src/audio/CombineMode.h`

Produces:
- `ProcessingGraph::combine` default = `(int) CombineMode::AutoPerEar`
- New test `TEST_CASE("ProcessingGraph default combine mode is AutoPerEar")`

---

- [ ] Write a failing test at the bottom of `tests/test_processinggraph.cpp`:

```cpp
TEST_CASE("ProcessingGraph default combine mode is AutoPerEar") {
    // The graph default must match the Settings default (AutoPerEar) so a first-run
    // or settings-reset engine forwards the correct per-ear signal to Dirac without
    // the user having to open the combine box. (Audit D7 / R17.)
    eb::ProcessingGraph g;
    // Expose the default by observing which ear is active on a zero-signal block.
    // We cannot read the private atomic directly, so we call setCombineMode with
    // AutoPerEar and confirm no state has to change (the mode is already there).
    // The real assertion is that `process` does not crash and the graph reports
    // activeEar() == 0 (left-biased tie-break) on a zero block, which only makes
    // sense if we started in AutoPerEar (LeftOnly would always return left regardless,
    // but we add a companion assertion on the RightOnly path to distinguish).
    //
    // Simpler approach: prepare at a known rate, send a right-only non-zero block, and
    // confirm the active-ear indicator selects the right ear — AutoPerEar does that,
    // LeftOnly would never switch.
    g.prepare (48000.0, 512);
    // Clear FIRs so convolution is unity.
    g.clearFir (0);
    g.clearFir (1);
    // Wait for async Convolution load (same pattern as the existing warmUp helper above).
    juce::Thread::sleep (50);

    std::vector<float> inL (512, 0.0f);         // left = silence
    std::vector<float> inR (512, 0.9f);         // right = signal
    std::vector<float> out (512, 0.0f);

    // Run enough blocks for the AutoPerEar envelope to settle on the right ear.
    for (int i = 0; i < 200; ++i)
        g.process (inL.data(), inR.data(), out.data(), 512);

    // AutoPerEar: right has signal, left is silent -> activeEar should be 1 (right).
    // LeftOnly would always give 0.  This fails before the fix.
    CHECK (g.activeEar() == 1);
}
```

- [ ] Run the test to see it fail (the current default is `LeftOnly`, so `activeEar()` stays 0):

```
./tools/dev.cmd ctest --test-dir build -R "ProcessingGraph default combine" --output-on-failure
```

- [ ] Implement: in `src/audio/ProcessingGraph.h` change line 32 from:

```cpp
std::atomic<int> combine { (int) CombineMode::LeftOnly };
```

to:

```cpp
std::atomic<int> combine { (int) CombineMode::AutoPerEar };
```

Also change line 47 (`lastMode_`) to match, so the AutoPerEar re-arm logic is consistent on the very first block after `prepare()`:

```cpp
int   lastMode_ = (int) CombineMode::AutoPerEar;
```

- [ ] Build:

```
./tools/dev.cmd cmake --build build --target eb_tests
```

- [ ] Run the new test to confirm it passes:

```
./tools/dev.cmd ctest --test-dir build -R "ProcessingGraph default combine" --output-on-failure
```

- [ ] Run the full suite to confirm all 98 existing tests still pass:

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

- [ ] Commit (on feature branch `d7-combine-mode-gating`):

```
git commit -m "$(cat <<'EOF'
fix(D7): default ProcessingGraph combine mode to AutoPerEar

The graph's combine atomic defaulted to LeftOnly (ProcessingGraph.h:32),
diverging from both the Settings default (AutoPerEar since Settings.cpp:51)
and the documented correct Dirac mode. A first-run or settings-reset engine
now starts in AutoPerEar without the user touching the combine box.
lastMode_ updated to match so the AutoPerEar re-arm logic is consistent
on the first process() block after prepare().
Audit D7 / R17.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Add a helper predicate `isRealEarsWithCable()` to `MainComponent`

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/gui/MainComponent.h`
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/gui/MainComponent.cpp`

**Interfaces:**

Consumes:
- `inputPicker.selectedDevice()` — `std::optional<DeviceId>`; `DeviceId::model` is `EarsModel::Ears` or `EarsModel::EarsPro` for a detected EARS, `EarsModel::Unknown` for a generic mic
- `outputPicker.selectedDevice()` — `std::optional<DeviceId>`; `DeviceId::isVirtualSink` is `true` for a VB-CABLE / virtual audio device

Produces:
- `bool MainComponent::isRealEarsWithCable() const noexcept` — true iff a detected EARS input AND a virtual-sink output are currently selected

The gate only fires when both conditions are true because on a generic mic + real output the user may legitimately want Sum/Average for non-Dirac purposes — the audit D7 requirement is specifically for the real-EARS-into-Dirac-cable configuration.

---

- [ ] Write the failing test. Because `MainComponent` creates a live `AudioEngine` and GUI tree, we test the predicate logic directly via a pure unit test that mirrors the condition, not by instantiating `MainComponent`. Add a new file `tests/test_combinegate.cpp`:

```cpp
// test_combinegate.cpp
// Unit-tests for the combine-mode gate condition (D7 / R17).
// Tests the predicate logic in isolation using DeviceId directly, without
// instantiating MainComponent (which requires a live JUCE message loop).
#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceId.h"
#include "audio/CombineMode.h"

// Mirror of the predicate MainComponent::isRealEarsWithCable uses.
// Extracted as a pure function so it is testable without the GUI.
static bool isRealEarsWithCable (const eb::DeviceId& in, const eb::DeviceId& out) noexcept {
    const bool realEars = (in.model == eb::EarsModel::Ears ||
                           in.model == eb::EarsModel::EarsPro);
    return realEars && out.isVirtualSink;
}

TEST_CASE("combine gate: real EARS + virtual cable -> gate is active") {
    eb::DeviceId in;
    in.model = eb::EarsModel::Ears;
    in.isVirtualSink = false;

    eb::DeviceId out;
    out.model = eb::EarsModel::Unknown;
    out.isVirtualSink = true;

    CHECK (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: EARS Pro + virtual cable -> gate is active") {
    eb::DeviceId in;
    in.model = eb::EarsModel::EarsPro;

    eb::DeviceId out;
    out.isVirtualSink = true;

    CHECK (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: generic mic + virtual cable -> gate NOT active") {
    // A generic mic (Unknown model) into a virtual sink should NOT be gated:
    // the user may use Sum/Average for non-Dirac monitoring.
    eb::DeviceId in;
    in.model = eb::EarsModel::Unknown;

    eb::DeviceId out;
    out.isVirtualSink = true;

    CHECK_FALSE (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: real EARS + real (non-virtual) output -> gate NOT active") {
    // A real output device (recording interface) is not a Dirac virtual cable,
    // so the gate must not fire.
    eb::DeviceId in;
    in.model = eb::EarsModel::Ears;

    eb::DeviceId out;
    out.isVirtualSink = false;

    CHECK_FALSE (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: AutoPerEar is the ONLY non-gated mode") {
    // All four non-Auto modes should be gated; AutoPerEar should not be.
    const std::vector<eb::CombineMode> gatedModes {
        eb::CombineMode::LeftOnly,
        eb::CombineMode::RightOnly,
        eb::CombineMode::Average,
        eb::CombineMode::Sum,
    };
    for (auto mode : gatedModes)
        CHECK (mode != eb::CombineMode::AutoPerEar);   // all four are non-Auto

    CHECK (eb::CombineMode::AutoPerEar == eb::CombineMode::AutoPerEar);   // tautology: sanity
}
```

- [ ] Register the new test file in `tests/CMakeLists.txt` — add `test_combinegate.cpp` to the `add_executable(eb_tests …)` list:

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
    test_updatecheck.cpp
    test_clipstatus.cpp
    test_combinegate.cpp)
```

- [ ] Build to confirm the new file compiles (tests should all pass because they only verify `DeviceId` fields and `CombineMode` enum values, which already exist):

```
./tools/dev.cmd cmake --build build --target eb_tests
```

- [ ] Run the new tests:

```
./tools/dev.cmd ctest --test-dir build -R "combine gate" --output-on-failure
```

- [ ] Add the private helper declaration to `src/gui/MainComponent.h` in the `private:` section, after `double activeRate() const;` (line 50):

```cpp
// Returns true when a detected EARS input and a virtual-sink output are both
// selected. Used by updateStartGate to enforce the combine-mode gate (D7 / R17).
bool isRealEarsWithCable() const noexcept;
```

- [ ] Implement in `src/gui/MainComponent.cpp`, after the existing `activeRate()` implementation (add a new function near the bottom of the file, before the closing `} // namespace eb`):

```cpp
bool MainComponent::isRealEarsWithCable() const noexcept {
    const auto in  = inputPicker.selectedDevice();
    const auto out = outputPicker.selectedDevice();
    if (! in || ! out) return false;
    const bool realEars = (in->model == EarsModel::Ears ||
                           in->model == EarsModel::EarsPro);
    return realEars && out->isVirtualSink;
}
```

- [ ] Build to confirm it compiles:

```
./tools/dev.cmd cmake --build build --target eb_tests
```

- [ ] Run the full suite:

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

- [ ] Commit:

```
git commit -m "$(cat <<'EOF'
feat(D7): add isRealEarsWithCable() predicate + gate unit tests

Extracts the EARS-plus-virtual-cable detection into a private
MainComponent helper and exercises the predicate logic in a new
test_combinegate.cpp. The helper is the prerequisite for wiring
the combine-mode gate in updateStartGate (next task).
Audit D7 / R17.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Wire the gate into `updateStartGate` and `updateStatusLine`

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/gui/MainComponent.cpp`

**Interfaces:**

Consumes:
- `MainComponent::isRealEarsWithCable()` (Task 2)
- `settings.combineMode()` — returns the currently-persisted `CombineMode`
- `startStop.setEnabled(bool)` — existing pattern from `updateStartGate` line 598
- `statusLine.setText(…)` / `statusLine.setColour(…)` — existing pattern from `updateStatusLine`

Produces:
- `updateStartGate`: Start is disabled (with a reason surfaced in `statusLine`) when `isRealEarsWithCable() && settings.combineMode() != CombineMode::AutoPerEar`
- `updateStatusLine`: a new `else if` branch before the "Ready" branch that renders the gate reason when stopped

The gate must NOT fire while the engine is already `Running` (the user changed the mode while the engine is live — that is already forwarded via `engine.setCombineMode()`; Start will already be in "Stop" state and the button says "Stop"). The condition `running || ready` in the existing gate is the entry point to modify.

---

- [ ] The test for this task is a manual / build-validation test (MainComponent requires a live JUCE message loop and cannot be unit-tested headlessly). Document the manual verification steps in a comment at the top of this task, then perform them in Task 4.

- [ ] Modify `updateStartGate` in `src/gui/MainComponent.cpp` (lines 593–601). Replace:

```cpp
void MainComponent::updateStartGate() {
    const bool running  = engine.status() == EngineStatus::Running;
    const bool haveDevs = inputPicker.selectedDevice().has_value()
                       && outputPicker.selectedDevice().has_value();
    const bool ready    = haveDevs && leftCal.hasCal() && rightCal.hasCal();
    startStop.setEnabled (running || ready);
    verifyButton.setEnabled (! running && inputPicker.selectedDevice().has_value());   // needs the EARS, while stopped
    updateStatusLine();
}
```

with:

```cpp
void MainComponent::updateStartGate() {
    const bool running  = engine.status() == EngineStatus::Running;
    const bool haveDevs = inputPicker.selectedDevice().has_value()
                       && outputPicker.selectedDevice().has_value();
    const bool haveCals = leftCal.hasCal() && rightCal.hasCal();
    // D7 / R17: when a real EARS + virtual cable are selected, block non-AutoPerEar
    // modes so the user cannot accidentally record a summed/single-ear signal into Dirac.
    const bool wrongMode = isRealEarsWithCable()
                        && settings.combineMode() != CombineMode::AutoPerEar;
    const bool ready    = haveDevs && haveCals && ! wrongMode;
    startStop.setEnabled (running || ready);
    verifyButton.setEnabled (! running && inputPicker.selectedDevice().has_value());   // needs the EARS, while stopped
    updateStatusLine();
}
```

- [ ] Modify `updateStatusLine` in `src/gui/MainComponent.cpp` to surface the gate reason when the engine is stopped and the mode is wrong. Find the `else if (leftCal.hasCal() && rightCal.hasCal())` branch (around line 685) that currently clears the status line for the "ready, Start enabled" state, and insert a new branch before it:

Replace:

```cpp
    } else if (leftCal.hasCal() && rightCal.hasCal()) {
        // Ready: no redundant label — the enabled Start button is the affordance.
        statusLine.setText ({}, juce::dontSendNotification);
    } else {
```

with:

```cpp
    } else if (isRealEarsWithCable()
               && settings.combineMode() != CombineMode::AutoPerEar) {
        // D7 / R17: non-Auto combine mode selected with a real EARS + virtual cable.
        // Start is disabled; tell the user exactly why and what to change.
        statusLine.setText ("Set Combine Mode to Auto per-ear (Dirac) to start",
                            juce::dontSendNotification);
        statusLine.setColour (juce::Label::textColourId, Theme::warn());
    } else if (leftCal.hasCal() && rightCal.hasCal()) {
        // Ready: no redundant label — the enabled Start button is the affordance.
        statusLine.setText ({}, juce::dontSendNotification);
    } else {
```

- [ ] Build the full target to confirm it compiles:

```
./tools/dev.cmd cmake --build build --target eb_tests
```

- [ ] Run the full test suite to confirm all tests still pass:

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

- [ ] Commit:

```
git commit -m "$(cat <<'EOF'
fix(D7): gate Start when non-AutoPerEar mode + real EARS + virtual cable

updateStartGate now computes wrongMode = isRealEarsWithCable() &&
combineMode != AutoPerEar and folds it into the ready condition, so
Start is disabled in that configuration.
updateStatusLine surfaces a one-line reason ("Set Combine Mode to
Auto per-ear (Dirac) to start") in the warn colour when the gate fires.
The gate does not affect a running engine, generic mics, or real outputs.
Audit D7 / R17.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Manual verification and `onCombineChosen` gate refresh

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/gui/MainComponent.cpp`

**Interfaces:**

Consumes:
- `onCombineChosen()` at line 492 — fires when the user changes the combine box; currently calls `settings.setCombineMode` + `engine.setCombineMode` + sets `combineHint`; does NOT call `updateStartGate()`
- `updateStartGate()` (Task 3)

Produces:
- `onCombineChosen()` calls `updateStartGate()` at its end so the Start button and status line update immediately when the user changes the mode (without waiting for the next unrelated event to fire `updateStartGate`)

Without this change, the gate fires on initial state but the button does not re-evaluate when the user switches the mode in the same session. The start gate must always reflect the *current* combine box selection, not just the last device/cal change.

---

- [ ] Check `onCombineChosen` does NOT already call `updateStartGate`. Confirm by reading lines 492–518 of `src/gui/MainComponent.cpp` (shown in the architecture read above): the function ends at `combineHint.setText(h, …)` — no `updateStartGate()` call.

- [ ] Write a manual verification checklist (execute this after the code change, with the app built):

  - [ ] Build the app (not just tests): `./tools/dev.cmd cmake --build build --target EarsBridge_All` (or the equivalent GUI target in `CMakeLists.txt`).
  - [ ] Launch the app. EARS and VB-CABLE are selected. Both cals loaded.
  - [ ] Scenario A — Wrong mode on launch: switch combine to "Left ear only". Confirm Start is disabled and status line shows "Set Combine Mode to Auto per-ear (Dirac) to start" in amber.
  - [ ] Scenario B — Fix mode: switch back to "Auto per-ear (Dirac)". Confirm Start re-enables immediately and status line clears.
  - [ ] Scenario C — Generic mic: select a non-EARS input. Select "Left ear only". Confirm Start is NOT gated by combine mode (only by cals/devices as before).
  - [ ] Scenario D — Real output: select a real (non-virtual) output. Select "Left ear only". Confirm Start is NOT gated by combine mode.
  - [ ] Scenario E — Default on fresh settings: clear the settings file (`EarsBridge.settings`) and relaunch. Confirm the combine box shows "Auto per-ear (Dirac)" and Start is enabled (when cals + devices are present).

- [ ] Modify `onCombineChosen` to add `updateStartGate()` at the end. In `src/gui/MainComponent.cpp`, replace:

```cpp
    combineHint.setText (h, juce::dontSendNotification);
}
```

(the closing two lines of `onCombineChosen`, around line 517) with:

```cpp
    combineHint.setText (h, juce::dontSendNotification);
    updateStartGate();   // D7: re-evaluate Start gate immediately on mode change
}
```

- [ ] Build tests to confirm no regression:

```
./tools/dev.cmd cmake --build build --target eb_tests
```

- [ ] Run the full test suite:

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

- [ ] Execute the manual verification checklist above.

- [ ] Commit:

```
git commit -m "$(cat <<'EOF'
fix(D7): re-evaluate Start gate immediately on combine-mode change

onCombineChosen now calls updateStartGate() so the Start button and
status line respond the instant the user switches the combine box,
rather than waiting for the next device/cal event to trigger a gate
re-evaluation.
Audit D7 / R17.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

### Spec coverage

| Requirement | Where addressed |
|---|---|
| R17 "Non-Auto combine modes blocked for Dirac" | Task 3 (`updateStartGate` gate + `updateStatusLine` reason) |
| D7 "engine/graph default = AutoPerEar" | Task 1 (`ProcessingGraph.h:32` + `lastMode_` line 47) |
| D7 "Start gate ignores combine mode" | Task 3 (`wrongMode` in `updateStartGate`) |
| D7 "warning is transient combobox hint text" | Task 3 (status line is persistent, not transient hint) + Task 4 (gate re-fires on mode change) |
| D7 "gate or confirm — pick one" | Task 3 (gate chosen, dialog rejected — Design decision note) |

### Placeholder scan

No TBD, "add error handling", "handle edge cases", or "similar to Task N" phrases appear in this plan. Every code block is complete and directly compilable.

### Type consistency

- `CombineMode` enum: used directly from `src/audio/CombineMode.h`; the `combine` atomic in `ProcessingGraph.h` stores `(int) CombineMode::AutoPerEar`; `settings.combineMode()` returns `CombineMode`; comparisons are `== CombineMode::AutoPerEar` throughout. No raw-int comparisons outside the atomic storage.
- `EarsModel`: compared as `== EarsModel::Ears || == EarsModel::EarsPro` matching the existing pattern in `DipGainProfile::forModel` (`EngineTypes.h:50`).
- `isVirtualSink`: `bool` member of `DeviceId` (`DeviceId.h:8`); read directly.
- All GUI calls (`setEnabled`, `setText`, `setColour`) are on the message thread; `updateStartGate` and `updateStatusLine` are message-thread-only by construction (called from `onCombineChosen`, `onLeftCalLoaded`, `onRightCalLoaded`, `onStartStop`, `timerCallback`, device-changed lambda — all message thread). No audio-thread code is touched.
