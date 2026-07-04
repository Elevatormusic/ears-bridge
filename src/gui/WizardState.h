#pragma once
#include <juce_core/juce_core.h>   // juce::String / jassert
#include <optional>

// The pure, headless 4-step wizard state machine (P1 Task 1 of the guided-wizard redesign).
// Spec: docs/superpowers/specs/2026-07-04-wizard-redesign-design.md §3 (interaction rules §3.1,
// step predicates §3.2, the pure machine §3.4). The semantics here are FROZEN (research-validated,
// §12) — do not deviate without a spec change.
//
// The contract this header encodes, verbatim from §3.1/§3.2:
//   - NEVER auto-advance. This function computes state; navigation is a user act (CTA/spine click)
//     carried by WizardState::active. It never decides to *move* the user on its own — the only
//     automatic stage selection is launch (first unmet step; all met => Measure).
//   - Done-ness is COMPUTED, never stored. Back-navigation is edit-in-place: a later step stays Done
//     iff its own predicates still hold. Nothing is mutated by navigation.
//   - First-unmet resolution: the resting active step is the first (Connect -> Measure) whose state
//     is not Done; all Done => Measure (the expert/resting state).
//   - Pin legality: a userPinned step is honored UNLESS it is Blocked (an Error step IS navigable —
//     you go there to fix it). A pinned Blocked step falls back to first-unmet.
//   - Blocked (prerequisite unmet) is visually/semantically distinct from Error (a done predicate
//     regressed). Downstream steps downgrade only where their predicates actually depend on the
//     broken one (COMPUTED, not cascaded) — hence Calibrate is never Blocked, and the soft Level
//     gate never blocks Measure.
//   - The ACTIVE step renders as StepState::Active in steps[] ONLY if it was Todo; a Done or Error
//     active step stays Done/Error (the spine shows state truthfully; "which stage is open" is
//     carried separately by WizardState::active).
//   - Banner: shown only for a broken step BEFORE the active one (or when the active step is Measure
//     and Connect/Calibrate regressed to non-Done). Never shown when the active step IS the broken
//     step (that stage surfaces the error natively).
//
// Pure: header-only, no statics, no OS reads, no JUCE beyond juce::String/jassert. All reasons are
// the neutral machine strings; the view layer adds summaries and the §7 copy variants.

namespace eb {

enum class WizardStep : int { Connect = 0, Calibrate = 1, Level = 2, Measure = 3 };
constexpr int kWizardStepCount = 4;

struct WizardInputs {
    // GateSnapshot mirror (MainComponent feeds its own GateSnapshot fields verbatim)
    bool haveDevs = false, haveCals = false, wrongMode = false,
         physicalOutput = false, noCalsLoaded = false, gateReady = false;
    bool deviceError = false;        // deviceDied_/statusErrorMsg_ nonempty
    bool calProblem = false;         // a rejected pair is surfaced on a card
    bool calBuilding = false;        // async FIR generation in flight
    bool unityAccepted = false;      // explicit continue-without-cal (P1: always false)
    bool engineRunning = false;
    bool levelLatched = false;
    bool referenceLoaded = false;
    bool hwDirac = false, overrideOn = false;
    int  configGen = 0;              // calGenCounter_ snapshot
    int  verdictGenL = -1, verdictGenR = -1;   // generation stamped on each ear's last verdict
    bool earGradedL = false, earGradedR = false;
};

enum class StepState : int { Blocked, Todo, Active, Done, Error };

struct StepStatus {
    StepState    state = StepState::Todo;
    juce::String reason;             // Error/Blocked/Todo machine reason ("" for Done; view adds summaries)
};

struct WizardState {
    StepStatus  steps[kWizardStepCount];
    WizardStep  active = WizardStep::Connect;
    juce::String banner;             // "" = none
    WizardStep  bannerTarget = WizardStep::Connect;
    bool        verdictsStale = false;
};

// ---- per-step resolution (§3.2) -----------------------------------------------------------------

// Connect: Error iff deviceError; else Done iff devices present and (mode/output objections cleared
// or overridden); else Todo with the FIRST unmet reason.
[[nodiscard]] inline StepStatus resolveConnect (const WizardInputs& in) {
    if (in.deviceError)
        return { StepState::Error, juce::String ("Device error - check the EARS and cable") };

    if (! in.haveDevs)
        return { StepState::Todo, juce::String ("Select an input and output device") };
    if (in.wrongMode && ! in.overrideOn)
        return { StepState::Todo, juce::String ("Set Combine Mode to Auto per-ear (Dirac)") };
    if (in.physicalOutput && ! in.overrideOn)
        return { StepState::Todo, juce::String ("Choose a virtual cable output") };

    return { StepState::Done, {} };
}

// Calibrate: NEVER Blocked (cal loading does not depend on devices — compute, don't cascade).
[[nodiscard]] inline StepStatus resolveCalibrate (const WizardInputs& in) {
    if (in.calProblem)
        return { StepState::Error, juce::String ("A calibration file was rejected - see the card") };
    if (in.calBuilding)
        return { StepState::Todo, juce::String ("Rebuilding filters...") };
    if ((in.haveCals && ! in.calBuilding && ! in.calProblem) || in.unityAccepted)
        return { StepState::Done, {} };
    return { StepState::Todo, juce::String ("Load both ear calibration files") };
}

// Level: Blocked while its prerequisites are genuinely un-started (Connect/Calibrate not yet Done and
// not merely regressed). Soft gate — Done iff levelLatched. A regressed (Error) prerequisite leaves
// Level un-Blocked but not Done (its Todo content shows; the regression banner points back).
[[nodiscard]] inline StepStatus resolveLevel (const WizardInputs& in, bool depsBlocking, bool depsDone) {
    if (depsBlocking)
        return { StepState::Blocked, juce::String ("Finish Connect and Calibrate first") };
    if (depsDone && in.levelLatched)
        return { StepState::Done, {} };
    return { StepState::Todo, in.engineRunning
                 ? juce::String ("Raise Dirac's output to the green band")
                 : juce::String ("Start monitoring and set the level") };
}

// Measure: Blocked EXACTLY when Level is Blocked (same dependency; the soft Level gate never blocks
// Measure — §3.2). Done iff both ears graded at the CURRENT generation.
[[nodiscard]] inline StepStatus resolveMeasure (const WizardInputs& in, bool depsBlocking, bool depsDone) {
    if (depsBlocking)
        return { StepState::Blocked, juce::String ("Finish Connect and Calibrate first") };
    (void) depsDone;

    const bool bothGradedAtGen = in.earGradedL && in.verdictGenL == in.configGen
                              && in.earGradedR && in.verdictGenR == in.configGen;
    if (bothGradedAtGen)
        return { StepState::Done, {} };

    return { StepState::Todo, (! in.referenceLoaded && ! in.hwDirac)
                 ? juce::String ("Learn the reference")
                 : juce::String ("Run the measurement in Dirac Live") };
}

// ---- the machine --------------------------------------------------------------------------------

[[nodiscard]] inline WizardState computeWizardState (const WizardInputs& in,
                                                     std::optional<WizardStep> userPinned) {
    WizardState out;

    // 1) Resolve each step's INTRINSIC state (done-ness computed, never stored; no cascade beyond
    //    the two real dependencies Level/Measure carry).
    out.steps[(int) WizardStep::Connect]   = resolveConnect (in);
    out.steps[(int) WizardStep::Calibrate] = resolveCalibrate (in);

    // §3.1 distinguishes Blocked ("prerequisite unmet ... cannot start yet", the never-done case)
    // from Error (a previously-met predicate REGRESSED — navigable, you jump there to fix it). So a
    // dependency blocks a downstream step only when it is genuinely un-started (Todo/Blocked); a
    // dependency that regressed to Error must NOT re-Block the downstream (that would auto-navigate
    // the user off the step they pinned, which §3.1 forbids — the regression banner points back
    // instead). Hence: deps "block" iff neither Done nor Error; the Level Done/Todo content still
    // needs both deps genuinely Done.
    const auto depState = [&out](WizardStep s) { return out.steps[(int) s].state; };
    const bool depsBlocking = ! ((depState (WizardStep::Connect)   == StepState::Done
                                  || depState (WizardStep::Connect)   == StepState::Error)
                                 && (depState (WizardStep::Calibrate) == StepState::Done
                                     || depState (WizardStep::Calibrate) == StepState::Error));
    const bool depsDone = depState (WizardStep::Connect)   == StepState::Done
                       && depState (WizardStep::Calibrate) == StepState::Done;

    out.steps[(int) WizardStep::Level]   = resolveLevel (in, depsBlocking, depsDone);
    out.steps[(int) WizardStep::Measure] = resolveMeasure (in, depsBlocking, depsDone);

    // 2) verdictsStale: any ear graded with verdictGen < configGen (evidence downgraded, not deleted).
    out.verdictsStale = (in.earGradedL && in.verdictGenL < in.configGen)
                     || (in.earGradedR && in.verdictGenR < in.configGen);

    // 3) Resolve the active step. A pin is honored unless it is Blocked (Error IS navigable).
    //    Otherwise: the first step whose state != Done; all Done => Measure.
    const auto firstUnmet = [&out]() -> WizardStep {
        for (int i = 0; i < kWizardStepCount; ++i)
            if (out.steps[i].state != StepState::Done)
                return (WizardStep) i;
        return WizardStep::Measure;
    };

    if (userPinned && out.steps[(int) *userPinned].state != StepState::Blocked)
        out.active = *userPinned;
    else
        out.active = firstUnmet();

    // 4) The active step renders as Active in steps[] ONLY if it was Todo (Done/Error stay themselves).
    auto& activeStatus = out.steps[(int) out.active];
    if (activeStatus.state == StepState::Todo)
        activeStatus.state = StepState::Active;

    // 5) Banner: a broken step BEFORE the active one, OR (active == Measure) a regressed
    //    Connect/Calibrate. Never when the active step IS the broken step. bannerTarget = the first
    //    such step; text = "Needs attention - " + that step's reason.
    const int activeIdx = (int) out.active;
    for (int i = 0; i < activeIdx; ++i) {
        if (out.steps[i].state == StepState::Error) {
            out.banner       = juce::String ("Needs attention - ") + out.steps[i].reason;
            out.bannerTarget = (WizardStep) i;
            break;
        }
    }
    if (out.banner.isEmpty() && out.active == WizardStep::Measure) {
        for (int i = 0; i <= (int) WizardStep::Calibrate; ++i) {
            if (out.steps[i].state != StepState::Done) {
                out.banner       = juce::String ("Needs attention - ") + out.steps[i].reason;
                out.bannerTarget = (WizardStep) i;
                break;
            }
        }
    }

    return out;
}

} // namespace eb
