#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"

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
