#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Live, in-sweep input-level readout. While Dirac is driving the sweep the user needs to SEE, in real time,
// (1) that the sweep is running, (2) the per-channel input level in dBFS, (3) a clip the instant it happens,
// and (4) a gross L/R imbalance (one earcup seated much quieter than the other). This pure helper maps the
// two per-channel peaks (already in dBFS, NOT clamped at 0 so a clipped float reads positive) + the debounced
// sweep-active flag to the exact status TEXT and a colour-free SEVERITY. The caller maps the severity to a
// Theme colour role; the helper never names a colour, so it is unit-testable without the GUI/JUCE LookAndFeel.
//
// HIG: the meaning is carried by the WORDS, never colour alone — "CLIPPING" + the explicit "+1.6 dBFS" reads
// fully without any colour, and the dB number is ALWAYS shown. The decay/peak-hold (so the digits don't
// jitter at 30 Hz) and the sweep-active debounce live in the GUI caller; this helper is a pure function of
// the (held) values it is handed.

enum class LiveInputSeverity {
    Normal,   // in-range live readout         -> Theme::text()
    Warn,     // low / imbalance hint          -> Theme::warn()
    Clip      // at/over full scale            -> Theme::warn() (the strongest warn/error role)
};

struct LiveInputStatus {
    juce::String      text;                              // empty => defer to the idle per-ear verdicts
    LiveInputSeverity severity = LiveInputSeverity::Normal;
};

// Tunables (kept here so the helper + its tests share one source of truth).
inline constexpr float kLiveImbalanceGapDb   = 6.0f;     // an L/R gap this large (dB) hints a reseat
inline constexpr float kLiveImbalanceQuietDb = -15.0f;   // ...but only if the quiet ear is below this level

// peakLDb / peakRDb: the (peak-held) per-channel input level in dBFS, NOT clamped at 0 (a clip reads positive).
// sweepActive: the debounced "the sweep is sounding now" flag (held ~0.3 s above the room floor by the caller).
// Returns empty text when NOT sweep-active so the caller keeps the existing idle per-ear verdict lines.
[[nodiscard]] inline LiveInputStatus liveInputStatus (float peakLDb, float peakRDb, bool sweepActive) {
    LiveInputStatus s;
    if (! sweepActive) return s;   // idle: defer to the per-ear verdicts (do NOT regress them)

    const float peakMax = juce::jmax (peakLDb, peakRDb);

    // CLIPPING: at or over full scale on either ear. Name the clipping ear + show the dB (the words carry it).
    if (peakMax >= 0.0f) {
        const bool   lHot   = peakLDb >= peakRDb;        // the hotter ear is the one that clipped
        const float  hotDb  = lHot ? peakLDb : peakRDb;
        const juce::String ear = lHot ? "L" : "R";
        // "CLIPPING  L +1.6 dBFS - lower the output"
        s.text     = "CLIPPING  " + ear + " "
                   + (hotDb >= 0.0f ? juce::String ("+") : juce::String())
                   + juce::String (hotDb, 1) + " dBFS - lower the output";
        s.severity = LiveInputSeverity::Clip;
        return s;
    }

    // In-progress live readout: both channels, rounded to 0.1 dB. "Sweep in progress - in  L -2.1  R -8.4 dBFS".
    auto fmt = [] (float db) {
        return (db >= 0.0f ? juce::String ("+") : juce::String()) + juce::String (db, 1);
    };
    s.text     = "Sweep in progress - in  L " + fmt (peakLDb) + "  R " + fmt (peakRDb) + " dBFS";
    s.severity = LiveInputSeverity::Normal;

    // L/R imbalance hint: one ear is >6 dB below the other AND that quiet ear is itself below ~-15 dBFS — a
    // seating imbalance the user can fix by reseating the quiet earcup. Append it on a second line; the WARN
    // severity carries the colour but, again, the word "low" + the named ear carry the meaning without colour.
    const float gap = std::abs (peakLDb - peakRDb);
    if (gap > kLiveImbalanceGapDb) {
        const bool  rQuiet = peakRDb < peakLDb;          // which ear is the quiet one
        const float quietDb = rQuiet ? peakRDb : peakLDb;
        if (quietDb < kLiveImbalanceQuietDb) {
            const juce::String ear = rQuiet ? "R" : "L";
            s.text    += "\n(" + ear + " low - reseat the earcup?)";
            s.severity = LiveInputSeverity::Warn;
        }
    }
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
