#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"
#include "gui/Copy.h"
#include "gui/StatusLadder.h"

// ==================================================================================================
// P4 Task 6: the copy-style gate. Walks the LIVE tree across the four steps plus driven states and
// asserts no visible user-facing string carries ASCII typography (" - ", "...", " -> "). Fail-closed
// for future copy: new strings born ASCII fail here. Fixture-driven (no user filenames in the tree).
// ==================================================================================================
namespace {
void collectVisibleText (juce::Component& c, juce::StringArray& out) {
    if (! c.isVisible()) return;
    if (auto* l = dynamic_cast<juce::Label*> (&c))       { if (l->getText().isNotEmpty()) out.add (l->getText()); }
    else if (auto* b = dynamic_cast<juce::Button*> (&c)) { if (b->getButtonText().isNotEmpty()) out.add (b->getButtonText()); }
    for (int i = 0; i < c.getNumChildComponents(); ++i)
        collectVisibleText (*c.getChildComponent (i), out);
}
} // namespace

TEST_CASE("P4 copy style: no ASCII dash/ellipsis/arrow survives in visible wizard copy") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const juce::File data (EB_TEST_DATA_DIR);
    mc.setVisible (true);            // headless Components are born INVISIBLE (componentFlags(0)) -
    mc.setSize (900, 720);           // without this the walker bails at the root and sweeps NOTHING
    REQUIRE (mc.leftCalForTest().loadFromFile (data.getChildFile ("L_HEQ_0000000.txt")));
    REQUIRE (mc.rightCalForTest().loadFromFile (data.getChildFile ("R_HEQ_0000000.txt")));
    // NOTE [plan deviation, verified]: the plan waited for calBuilding to clear, but the FIR build's
    // completion rides MessageManager::callAsync (MainComponent.cpp rebuild continuation) and this
    // project leaves JUCE_MODAL_LOOPS_PERMITTED off - headless, that message is NEVER delivered, so
    // the wait would spin its full budget and change nothing. The sweep runs against the transient
    // (still-building) tree; the deep Level/Measure copy is injected via the drive seams below, which
    // do not depend on the build settling.

    juce::StringArray offenders;
    auto sweep = [&] (const juce::String& scene) {
        juce::StringArray texts;
        collectVisibleText (mc, texts);
        // Fail-closed coverage guard: every swept scene MUST yield visible text (spine + header +
        // CTA at minimum). An empty collection means the walker went blind (the root-visibility
        // trap above), and a blind gate must FAIL, not pass. Caught live: the first run of this
        // gate "passed" with zero strings collected.
        INFO ("scene '" << scene << "' collected no visible text - the walker is blind");
        REQUIRE (! texts.isEmpty());
        for (const auto& t : texts)
            if (t.contains (" - ") || t.contains ("...") || t.contains (" -> "))
                offenders.add (scene + ": \"" + t + "\"");
    };
    for (auto step : { eb::WizardStep::Connect, eb::WizardStep::Calibrate,
                       eb::WizardStep::Level, eb::WizardStep::Measure }) {
        mc.forceWizardStepForTest (step);
        sweep ("step " + juce::String ((int) step));
    }
    // Driven states with their own copy: level clip, verdict pair (incl. details), advanced open.
    mc.forceWizardStepForTest (eb::WizardStep::Level);
    mc.driveLevelClipForTest (true);  sweep ("level-clip");  mc.driveLevelClipForTest (false);
    eb::ShapeScalars s; s.driftMaxDb = -9.0f;
    auto marg = [] { eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradedMarginal;
                     e.sweepSnrDb = 18; e.irSnrDb = 52; e.thdPercent = 0.3f; e.peakDb = -8; return e; }();
    auto clean = [] { eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradedClean;
                      e.sweepSnrDb = 30; e.irSnrDb = 56; e.thdPercent = 0.8f; e.peakDb = -8; return e; }();
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    mc.driveVerdictForTest (eb::verdictCardModelAuto ("LEFT EAR", clean, 0u, {}, false, false),
                            eb::verdictCardModelAuto ("RIGHT EAR", marg, eb::ShapeFlag::kDrift, s, false, false),
                            true);
    sweep ("verdict-details");
    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    mc.calibrateStageForTest().setAdvancedOpen (true);  sweep ("cal-advanced");
    mc.calibrateStageForTest().setAdvancedOpen (false);

    INFO ("ASCII typography in visible copy:\n" << offenders.joinIntoString ("\n"));
    CHECK (offenders.isEmpty());
    tmp.deleteRecursively();
}

TEST_CASE("P4 unity wording: the accepted-state copy is pinned (the P2-T6 owed coverage)") {
    using CS = eb::CalibrateStage;
    CHECK (CS::unityHintText (false)
           == "Calibration is recommended" + eb::kDash + "without it the measurement isn't corrected for the EARS mic.");
    CHECK (CS::unityHintText (true)
           == "Continuing without calibration" + eb::kDash + "the measurement won't be corrected for the EARS mic.");
}
