#pragma once
#include <juce_core/juce_core.h>   // juce::String / jassert
#include "gui/Copy.h"   // P4 T6: typography constants (juce_core only)
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

// Shared machine-reason strings (minor-4): the SINGLE source of the Connect/Calibrate first-unmet reasons,
// used by BOTH the state machine (resolveConnect/resolveCalibrate) AND MainComponent's Start-gate helpText,
// so the two can never drift. The neutral machine phrasing has no trailing period; the helpText site appends
// one at composition (a full sentence for a screen reader). Inline functions (not constexpr String — juce::
// String is not a literal type) with static locals so each returns a single shared instance.
inline const juce::String& kReasonNoDevices()  { static const juce::String s ("Select an input and output device");        return s; }
inline const juce::String& kReasonNoCals()     { static const juce::String s ("Load both ear calibration files");          return s; }
inline const juce::String& kReasonWrongMode()  { static const juce::String s ("Set Combine Mode to Auto per-ear (Dirac)"); return s; }
inline const juce::String& kReasonPhysicalOut(){ static const juce::String s ("Choose a virtual cable output");            return s; }

struct WizardInputs {
    // GateSnapshot mirror (MainComponent feeds its own GateSnapshot fields verbatim)
    bool haveDevs = false, haveCals = false, wrongMode = false,
         physicalOutput = false, noCalsLoaded = false, gateReady = false;
    bool deviceError = false;        // deviceDied_/statusErrorMsg_ nonempty
    bool calProblem = false;         // a rejected pair is surfaced on a card
    bool calBuilding = false;        // async FIR generation in flight
    bool unityAccepted = false;      // explicit continue-without-cal (P2 T6: session-scoped, masked by noCalsLoaded, revoked on any load attempt)
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
        return { StepState::Error, "Device error" + kDash + "check the EARS and cable" };

    if (! in.haveDevs)
        return { StepState::Todo, kReasonNoDevices() };
    if (in.wrongMode && ! in.overrideOn)
        return { StepState::Todo, kReasonWrongMode() };
    if (in.physicalOutput && ! in.overrideOn)
        return { StepState::Todo, kReasonPhysicalOut() };

    return { StepState::Done, {} };
}

// Calibrate: NEVER Blocked (cal loading does not depend on devices — compute, don't cascade).
[[nodiscard]] inline StepStatus resolveCalibrate (const WizardInputs& in) {
    if (in.calProblem)
        return { StepState::Error, "A calibration file was rejected" + kDash + "see the card" };
    if (in.calBuilding)
        return { StepState::Todo, "Rebuilding filters" + kEllipsis };
    if ((in.haveCals && ! in.calBuilding && ! in.calProblem) || in.unityAccepted)
        return { StepState::Done, {} };
    return { StepState::Todo, kReasonNoCals() };
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

// ---- banner composition (§3.1) ------------------------------------------------------------------

struct BannerComposition {
    juce::String text;                       // "" = no banner
    WizardStep   target = WizardStep::Connect;
};

// The regression-banner rule, factored so BOTH the machine (against ws.active) and the VIEW (against the
// actually-SHOWN step, which may be a held pin EARLIER than ws.active) compose it identically: the first
// step with state == Error strictly BEFORE `shownStep`. Never fires when the shown step IS (or precedes)
// the broken one — that stage surfaces its own error natively. Pure: reads only the resolved steps[].
[[nodiscard]] inline BannerComposition composeBanner (const WizardState& ws, WizardStep shownStep) {
    const int shownIdx = (int) shownStep;
    for (int i = 0; i < shownIdx; ++i)
        if (ws.steps[i].state == StepState::Error)
            return { "Needs attention" + kDash + ws.steps[i].reason, (WizardStep) i };
    return {};
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

    // 5) Banner: a broken step BEFORE the active one. Never when the active step IS the broken step (that
    //    stage surfaces the error natively). Composed via the shared composeBanner() so the VIEW can re-run
    //    the SAME rule against a held-pin shown step earlier than active (§ minor-1). bannerTarget = the
    //    first such step; text = "Needs attention" + kDash + that step's reason.
    const auto banner = composeBanner (out, out.active);
    out.banner       = banner.text;
    out.bannerTarget = banner.target;
    // (The former "active == Measure with a regressed Connect/Calibrate" clause was DEAD: when active is
    // Measure, every earlier step is Done — a regressed Connect/Calibrate is Error, not Todo/Blocked, and
    // an Error step BEFORE the active one is already caught by composeBanner (its bannerTarget/text are
    // identical). Blocked upstream would keep Measure Blocked so active could never be Measure. Removed.)

    return out;
}

} // namespace eb
