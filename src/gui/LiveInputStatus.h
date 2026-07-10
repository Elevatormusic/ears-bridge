#pragma once
#include <juce_core/juce_core.h>
#include "gui/Copy.h"   // P4 T6: typography constants (juce_core only)

namespace eb {

// Live, in-sweep input-level readout. While Dirac is driving the sweep the user needs to SEE, in real time,
// (1) that the sweep is running and (2) the sweep PEAK in dBFS, so they can set the output level and catch a
// clip. Dirac sweeps the earcups SEQUENTIALLY (see below), so a live L-vs-R comparison is meaningless mid-
// sweep — an imbalance is judged AFTER the grade, not here. This pure helper maps the two per-channel peaks
// (already in dBFS, NOT clamped at 0 so a clipped float reads positive) + the debounced sweep-active flag to
// the exact status TEXT and a colour-free SEVERITY. The caller maps the severity to a Theme colour role; the
// helper never names a colour, so it is unit-testable without the GUI/JUCE LookAndFeel.
//
// HIG: the meaning is carried by the WORDS, never colour alone — "CLIPPED" + the explicit "+1.6 dBFS" reads
// fully without any colour, and the dB number is ALWAYS shown. The running peak-hold (so the digits don't
// jitter at 30 Hz) and the sweep-active debounce live in the GUI caller; this helper is a pure function of
// the (held) values it is handed.

enum class LiveInputSeverity {
    Normal,   // in-range live readout         -> Theme::text()
    Clip      // sweep peak at/over full scale -> Theme::danger()
};

struct LiveInputStatus {
    juce::String      text;                              // empty => defer to the idle per-ear verdicts
    LiveInputSeverity severity = LiveInputSeverity::Normal;
};

// peakLDb / peakRDb: the (peak-held) per-channel input level in dBFS, NOT clamped at 0 (a clip reads positive).
// sweepActive: the debounced "the sweep is sounding now" flag (held ~0.3 s above the room floor by the caller).
// Returns empty text when NOT sweep-active so the caller keeps the existing idle per-ear verdict lines.
[[nodiscard]] inline LiveInputStatus liveInputStatus (float peakLDb, float peakRDb, bool sweepActive) {
    LiveInputStatus s;
    if (! sweepActive) return s;   // idle: defer to the per-ear verdicts (do NOT regress them)

    // Dirac HARD-PANS its per-ear sweeps SEQUENTIALLY (L earcup, then R), so at any instant ONLY ONE earcup is
    // sounding and the other is legitimately silent / crosstalk. A live L-vs-R comparison is therefore meaningless
    // mid-sweep: the idle earcup is NOT "low - reseat", it just hasn't been swept yet. (That false "(R low -
    // reseat the earcup?)" fired on the OPPOSITE ear during every sweep.) So the live readout reports only the
    // PEAK - the loudest the input reached this sweep - which is exactly what's needed to set the level and catch
    // a clip. The L/R imbalance is judged AFTER the grade (both earcups measured), never live.
    const float peakMax = juce::jmax (peakLDb, peakRDb);

    if (peakMax >= 0.0f) {   // the sweep PEAK hit/over full scale on the loudest earcup (held, so past-tense)
        s.text     = "CLIPPED  +" + juce::String (peakMax, 1) + " dBFS" + kDash + "lower the output";
        s.severity = LiveInputSeverity::Clip;
        return s;
    }
    s.text     = "Sweep in progress" + kDash + "peak " + juce::String (peakMax, 1) + " dBFS";
    s.severity = LiveInputSeverity::Normal;
    return s;
}

// ---- Status-line PHASE state machine (pure; unit-tested without the GUI) -------------------------------
//
// updateStatusLine() picks ONE of four high-level phases each tick; the GUI then renders the chosen phase
// with set-if-changed (no clear-then-reset) so the line is persistent and never flickers. Factored out here
// as a pure function so the decision — especially "a captured grade must beat the pre-sweep waiting text"
// (Bug A) and "a single sub-gate tick during a held sweep stays LiveSweep" (Bug B) — is directly testable.
//
//   Error     -> a hard, invalidating condition is latched (invalid capture / output-clip / low-SNR / 48k
//                veto). The caller renders the SPECIFIC error text; it always wins.
//   LiveSweep -> Dirac is audibly sweeping right now (the debounced+held sweep-active flag). Show the live
//                per-channel readout. Wins over Captured/Waiting so the user sees the level move / catch a clip.
//   Captured  -> a grade exists this run (an ear is GradedClean/GradedSuspect/ReferenceStale) and the sweep
//                is NOT sounding -> show the PERSISTENT "captured - safe to run the next" confirmation + the
//                per-ear verdict. This is the Bug-A fix: it beats Waiting once anything has graded.
//   Waiting   -> Running, a reference is loaded, nothing has graded yet this run, the sweep isn't sounding ->
//                the genuine pre-first-sweep "listening for the Dirac sweep" wait.
enum class StatusPhase { Error, LiveSweep, Captured, Waiting };

// running:      the engine is in the Running state (false -> the caller handles the non-running ladder itself).
// hasGrade:     a per-ear reference is loaded AND at least one ear has a graded verdict this run.
// sweepActive:  the debounced+release-held "the sweep is sounding now" flag (see sweepActiveWithHold).
// anyHardError: an invalidating condition is latched (invalid capture / output-clip / low-SNR / 48k veto) —
//               it must win over a "safe to run the next" confirmation (don't say safe over a clipped sweep).
[[nodiscard]] inline StatusPhase statusPhase (bool running, bool hasGrade, bool sweepActive, bool anyHardError) {
    if (! running)     return StatusPhase::Waiting;   // caller owns the non-running text; phase is a placeholder
    if (anyHardError)  return StatusPhase::Error;     // a latched invalidating condition always wins
    if (sweepActive)   return StatusPhase::LiveSweep;  // the sweep is sounding -> show the live readout
    if (hasGrade)      return StatusPhase::Captured;   // Bug A: a captured grade beats the pre-sweep waiting text
    return StatusPhase::Waiting;                       // pre-first-sweep wait
}

// Attack/release debounce for the sweep-active flag, as a PURE step so the release HOLD is unit-testable.
// `release` is the countdown of remaining held ticks. When the live level is above the gate we re-arm the
// counter to its full hold (attack is applied separately by the caller's consecutive-above count); when it
// dips below the gate we DECREMENT rather than reset to 0, so the quiet L<->R inter-sweep gap and brief dips
// don't flip the live readout off (Bug B-2). Returns the next countdown; "engaged" is next > 0.
[[nodiscard]] inline int sweepActiveRelease (int release, bool aboveGate, int holdTicks) {
    if (aboveGate) return holdTicks;                  // re-arm: hold for `holdTicks` more ticks after this
    return release > 0 ? release - 1 : 0;             // dipped below: bleed the hold down, don't snap to 0
}

} // namespace eb
