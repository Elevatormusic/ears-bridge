#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MotionRamp.h"
#include "gui/SystemA11y.h"
#include "gui/DisclosureRow.h"
#include "gui/MainComponent.h"

// ==================================================================================================
// P4 Task 1: the ONE motion primitive. One-shot, self-terminating, Reduce-Motion snaps. The rest
// state of a NEVER-started ramp is 1.0 (consumers multiply alphas by value(); a fresh ramp must not
// dim anything). Headless determinism: assertions read the ramp VALUE, never the wall clock;
// finishForTest() completes synchronously.
// ==================================================================================================
TEST_CASE("MotionRamp: rest state is 1.0; Reduce Motion snaps with no timer") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::MotionRamp r;
    CHECK (r.value() == 1.0f);                       // never-started = completed
    CHECK_FALSE (r.running());

    eb::SystemA11y::setForTest (true, false, false); // Reduce Motion ON
    int ticks = 0; r.onTick = [&] { ++ticks; };
    r.start();
    CHECK (r.value() == 1.0f);                       // snapped, immediately
    CHECK_FALSE (r.running());                       // NO timer under the flag (the RM negative)
    CHECK (ticks == 1);                              // exactly one apply
    eb::SystemA11y::setForTest (false, false, false);
}

TEST_CASE("MotionRamp: animates from 0, finishForTest completes, timer self-terminates") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::SystemA11y::setForTest (false, false, false);
    eb::MotionRamp r;
    r.start();
    CHECK (r.value() < 0.05f);                       // just started: eased 0
    CHECK (r.running());
    r.finishForTest();
    CHECK (r.value() == 1.0f);
    CHECK_FALSE (r.running());

    // Live smoke: a real ramp completes and STOPS ITS OWN TIMER (the churn negative - no standing
    // timers at rest). NOTE: the runDispatchLoopUntil idiom needs JUCE_MODAL_LOOPS_PERMITTED, which
    // this project leaves OFF (see the test_wizardnav.cpp Level-soft-gate note) - so pump the JUCE
    // timer queue directly instead. 600ms deadline for a 160ms ramp = generous CI margin.
    r.start();
    const auto deadline = juce::Time::getMillisecondCounter() + 600;
    while (r.running() && juce::Time::getMillisecondCounter() < deadline) {
        juce::Thread::sleep (5);
        juce::Timer::callPendingTimersSynchronously();
    }
    CHECK (r.value() == 1.0f);
    CHECK_FALSE (r.running());
}

TEST_CASE("P4 frozen policy: stage switches are INSTANT - shown stage fully opaque immediately") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    eb::SystemA11y::setForTest (false, false, false);        // even with animations allowed...
    mc.forceWizardStepForTest (eb::WizardStep::Level);
    auto& lvl = mc.levelStageForTest();
    CHECK (lvl.isVisible());                                  // ...the switch is complete NOW
    CHECK (lvl.getAlpha() == 1.0f);                           // no fade-in, ever (frozen decision 1)
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    CHECK_FALSE (lvl.isVisible());                            // outgoing stage gone NOW (no fade-out)
    CHECK (mc.measureStageForTest().getAlpha() == 1.0f);
    tmp.deleteRecursively();
}

// ==================================================================================================
// P4 Task 2: disclosure motion. Per-surface Reduce-Motion NEGATIVES first (the flag turns the
// animation OFF — end-state immediate, no timer), then the positive path via ramp seams (headless
// timers never tick on their own; finishForTest() is the deterministic clock).
// ==================================================================================================
TEST_CASE("P4 chevron: Reduce Motion snaps the rotation; animated path eases then completes") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::DisclosureRow row ("Advanced FIR");
    // NEGATIVE (RM ON): open -> chevron fully rotated immediately.
    eb::SystemA11y::setForTest (true, false, false);
    row.setOpen (true);
    CHECK (row.chevronAmountForTest() == 1.0f);
    row.setOpen (false);
    CHECK (row.chevronAmountForTest() == 0.0f);
    // POSITIVE (RM OFF): open -> mid-rotation until the ramp completes.
    eb::SystemA11y::setForTest (false, false, false);
    row.setOpen (true);
    CHECK (row.chevronAmountForTest() < 0.05f);       // ramp just started
    row.chevronRampForTest().finishForTest();
    CHECK (row.chevronAmountForTest() == 1.0f);
    row.setOpen (false);                               // closing rotates BACK through the same ramp
    CHECK (row.chevronAmountForTest() > 0.95f);        // still pointing open at t=0...
    row.chevronRampForTest().finishForTest();
    CHECK (row.chevronAmountForTest() == 0.0f);        // ...fully closed at t=1
}

TEST_CASE("P4 section reveal: Calibrate advanced fades in on open; Reduce Motion snaps; close instant") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    auto& stage = mc.calibrateStageForTest();
    auto& toggle = mc.complexPhaseToggleForTest();             // one representative section control
    // NEGATIVE (RM ON): open -> the revealed control is at FULL alpha immediately, no ramp running.
    eb::SystemA11y::setForTest (true, false, false);
    stage.setAdvancedOpen (true);
    CHECK (toggle.getAlpha() == 1.0f);
    CHECK_FALSE (stage.revealRampForTest().running());
    stage.setAdvancedOpen (false);
    // POSITIVE (RM OFF): open -> alpha starts near 0, completes to 1 on finish.
    eb::SystemA11y::setForTest (false, false, false);
    stage.setAdvancedOpen (true);
    CHECK (toggle.getAlpha() < 0.05f);
    stage.revealRampForTest().finishForTest();
    CHECK (toggle.getAlpha() == 1.0f);
    // CLOSE IS INSTANT (frozen decision 2): alpha restored, no ramp started.
    stage.setAdvancedOpen (false);
    CHECK (toggle.getAlpha() == 1.0f);
    CHECK_FALSE (stage.revealRampForTest().running());
    tmp.deleteRecursively();
}

TEST_CASE("P4 section reveal: Connect override section - same contract") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    auto& stage = mc.connectStageForTest();
    auto& ovr = mc.overrideToggleForTest();
    eb::SystemA11y::setForTest (true, false, false);
    stage.notUsingDiracForTest().setOpen (true);
    CHECK (ovr.getAlpha() == 1.0f);                            // RM negative
    stage.notUsingDiracForTest().setOpen (false);
    eb::SystemA11y::setForTest (false, false, false);
    stage.notUsingDiracForTest().setOpen (true);
    CHECK (ovr.getAlpha() < 0.05f);
    stage.revealRampForTest().finishForTest();
    CHECK (ovr.getAlpha() == 1.0f);
    stage.notUsingDiracForTest().setOpen (false);
    CHECK (ovr.getAlpha() == 1.0f);                            // close restored alpha instantly
    tmp.deleteRecursively();
}
