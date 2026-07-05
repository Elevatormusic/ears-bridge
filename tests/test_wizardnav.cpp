#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"
#include "gui/juce_design_probe.h"   // P2 Task 5: probe the showing/label set of the Calibrate stage

// Task 4: navigation, tick wiring, focus & a11y. Headless MainComponent (hermetic TestConfig — never
// touches the real %APPDATA%/Settings/log dir, no network). A fresh temp profile has NO devices, so the
// launch/first-unmet resolution lands on Connect and Level/Measure are Blocked — the anchor for the pin
// legality tests below. These tests assert the wiring Task 4 adds on top of the pure WizardState machine
// (already exhaustively tested in test_wizardstate.cpp): stage forcing, CTA text/enablement, focus
// placement, the M1 gate helpText, and the H2 picker inner-combo titles.

namespace {
eb::MainComponent::TestConfig hermetic (juce::File tmp) {
    return eb::MainComponent::TestConfig { tmp, /*disableNetwork*/ true,
                                           tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") };
}
// Is `descendant` the same as, or nested under, `ancestor`?
bool isDescendantOf (const juce::Component* descendant, const juce::Component* ancestor) {
    for (auto* c = descendant; c != nullptr; c = c->getParentComponent())
        if (c == ancestor) return true;
    return false;
}
} // namespace

TEST_CASE("WizardNav fresh construct shows Connect (first unmet)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    CHECK (mc.shownStageForTest() == eb::WizardStep::Connect);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav pinning Calibrate is honored (never Blocked)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    CHECK (mc.shownStageForTest() == eb::WizardStep::Calibrate);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav pinning Measure with nothing configured falls back to Connect") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // Measure is Blocked in the empty profile (Connect+Calibrate not Done) -> an ordinary pin is illegal
    // and the machine resolves to first-unmet == Connect. (This uses the live-navigation seam, NOT
    // forceWizardStepForTest, which deliberately overrides pin legality for the gate.)
    mc.pinStepForTest (eb::WizardStep::Measure);
    CHECK (mc.shownStageForTest() == eb::WizardStep::Connect);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav Continue CTA text and disabled-while-Todo") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // Fresh: Connect is Todo (no devices) -> its Continue is disabled and reads "Continue to calibration".
    auto& cta = mc.connectContinueForTest();
    CHECK (cta.getButtonText() == juce::String ("Continue to calibration"));
    CHECK_FALSE (cta.isEnabled());

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav focus lands inside the shown stage on switch") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    // The documented seam: the first-focus target of the shown stage lives inside CalibrateStage.
    auto* target = mc.firstFocusTargetForTest();
    REQUIRE (target != nullptr);
    CHECK (isDescendantOf (target, &mc.calibrateStageForTest()));
    // minor-3: focus must land on a real control, never the scroll Viewport (which wants focus by default).
    CHECK (dynamic_cast<juce::Viewport*> (target) == nullptr);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav M1 Start help text carries the gate reason when not ready") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // Empty profile: the gate is not ready, so Start's helpText names WHY (a nonempty reason).
    CHECK (mc.startButtonForTest().getHelpText().isNotEmpty());

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav H2 device pickers carry inner-combo accessible titles") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    CHECK (mc.inputPickerForTest().comboTitleForTest() == juce::String ("Input device"));
    CHECK (mc.outputPickerForTest().comboTitleForTest() == juce::String ("Output virtual cable"));

    tmp.deleteRecursively();
}

// M-2: the Level green-band latch must be invalidated on an input-device change through BOTH the user
// pick and the hot-plug re-resolution. Both route through applyResolvedInput; this drives that shared
// path directly (the hot-plug lambda's exact member) and asserts a CHANGED key clears the latch while an
// UNCHANGED key leaves a still-valid latch alone. Guards the regression where a replug (or the EARS
// gain-DIP rename fallback) re-applied the input without clearing the latch, so Level kept claiming
// "In green band" on old-gain evidence.
TEST_CASE("WizardNav levelLatched clears when the applied input key changes (M-2)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // A distinct input device (its own uid -> its own key()). Applying it must clear a set latch.
    eb::DeviceId dev; dev.typeName = "Windows Audio"; dev.name = "EARS"; dev.uid = "uid-A";
    mc.applyResolvedInputForTest (dev);   // seed the memo to dev's key
    mc.setLevelLatchedForTest (true);

    // Same device again -> key unchanged -> the still-valid latch survives.
    mc.applyResolvedInputForTest (dev);
    CHECK (mc.levelLatchedForTest());

    // A genuinely different device -> key changes -> the latch is invalidated.
    eb::DeviceId dev2; dev2.typeName = "Windows Audio"; dev2.name = "EARS Pro"; dev2.uid = "uid-B";
    mc.applyResolvedInputForTest (dev2);
    CHECK_FALSE (mc.levelLatchedForTest());

    tmp.deleteRecursively();
}

// The recorded carryover: calBuilding causes a TRANSIENT auto-navigation risk. When the user has pinned
// Measure and an async FIR rebuild flips Level/Measure to Blocked, the machine's ws.active would resolve
// back to Calibrate/Connect and yank the view off Measure — a flicker. resolveShownStage HOLDS the pinned
// stage across that transient (pin is Blocked in ws but NOT in the calBuilding-masked state), while the
// spine still renders the rebuild truthfully. This tests the PURE hold decision directly (the honest unit
// — a fully-configured L+R+cal device stack cannot be fabricated in a hermetic no-device test env).
TEST_CASE("WizardNav resolveShownStage holds a pin blocked only by calBuilding") {
    // Build the two states the app would compute at the transient: everything satisfied, Measure pinned.
    eb::WizardInputs base;
    base.haveDevs = true; base.haveCals = true; base.gateReady = true;
    base.engineRunning = true; base.levelLatched = true;
    base.configGen = 1; base.verdictGenL = 1; base.verdictGenR = 1;
    base.earGradedL = true; base.earGradedR = true;

    // ws: calBuilding ON -> Calibrate Todo(rebuilding), Level+Measure Blocked; pinned Measure is illegal so
    // ws.active resolves to first-unmet (Calibrate). masked: calBuilding OFF -> Measure legal (Done).
    auto withBuild = base;    withBuild.calBuilding = true;
    const auto ws     = eb::computeWizardState (withBuild, eb::WizardStep::Measure);
    const auto masked = eb::computeWizardState (base,      eb::WizardStep::Measure);

    REQUIRE (ws.active != eb::WizardStep::Measure);                                  // machine yanked away
    REQUIRE (ws.steps[(int) eb::WizardStep::Measure].state == eb::StepState::Blocked);
    REQUIRE (masked.steps[(int) eb::WizardStep::Measure].state != eb::StepState::Blocked);

    // The view rule holds Measure across the transient.
    CHECK (eb::MainComponent::resolveShownStage (ws, eb::WizardStep::Measure, masked)
               == eb::WizardStep::Measure);

    // Non-transient illegality (genuinely blocked even with the rebuild masked) is NOT held.
    eb::WizardInputs empty;   // no devices/cals -> Measure Blocked both with and without calBuilding
    const auto wsEmpty     = eb::computeWizardState (empty, eb::WizardStep::Measure);
    const auto maskedEmpty = eb::computeWizardState (empty, eb::WizardStep::Measure);
    CHECK (eb::MainComponent::resolveShownStage (wsEmpty, eb::WizardStep::Measure, maskedEmpty)
               == wsEmpty.active);
}

// P2 Task 5: exactly ONE guidance caption on the Calibrate stage (spec 5.2 - the HEQ note used
// to render per card), and the Advanced-FIR section genuinely discloses (hidden controls are
// out of the probe's showing set; open brings them back).
TEST_CASE("Calibrate stage: one guidance caption; Advanced FIR discloses") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");

    auto countShowing = [&] (const juce::String& needle) {
        hig::writeDesignProbe (mc, jf, pf);
        const auto tree = juce::JSON::parse (jf);
        int n = 0;
        if (const auto* els = tree.getProperty ("elements", {}).getArray())
            for (auto& e : *els)
                if ((bool) e.getProperty ("showing", false)
                    && e.getProperty ("label", {}).toString().contains (needle))
                    ++n;
        return n;
    };

    CHECK (countShowing ("HEQ files for headphones") == 1);   // the ONE caption, exactly once
    CHECK (countShowing ("FIR LENGTH") == 0);                 // collapsed by default (all-default settings)
    mc.calibrateStageForTest().setAdvancedOpen (true);
    mc.resized();
    CHECK (countShowing ("FIR LENGTH") == 1);                 // disclosed
    mc.calibrateStageForTest().setAdvancedOpen (false);
    mc.resized();
    CHECK (countShowing ("FIR LENGTH") == 0);                 // and collapses again
    tmp.deleteRecursively();
}

// Map #9/#10: the header run-note mirrors the machine's first-unmet reason and clears on Done.
TEST_CASE("Calibrate stage: run-note carries the machine reason while not Done") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.pinStepForTest (eb::WizardStep::Calibrate);
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");
    hig::writeDesignProbe (mc, jf, pf);
    // Pin THE header's own run-note element (componentID "calRunNote"), not just "some showing element
    // whose label matches" - the spine's per-step meta Label has carried the same machine reason since
    // P1, so a label-only match stays green even if the header wiring is deleted. Find the element by
    // its id, then assert ITS label is the machine reason.
    // Bind the parsed tree to a named local before taking getArray() - the array pointer is owned by
    // the parsed var, so `JSON::parse(jf).getProperty(...).getArray()` would dangle at the end of the
    // full expression (the idiom every other probe test uses: hold `tree`, then iterate).
    const auto tree = juce::JSON::parse (jf);
    bool foundRunNote = false, runNoteHasReason = false;
    if (const auto* els = tree.getProperty ("elements", {}).getArray())
        for (auto& e : *els)
            if (e.getProperty ("id", {}).toString() == "calRunNote"
                && (bool) e.getProperty ("showing", false)) {
                foundRunNote = true;
                runNoteHasReason = e.getProperty ("label", {}).toString() == eb::kReasonNoCals();
            }
    CHECK (foundRunNote);        // the header's run-note element is present + showing
    CHECK (runNoteHasReason);    // and IT (not some sibling) carries the machine reason
    tmp.deleteRecursively();
}

// P2 Task 6 (spec 5.2 unity path + map #8): continue-without-calibration is an EXPLICIT,
// session-scoped choice: it completes Calibrate, retires its own button, keeps the warning
// visible as evidence - and ANY cal load revokes it (with the Required line lighting up on
// the still-empty sibling).
TEST_CASE("Unity path: explicit accept completes Calibrate; a cal load revokes it") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.pinStepForTest (eb::WizardStep::Calibrate);
    auto& stage = mc.calibrateStageForTest();

    // Fresh: both empty -> hint + button offered; CTA disabled (Calibrate Todo).
    REQUIRE (stage.unityButtonForTest().isVisible());
    CHECK_FALSE (stage.continueButton().isEnabled());

    stage.unityButtonForTest().onClick();                     // the REAL wired path
    // Accepted: Calibrate is Done (unity), the CTA enables, the button retires, the hint stays.
    CHECK (stage.continueButton().isEnabled());
    CHECK_FALSE (stage.unityButtonForTest().isVisible());

    // Revocation: loading ONE cal resets the choice AND masks it (noCalsLoaded false).
    auto cal = juce::File::createTempFile (".txt");
    cal.replaceWithText ("* HEQ\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (mc.leftCalForTest().loadFromFile (cal));
    cal.deleteFile();
    CHECK_FALSE (stage.continueButton().isEnabled());         // one file != a valid applied pair
    CHECK_FALSE (stage.unityButtonForTest().isVisible());     // not both-empty any more
    // Map #8b/#10: the still-empty RIGHT card now claims Required (Start is truly blocked).
    auto* req = mc.rightCalForTest().findChildWithID ("calDzReq");
    REQUIRE (req != nullptr);
    CHECK (req->isVisible());
    // And clearing back to empty does NOT silently resurrect the old acceptance.
    mc.leftCalForTest().clearCal();
    CHECK (stage.unityButtonForTest().isVisible());           // offered again - not auto-applied
    CHECK_FALSE (stage.continueButton().isEnabled());
    tmp.deleteRecursively();
}
