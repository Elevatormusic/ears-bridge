#pragma once
#include <juce_core/juce_core.h>   // juce::String / jmax / roundToInt
#include <cmath>                    // std::log10 / std::sqrt / std::abs
#include <vector>

namespace eb {

// IR-quality verdict (pure, header-only, no GUI deps — mirrors gui/SnrStatus.h and
// gui/RawRailStatus.h). GRADES the recovered impulse response from deconvolve() with two
// RELATIVE metrics — IR signal-to-noise and harmonic distortion — but it is GUIDANCE ONLY:
// it never invalidates a capture (that masking is Task 4's job), it only shows the numbers.
//
// Honest gating: it grades quality ONLY when the match-gate (Task 1's referenceMatches)
// already said matched==true. If matched==false, the GATE — not quality — governs: the
// reference doesn't match this sweep, so there is nothing to grade and the note says
// "re-learn", NOT a quality number. (Grading a non-match would be dishonest.)
//
//   IR-SNR — the main-impulse peak energy (a short window around the max-|sample| index)
//            vs the IR "noise tail" energy (the IR away from the main peak AND away from
//            the harmonic-distortion spots, so distortion can't masquerade as noise):
//            irSnrDb = 10*log10(peakWindowEnergy / tailEnergy).
//   THD   — for an exponential-sine-sweep (Farina) deconvolution, the k-th harmonic impulse
//            lands at a known offset BEFORE the linear (main) impulse: dt_k = T*ln(k)/ln(w2/w1)
//            seconds. In the circular IR those negative-time spots wrap to (peakIdx - dt_k + n)
//            % n. We sum the energy at the first few harmonic spots (k=2,3,4) and express it as
//            a % of the main-impulse energy: thdPercent = 100*sqrt(harmonicEnergy / mainEnergy).
//            The offsets depend on the SWEEP params, so the CALLER (which knows the reference's
//            sweep) passes them precomputed as harmonicOffsetsSamples (offsets before the peak).
//
// Honest v1 limitations (deliberate per the "show now, ratify later" decision):
//  - PROVISIONAL thresholds (kMinIrSnrDb / kMaxThdPct) — need on-device ratification.
//  - RELATIVE IR quality, not an absolute/calibrated dB or a certified THD figure — guidance.
//  - GUIDANCE / non-invalidating — it never flips cleanCapture; it surfaces the numbers only.

// PROVISIONAL — on-device ratification needed.
static constexpr float kMinIrSnrDb = 20.0f;
// PROVISIONAL — on-device ratification needed.
static constexpr float kMaxThdPct  = 5.0f;

struct IrQuality {
    bool  matched    = false;   // mirrors the match-gate; if false we did NOT grade quality
    float irSnrDb    = 0.0f;    // main-peak energy vs the noise-tail energy (10*log10)
    float thdPercent = 0.0f;    // Farina-harmonic energy as % of the main impulse
    bool  lowQuality = false;   // warn: matched AND (irSnrDb < min || thdPercent > max)
};

// Pure verdict. `ir`/`n` = the deconvolved impulse response (main lobe near index 0; see
// Deconvolver.cpp). `matched` = the match-gate result (do NOT grade a non-match).
// harmonicOffsetsSamples = the Farina k=2,3,4 offsets BEFORE the main peak (caller-computed
// from the sweep params; empty => THD not assessed = 0). RT-safe relative to the audio thread
// only in the sense that it is pure + lock-free, but it is OFFLINE DSP (run once per completed
// sweep) — not for per-block use; irQualityNote builds a String and is GUI-side ONLY.
[[nodiscard]] inline IrQuality evaluateIr (const float* ir, int n, bool matched,
                                           float minIrSnrDb = kMinIrSnrDb,
                                           float maxThdPct  = kMaxThdPct,
                                           const std::vector<int>& harmonicOffsetsSamples = {}) {
    IrQuality q;
    q.matched = matched;
    if (ir == nullptr || n <= 0) return q;

    // --- locate the main (linear) impulse: the max-|sample| index --------------
    int   peakIdx = 0;
    float peakMag = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float m = std::abs (ir[i]);
        if (m > peakMag) { peakMag = m; peakIdx = i; }
    }

    // A short window around the main peak holds the "impulse" energy. +/-2 samples
    // (the regularized main lobe is a few taps wide; see the recover-IR test tolerance).
    constexpr int kPeakHalfWidth = 2;
    auto inPeakWindow = [&] (int i) { return std::abs (i - peakIdx) <= kPeakHalfWidth; };

    // The harmonic spots wrap to (peakIdx - off + n) % n in the circular IR. Mark a small
    // window around each so we (a) measure their energy for THD and (b) EXCLUDE them from the
    // noise tail (so distortion does not inflate the "noise" and depress the IR-SNR twice).
    constexpr int kHarmHalfWidth = 2;
    auto inAnyHarmonic = [&] (int i) {
        for (int off : harmonicOffsetsSamples) {
            const int c = ((peakIdx - off) % n + n) % n;   // circular wrap of the negative-time spot
            int d = std::abs (i - c);
            d = std::min (d, n - d);                       // circular distance
            if (d <= kHarmHalfWidth) return true;
        }
        return false;
    };

    // --- energies ---------------------------------------------------------------
    double peakEnergy = 0.0, tailEnergy = 0.0, harmEnergy = 0.0;
    for (int i = 0; i < n; ++i) {
        const double e = (double) ir[i] * ir[i];
        if (inPeakWindow (i))        peakEnergy += e;
        else if (inAnyHarmonic (i))  harmEnergy += e;   // distortion: not peak, not noise
        else                         tailEnergy += e;   // the noise tail (excludes peak + harmonics)
    }

    const double tiny = 1.0e-20;   // floors the ratio so a silent tail can't produce +inf/NaN
    q.irSnrDb    = 10.0f * (float) std::log10 ((peakEnergy + tiny) / (tailEnergy + tiny));
    q.thdPercent = 100.0f * (float) std::sqrt (harmEnergy / (peakEnergy + tiny));

    // lowQuality only when we actually graded (matched). Honest: don't grade a non-match.
    q.lowQuality = matched && (q.irSnrDb < minIrSnrDb || q.thdPercent > maxThdPct);
    return q;
}

// GUI-side note (builds a juce::String — do NOT call on the audio thread). Three branches:
//  - !matched              -> a distinct SHORT re-learn message (the GATE failed; NOT a grade).
//  - matched && lowQuality -> a SHORT warn with the numbers (full detail goes in the tooltip).
//  - else                  -> "" (a clean grade leaves the label blank).
// Wording kept <= ~60 chars for the status line (the #8 cut-off lesson).
[[nodiscard]] inline juce::String irQualityNote (const IrQuality& q) {
    if (! q.matched)
        return "Reference doesn't match your sweep - re-learn it.";
    if (! q.lowQuality)
        return {};
    return "Low measurement quality: " + juce::String (juce::roundToInt (q.irSnrDb))
         + " dB / " + juce::String (juce::roundToInt (q.thdPercent)) + "% distortion.";
}

} // namespace eb
