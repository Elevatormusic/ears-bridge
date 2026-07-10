#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MotionRamp.h"
#include "gui/SystemA11y.h"
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
