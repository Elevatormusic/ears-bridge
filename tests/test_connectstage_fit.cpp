#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"
#include "gui/stages/ConnectStage.h"

// T10 ledger: every gated Connect workflow state fits the 900x720 minimum with zero scroll.
// The Viewport stays as a safety net only; content taller than the viewport = the bug.
static void applyConnectState (eb::MainComponent& mc, eb::ConnectStage::WorkflowState s) {
    using WS = eb::ConnectStage::WorkflowState;
    auto& stage = mc.connectStageForTest();
    stage.notUsingDiracForTest().setOpen (s == WS::OverrideOpen || s == WS::OverrideOpenWorst);
    const bool dirac = (s == WS::DiracHintFix || s == WS::DiracHintFixRateWarn || s == WS::OverrideOpenWorst);
    const bool rate  = (s == WS::RateWarn || s == WS::DiracHintFixRateWarn || s == WS::OverrideOpenWorst);
    mc.driveConnectWarningsForTest (dirac, rate);
}

TEST_CASE("ConnectStage: every gated workflow state fits 900x720 with zero scroll [T10]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    static_assert (eb::ConnectStage::kWorkflowStateCount == 6,
                   "new Connect workflow state: extend applyConnectState + the T10 ledger");
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    juce::StringArray bad;
    for (int i = 0; i < eb::ConnectStage::kWorkflowStateCount; ++i) {
        applyConnectState (mc, (eb::ConnectStage::WorkflowState) i);
        auto& vp = mc.connectStageForTest().viewportForTest();
        const int contentH = vp.getViewedComponent()->getHeight(), vpH = vp.getHeight();
        if (contentH > vpH)
            bad.add ("state " + juce::String (i) + ": content " + juce::String (contentH)
                     + "px > viewport " + juce::String (vpH) + "px");
    }
    applyConnectState (mc, eb::ConnectStage::WorkflowState::Default);   // restore model truth
    INFO ("overflowing states:\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

// The output MESSAGE SLOT (T10-D7): while the dirac cable hint is visible it OWNS the row and
// outputHint hides; when it clears, outputHint returns. Negative half: the passive hint must
// not vanish in the default state.
TEST_CASE("ConnectStage: dirac hint occupies the output message slot; outputHint returns after [T10]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    // Establish the NotVirtual baseline the slot test needs: the sim engine's autoSelectDefaults()
    // preselects a standard VB-CABLE output, so updateDiracCableHint() shows the (shared-mode) dirac
    // hint by default and the passive slot is legitimately suppressed. Clear the output selection so
    // "default" is the no-warning guidance state - then drive the swap on/off from there.
    mc.outputPickerForTest().setDevices ({}, {});
    mc.driveConnectWarningsForTest (false, false);        // re-derive against the cleared output
    auto* hint = mc.connectStageForTest().diracCableHintForTest();
    REQUIRE (hint != nullptr);
    auto& outputHint = mc.outputHintForTest();
    CHECK (outputHint.isVisible());                       // default: passive guidance shown
    mc.driveConnectWarningsForTest (true, false);
    CHECK (hint->isVisible());
    CHECK_FALSE (outputHint.isVisible());                 // slot swapped
    CHECK (hint->getBounds().getHeight() > 0);
    mc.driveConnectWarningsForTest (false, false);
    CHECK (outputHint.isVisible());                       // guidance returns
    tmp.deleteRecursively();
}
