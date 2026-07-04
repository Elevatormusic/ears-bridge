#include <catch2/catch_test_macros.hpp>
#include "gui/WizardState.h"

// Headless tests for eb::computeWizardState — the PURE 4-step wizard state machine (P1 Task 1,
// spec docs/superpowers/specs/2026-07-04-wizard-redesign-design.md §3). Semantics are FROZEN:
//   - never auto-advance; done-ness is COMPUTED, never stored;
//   - per-step first-unmet resolution (Connect -> Calibrate -> Level -> Measure);
//   - pin legality: an Error step IS navigable, a Blocked step is NOT (falls back to first unmet);
//   - Calibrate is NEVER Blocked (compute cal-loading independently, don't cascade from devices);
//   - Measure is Blocked EXACTLY when Level is Blocked (the soft Level gate never blocks Measure);
//   - the active step renders Active in steps[] ONLY if it was Todo (Done/Error stay themselves);
//   - a banner appears only for a broken step BEFORE the active one (or Measure w/ a regressed
//     Connect/Calibrate) — never when the active step IS the broken one.
// The 12 load-bearing cases from the task brief are covered as named TEST_CASEs below.

using eb::WizardStep;
using eb::StepState;
using eb::WizardInputs;
using eb::WizardState;

// A fully-satisfied input set: all four steps Done, both ears graded at the current generation.
static WizardInputs allDone() {
    WizardInputs in;
    in.haveDevs = true;
    in.haveCals = true;
    in.wrongMode = false;
    in.physicalOutput = false;
    in.levelLatched = true;
    in.engineRunning = true;
    in.referenceLoaded = true;
    in.configGen = 5;
    in.earGradedL = true; in.verdictGenL = 5;
    in.earGradedR = true; in.verdictGenR = 5;
    return in;
}

// ---- 1. Fresh launch: all defaults --------------------------------------------------------------

TEST_CASE("WizardState fresh launch: Connect active Todo, Level and Measure Blocked, no banner") {
    WizardInputs in;   // all defaults (nothing satisfied)
    const auto s = eb::computeWizardState (in, std::nullopt);

    CHECK (s.active == WizardStep::Connect);
    // The active step was Todo, so it renders Active in steps[].
    CHECK (s.steps[(int) WizardStep::Connect].state == StepState::Active);
    CHECK (s.steps[(int) WizardStep::Connect].reason == juce::String ("Select an input and output device"));
    CHECK (s.steps[(int) WizardStep::Level].state == StepState::Blocked);
    CHECK (s.steps[(int) WizardStep::Measure].state == StepState::Blocked);
    CHECK (s.banner.isEmpty());
}

// ---- 2. Devices selected but wrong combine mode -------------------------------------------------

TEST_CASE("WizardState wrongMode: Connect Todo with the combine reason; overrideOn flips it Done") {
    WizardInputs in;
    in.haveDevs = true;
    in.wrongMode = true;

    const auto todo = eb::computeWizardState (in, std::nullopt);
    CHECK (todo.steps[(int) WizardStep::Connect].state == StepState::Active);   // active+Todo -> Active
    CHECK (todo.steps[(int) WizardStep::Connect].reason
           == juce::String ("Set Combine Mode to Auto per-ear (Dirac)"));

    in.overrideOn = true;
    const auto done = eb::computeWizardState (in, std::nullopt);
    CHECK (done.steps[(int) WizardStep::Connect].state == StepState::Done);
}

// ---- 3. All four satisfied ----------------------------------------------------------------------

TEST_CASE("WizardState all satisfied: every step Done, active = Measure") {
    const auto s = eb::computeWizardState (allDone(), std::nullopt);
    CHECK (s.steps[(int) WizardStep::Connect].state   == StepState::Done);
    CHECK (s.steps[(int) WizardStep::Calibrate].state == StepState::Done);
    CHECK (s.steps[(int) WizardStep::Level].state     == StepState::Done);
    CHECK (s.steps[(int) WizardStep::Measure].state   == StepState::Done);
    CHECK (s.active == WizardStep::Measure);
    CHECK (s.banner.isEmpty());
    CHECK (s.verdictsStale == false);
}

// ---- 4. First-unmet resolution: cals but no devices --------------------------------------------

TEST_CASE("WizardState first-unmet: cals loaded but no devices -> active = Connect") {
    WizardInputs in;
    in.haveCals = true;   // Calibrate would be Done, but Connect is not
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.active == WizardStep::Connect);
    CHECK (s.steps[(int) WizardStep::Calibrate].state == StepState::Done);
    CHECK (s.steps[(int) WizardStep::Connect].state   == StepState::Active);
}

// ---- 5. Pin legality ----------------------------------------------------------------------------

TEST_CASE("WizardState pin legality: Calibrate honored (never Blocked); Measure pin falls back") {
    WizardInputs in;   // nothing satisfied -> Connect Todo, Calibrate Todo, Level+Measure Blocked

    // Calibrate is never Blocked, so pinning it while Connect is Todo is honored.
    const auto pinCal = eb::computeWizardState (in, WizardStep::Calibrate);
    CHECK (pinCal.active == WizardStep::Calibrate);
    CHECK (pinCal.steps[(int) WizardStep::Calibrate].state == StepState::Active);   // Todo shown Active

    // Measure is Blocked (Level deps unmet), so pinning it falls back to the first unmet = Connect.
    const auto pinMeasure = eb::computeWizardState (in, WizardStep::Measure);
    CHECK (pinMeasure.active == WizardStep::Connect);
    CHECK (pinMeasure.steps[(int) WizardStep::Measure].state == StepState::Blocked);
}

// ---- 6. Error navigability ----------------------------------------------------------------------

TEST_CASE("WizardState error navigability: deviceError + pinned Connect -> active Connect, Error") {
    WizardInputs in;
    in.deviceError = true;
    const auto s = eb::computeWizardState (in, WizardStep::Connect);
    CHECK (s.active == WizardStep::Connect);
    CHECK (s.steps[(int) WizardStep::Connect].state == StepState::Error);   // Error stays Error, not Active
    CHECK (s.steps[(int) WizardStep::Connect].reason
           == juce::String ("Device error - check the EARS and cable"));
    // Active step IS the broken step -> no banner.
    CHECK (s.banner.isEmpty());
}

// ---- 6b. Error pin navigability while an earlier step is un-started ------------------------------
// An Error step must be navigable EVEN WHEN it is not the first-unmet — otherwise an implementation
// that only honored a pin on the first-unmet step (rejecting Error pins) would still pass all the
// cases above. Here Connect is Todo (no devices) and Calibrate is Error (calProblem); pinning
// Calibrate must land there in Error, not fall back to first-unmet (Connect).
TEST_CASE("WizardState error pin: Connect Todo + calProblem, pinned Calibrate -> active Calibrate, Error") {
    WizardInputs in;            // no devices -> Connect Todo (Active), Calibrate would be Error
    in.calProblem = true;
    const auto s = eb::computeWizardState (in, WizardStep::Calibrate);
    CHECK (s.active == WizardStep::Calibrate);                              // Error pin honored, not first-unmet
    CHECK (s.steps[(int) WizardStep::Calibrate].state == StepState::Error);  // stays Error (navigable)
    // Connect is the first-unmet but NOT the active step (Calibrate is pinned), so it stays Todo — only
    // the ACTIVE Todo step is promoted to Active. This is the load-bearing distinction: the pin landed on
    // Calibrate (an Error step that is not first-unmet), proving Error pins are honored.
    CHECK (s.steps[(int) WizardStep::Connect].state == StepState::Todo);
    // Active step IS the broken (Error) step -> no banner; and Connect (Todo, before it) is not Error.
    CHECK (s.banner.isEmpty());
}

// ---- 7. calBuilding -----------------------------------------------------------------------------

TEST_CASE("WizardState calBuilding: Calibrate Todo Rebuilding filters, NOT Done, even with haveCals") {
    WizardInputs in;
    in.haveDevs = true;   // Connect Done so Calibrate is the first unmet / active
    in.haveCals = true;
    in.calBuilding = true;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Calibrate].state == StepState::Active);   // active+Todo -> Active
    CHECK (s.steps[(int) WizardStep::Calibrate].reason == juce::String ("Rebuilding filters..."));
    CHECK (s.active == WizardStep::Calibrate);
}

// ---- 8. Soft Level: latch false does not block Measure -----------------------------------------

TEST_CASE("WizardState soft Level: levelLatched false but Connect+Calibrate Done -> Measure not Blocked") {
    WizardInputs in;
    in.haveDevs = true;
    in.haveCals = true;
    in.levelLatched = false;
    in.engineRunning = false;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Level].state   != StepState::Blocked);
    CHECK (s.steps[(int) WizardStep::Measure].state != StepState::Blocked);
    // Level is the first unmet -> active + Todo -> Active; reason is the pre-monitoring one.
    CHECK (s.active == WizardStep::Level);
    CHECK (s.steps[(int) WizardStep::Level].state == StepState::Active);
    CHECK (s.steps[(int) WizardStep::Level].reason == juce::String ("Start monitoring and set the level"));
}

TEST_CASE("WizardState soft Level: engine running gives the green-band reason") {
    WizardInputs in;
    in.haveDevs = true;
    in.haveCals = true;
    in.engineRunning = true;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Level].reason == juce::String ("Raise Dirac's output to the green band"));
}

// ---- 9. Regression banner -----------------------------------------------------------------------

TEST_CASE("WizardState regression banner: all Done, pin Measure, then deviceError -> banner to Connect") {
    WizardInputs in = allDone();
    in.deviceError = true;   // Connect regresses to Error while user sits on Measure

    const auto pinnedMeasure = eb::computeWizardState (in, WizardStep::Measure);
    CHECK (pinnedMeasure.active == WizardStep::Measure);
    CHECK (pinnedMeasure.steps[(int) WizardStep::Connect].state == StepState::Error);
    CHECK (pinnedMeasure.banner.isNotEmpty());
    CHECK (pinnedMeasure.bannerTarget == WizardStep::Connect);
    CHECK (pinnedMeasure.banner.contains ("Device error - check the EARS and cable"));

    // AND the banner is EMPTY when the user is pinned on the broken step itself.
    const auto pinnedConnect = eb::computeWizardState (in, WizardStep::Connect);
    CHECK (pinnedConnect.active == WizardStep::Connect);
    CHECK (pinnedConnect.banner.isEmpty());
}

// ---- 10. Stale verdicts -------------------------------------------------------------------------

TEST_CASE("WizardState stale verdicts: ears graded at gen 3, configGen 4 -> Measure Todo, stale true") {
    WizardInputs in;
    in.haveDevs = true;
    in.haveCals = true;
    in.levelLatched = true;
    in.engineRunning = true;
    in.referenceLoaded = true;
    in.configGen = 4;
    in.earGradedL = true; in.verdictGenL = 3;
    in.earGradedR = true; in.verdictGenR = 3;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Measure].state != StepState::Done);   // stale -> not Done
    CHECK (s.verdictsStale == true);
    CHECK (s.active == WizardStep::Measure);   // first unmet with the others Done
}

// ---- 11. unityAccepted --------------------------------------------------------------------------

TEST_CASE("WizardState unityAccepted: Calibrate Done with haveCals false") {
    WizardInputs in;
    in.haveDevs = true;
    in.haveCals = false;
    in.unityAccepted = true;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Calibrate].state == StepState::Done);
}

// ---- 12. Determinism ----------------------------------------------------------------------------

static bool sameState (const WizardState& a, const WizardState& b) {
    if (a.active != b.active || a.bannerTarget != b.bannerTarget
        || a.verdictsStale != b.verdictsStale || a.banner != b.banner)
        return false;
    for (int i = 0; i < eb::kWizardStepCount; ++i)
        if (a.steps[i].state != b.steps[i].state || a.steps[i].reason != b.steps[i].reason)
            return false;
    return true;
}

TEST_CASE("WizardState determinism: same inputs twice -> identical outputs") {
    WizardInputs in = allDone();
    in.deviceError = true;   // exercise a banner-bearing state too
    const auto a = eb::computeWizardState (in, WizardStep::Measure);
    const auto b = eb::computeWizardState (in, WizardStep::Measure);
    CHECK (sameState (a, b));

    WizardInputs fresh;   // and the default state
    CHECK (sameState (eb::computeWizardState (fresh, std::nullopt),
                      eb::computeWizardState (fresh, std::nullopt)));
}

// ---- extra: calProblem is an Error on Calibrate (never Blocked) ---------------------------------

TEST_CASE("WizardState calProblem: Calibrate Error even when Connect is not Done") {
    WizardInputs in;   // no devices -> Connect Todo, but a cal pair was rejected
    in.calProblem = true;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Calibrate].state == StepState::Error);
    CHECK (s.steps[(int) WizardStep::Calibrate].reason
           == juce::String ("A calibration file was rejected - see the card"));
    // Active is still Connect (first unmet); a broken step BEFORE the active one has no banner
    // because Calibrate is AFTER Connect. No banner.
    CHECK (s.active == WizardStep::Connect);
    CHECK (s.banner.isEmpty());
}

// ---- extra: Connect first-unmet order (physicalOutput reason) -----------------------------------

TEST_CASE("WizardState Connect first-unmet order: devs ok, physicalOutput -> the virtual-cable reason") {
    WizardInputs in;
    in.haveDevs = true;
    in.physicalOutput = true;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Connect].reason == juce::String ("Choose a virtual cable output"));

    in.overrideOn = true;   // override clears the physical-output objection
    const auto ok = eb::computeWizardState (in, std::nullopt);
    CHECK (ok.steps[(int) WizardStep::Connect].state == StepState::Done);
}

// ---- extra: Measure neutral Todo reasons (Learn reference vs run-in-Dirac) ----------------------

TEST_CASE("WizardState Measure Todo reasons: Learn the reference when no reference and not hwDirac") {
    WizardInputs in;
    in.haveDevs = true;
    in.haveCals = true;
    in.levelLatched = true;
    in.referenceLoaded = false;
    in.hwDirac = false;
    const auto s = eb::computeWizardState (in, std::nullopt);
    CHECK (s.steps[(int) WizardStep::Measure].reason == juce::String ("Learn the reference"));

    in.referenceLoaded = true;   // now the run-in-Dirac neutral reason
    const auto run = eb::computeWizardState (in, std::nullopt);
    CHECK (run.steps[(int) WizardStep::Measure].reason == juce::String ("Run the measurement in Dirac Live"));
}
