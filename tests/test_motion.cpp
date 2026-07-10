#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MotionRamp.h"
#include "gui/SystemA11y.h"
#include "gui/DisclosureRow.h"
#include "gui/CaptureCard.h"
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

// ==================================================================================================
// P4 Task 3: Measure motion. The capturing border EASES IN; everything danger-adjacent SNAPS (frozen
// decision 3) - the Failed border never touches the ramp, and a mid-flight Capturing -> Failed flip
// STOPS the ramp (a Failed card must never carry a live 60 Hz ramp ticking repaints on a danger
// surface). Progress stays DATA-driven: a progress-only model update never restarts the ease.
// ==================================================================================================
TEST_CASE("P4 CaptureCard: capturing border eases in; Reduce Motion snaps; FAILED SNAPS ALWAYS") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CaptureCard card;
    card.setSize (273, 148);
    // NEGATIVE (RM ON): Waiting -> Capturing completes the ramp immediately.
    eb::SystemA11y::setForTest (true, false, false);
    card.setModel (eb::CaptureCardModel::waiting ("LEFT EAR", eb::CombineMode::AutoPerEar));
    card.setModel (eb::CaptureCardModel::capturing ("LEFT EAR", 0.2f, 10));
    CHECK (card.borderRampForTest().value() == 1.0f);
    CHECK_FALSE (card.borderRampForTest().running());
    // POSITIVE (RM OFF): the transition starts the one-shot ramp.
    eb::SystemA11y::setForTest (false, false, false);
    card.setModel (eb::CaptureCardModel::waiting ("LEFT EAR", eb::CombineMode::AutoPerEar));
    card.setModel (eb::CaptureCardModel::capturing ("LEFT EAR", 0.2f, 10));
    CHECK (card.borderRampForTest().value() < 0.05f);
    CHECK (card.borderRampForTest().running());
    card.borderRampForTest().finishForTest();
    CHECK (card.borderRampForTest().value() == 1.0f);
    // A progress-only update (same state) must NOT restart the ramp (30 Hz feed discipline).
    card.setModel (eb::CaptureCardModel::capturing ("LEFT EAR", 0.4f, 10));
    CHECK_FALSE (card.borderRampForTest().running());
    // DANGER SNAPS (frozen decision 3): Capturing -> Failed never animates - no ramp involvement.
    card.setModel (eb::CaptureCardModel::failed ("LEFT EAR"));
    CHECK_FALSE (card.borderRampForTest().running());
    // MID-FLIGHT failure (churn negative, past the brief): the flip stops a RUNNING ramp - the
    // danger surface owns the moment with the ramp at rest, not mid-ease.
    card.setModel (eb::CaptureCardModel::waiting ("LEFT EAR", eb::CombineMode::AutoPerEar));
    card.setModel (eb::CaptureCardModel::capturing ("LEFT EAR", 0.1f, 10));
    CHECK (card.borderRampForTest().running());
    card.setModel (eb::CaptureCardModel::failed ("LEFT EAR"));
    CHECK_FALSE (card.borderRampForTest().running());
    CHECK (card.borderRampForTest().value() == 1.0f);
    eb::SystemA11y::setForTest (false, false, false);
}

// ==================================================================================================
// P4 Task 5: disclosure a11y. getAccessibilityHandler() needs a native peer (vendored
// juce_Component.cpp:2992 - the ':2994' precondition from P2 T1), so the test calls the protected
// factory directly through a test subclass: same handler class, no peer, deterministic.
// ==================================================================================================
TEST_CASE("P4 DisclosureRow a11y: announces expandable + expanded/collapsed, not toggle on/off") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    struct TestRow : eb::DisclosureRow {
        TestRow() : DisclosureRow ("Advanced FIR") {}
        using eb::DisclosureRow::createAccessibilityHandler;   // expose the protected factory
    } row;
    auto handler = row.createAccessibilityHandler();
    REQUIRE (handler != nullptr);
    CHECK (handler->getRole() == juce::AccessibilityRole::button);
    auto closed = handler->getCurrentState();
    CHECK (closed.isExpandable());
    CHECK_FALSE (closed.isExpanded());
    CHECK_FALSE (closed.isCheckable());                        // the old toggle semantics are GONE
    row.setOpen (true);
    auto open = handler->getCurrentState();
    CHECK (open.isExpanded());
    // Locked-open row: still reports expanded (the state is the truth, the lock is behavior).
    row.setLocked (true);
    CHECK (handler->getCurrentState().isExpanded());
    // The summary rides the accessible description.
    row.setSummary ("Min phase - Auto length");
    CHECK (row.getDescription() == "Min phase - Auto length");
}

// Cold-gate T1-T5 fix-forward: the locked-revert path's no-visual-change claim gets its not-X. A
// click on a LOCKED row reverts silently - it must start NO chevron ramp (the comment in clicked()
// asserts it; this pins it) and the row stays open.
TEST_CASE("P4 DisclosureRow: clicking a locked row starts no ramp and stays open") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::SystemA11y::setForTest (false, false, false);          // animations allowed - a ramp COULD run
    eb::DisclosureRow row ("Advanced FIR");
    row.setOpen (true);
    row.chevronRampForTest().finishForTest();                  // settle the open ease
    row.setLocked (true);
    int changes = 0; row.onOpenChanged = [&] (bool) { ++changes; };
    row.clickForTest();                                        // user tries to collapse the locked row
    CHECK (row.isOpen());                                      // refused
    CHECK_FALSE (row.chevronRampForTest().running());          // NO ramp on the silent revert
    CHECK (row.chevronAmountForTest() == 1.0f);                // chevron never left the open glyph
    CHECK (changes == 0);                                      // no phantom change report
}
