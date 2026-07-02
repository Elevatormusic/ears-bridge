#pragma once
#include <juce_core/juce_core.h>   // juce::String / jmax / jmin / roundToInt
#include <cmath>                    // std::log10

namespace eb {

// Sweep-to-Noise SNR data-quality verdict (pure, header-only, no GUI deps — mirrors gui/ClipStatus.h
// and gui/RawRailStatus.h). Computes, PER EAR, how far the in-sweep peak sat above the frozen pre-sweep
// room-noise floor, takes the MIN of the two ears (the good ear must NOT vouch for the bad one), and
// warns when it is below a PROVISIONAL threshold — but only off a floor we trust (honest silence
// otherwise). evaluateSnr is RT-safe (a few log10 + compares, no alloc/lock) so the engine can run it on
// the capture thread at the SweepActive->Complete edge; snrNote builds a juce::String and is GUI-side
// ONLY (never call it on the audio thread).
//
// Honest v1 limitations (deliberate per the "warn now, ratify later" decision):
//  - PROVISIONAL threshold (kMinSweepSnrDb) — needs on-device ratification.
//  - BROADBAND peak-over-peak — a single noisy band can hide; this does NOT certify per-band SNR.
//  - Conservative — peak-over-peak UNDERSTATES the true SNR (RMS sweep energy is higher), so it warns
//    later than reality, never earlier; it is guidance, not a calibrated dB figure.

// PROVISIONAL - needs on-device ratification; v1 is broadband + peak-over-peak (conservative); does not
// certify per-band SNR.
static constexpr float kMinSweepSnrDb = 20.0f;

struct SnrVerdict {
    bool  lowSnr       = false;   // warn: trusted floor AND min-ear SNR below threshold
    bool  floorTrusted = false;   // the pre-sweep window was quiet/stationary AND a floor was captured
    float snrDbL = 0.0f, snrDbR = 0.0f, snrDbMin = 0.0f;   // per-ear + the min that drives the verdict
};

// Pure verdict, per-ear-floor form (#46): each ear's SNR against ITS OWN noise floor. floorL/floorR =
// frozen pre-sweep noise floors (linear peak, 0 until armed); peakL/peakR = the per-ear max in-sweep peak;
// floorStable = the floor-confidence guard. minDb defaults to the provisional threshold. RT-safe: no
// allocation, no lock, no String. (The single-floor overload below delegates here - it previously graded
// BOTH ears against one floor, so a noisy right earcup was measured against the quiet ear's floor.)
[[nodiscard]] inline SnrVerdict evaluateSnr (float floorL, float floorR, float peakL, float peakR,
                                             bool floorStable, float minDb = kMinSweepSnrDb) {
    SnrVerdict v;
    const float tiny = 1.0e-6f;   // floors the log argument so a 0 peak/floor can't produce -inf/NaN
    v.snrDbL = 20.0f * std::log10 (juce::jmax (peakL, tiny) / juce::jmax (floorL, tiny));
    v.snrDbR = 20.0f * std::log10 (juce::jmax (peakR, tiny) / juce::jmax (floorR, tiny));
    v.snrDbMin = juce::jmin (v.snrDbL, v.snrDbR);   // min: the good ear must NOT vouch for the bad
    v.floorTrusted = floorStable && floorL > 0.0f && floorR > 0.0f;
    v.lowSnr = v.floorTrusted && v.snrDbMin < minDb;   // honest silence when the floor isn't trusted
    return v;
}

[[nodiscard]] inline SnrVerdict evaluateSnr (float armFloor, float peakL, float peakR,
                                             bool floorStable, float minDb = kMinSweepSnrDb) {
    return evaluateSnr (armFloor, armFloor, peakL, peakR, floorStable, minDb);
}

// GUI-side note (builds a juce::String — do NOT call on the audio thread). Empty unless the verdict
// warns; otherwise names the (rounded) min-ear SNR so the user sees how close to the floor the sweep ran.
[[nodiscard]] inline juce::String snrNote (const SnrVerdict& v) {
    if (! v.lowSnr) return {};
    return "Sweep is only " + juce::String (juce::roundToInt (v.snrDbMin))
         + " dB above the room noise - quieten the room or raise the level, then re-measure.";
}

} // namespace eb
