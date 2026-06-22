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

} // namespace eb
