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
//   IR-SNR — the MEAN POWER of the IR's real span (a few-ms gate around the peak — a headphone IR is
//            SPREAD, not a single tap) vs the MEAN POWER of the clean region (away from the gate AND from
//            the harmonic-distortion spots, so distortion can't masquerade as noise). Per-sample, each
//            window over its OWN count (Huszty & Sakamoto): irSnrDb = 10*log10(signalMean / noiseMean).
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

// PROVISIONAL — on-device ratification needed. Was 20 (the OLD +/-2-sample metric); the mean-power
// IR-SNR of a clean SPREAD headphone IR reads solidly positive, so 6 dB is a conservative clean floor.
static constexpr float kMinIrSnrDb = 6.0f;
// PROVISIONAL — on-device ratification needed.
static constexpr float kMaxThdPct  = 5.0f;
// Signal-gate span (post-peak decay) + pre-margin (onset / short pre-ring), in ms. PROVISIONAL.
static constexpr double kSignalGateMs = 8.0;
static constexpr double kPreMarginMs  = 0.5;

// PROVISIONAL — on-device ratification gate (review fix). FALSE until the IR-SNR/THD cutoffs are
// ratified on real hardware. A clean deconvolved ESS reads only ~13 dB IR-SNR (see test_refmonitor.cpp
// and the report), so the default kMinIrSnrDb=20 would flag a GOOD measurement as low-quality — a false
// "low quality" warn. While this is false, the GUI shows the IR-SNR/THD numbers as INFO (neutral/dim
// tone), NOT a pass/fail warn: a clean measurement never reads as a warning. The match-gate (a mismatch)
// STILL shows the "re-learn" message — that gate is always valid. The thresholds and the lowQuality
// computation stay live in the PURE module (IrQualityStatus / RefMonitor) for the ratification campaign;
// only the GUI presentation is gated here. Flip to true once the cutoffs are ratified on-device.
static constexpr bool kIrThresholdsRatified = false;

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
                                           const std::vector<int>& harmonicOffsetsSamples = {},
                                           double sampleRate = 48000.0) {
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

    // Signal = the IR's real span around the peak. A headphone IR is SPREAD over a few ms (not a single
    // tap), so the signal is a ms-based gate: a small pre-margin (onset / short pre-ring) + a post-peak
    // decay span. ms-based so it scales with rate; circular; clamped to n.
    const int signalSpan = juce::jlimit (1, n, (int) std::lround (kSignalGateMs * sampleRate / 1000.0));
    const int preMargin  = juce::jlimit (0, n, (int) std::lround (kPreMarginMs  * sampleRate / 1000.0));
    auto inSignalGate = [&] (int i) {
        int d = i - peakIdx;                          // shortest circular signed distance
        if (d >  n / 2) d -= n;
        if (d < -n / 2) d += n;
        return d >= -preMargin && d <= signalSpan;
    };

    // The harmonic spots wrap to (peakIdx - off + n) % n in the circular IR. Exclude them from BOTH the
    // signal AND the noise (distortion is neither) so they can't masquerade as noise and depress the SNR.
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

    // --- per-sample MEAN-power energies (signal / noise each divided by its OWN sample count) ---
    double signalEnergy = 0.0, noiseEnergy = 0.0, harmEnergy = 0.0;
    long   signalCount  = 0,   noiseCount  = 0;
    for (int i = 0; i < n; ++i) {
        const double e = (double) ir[i] * ir[i];
        if (inSignalGate (i))       { signalEnergy += e; ++signalCount; }
        else if (inAnyHarmonic (i))   harmEnergy   += e;   // distortion: not signal, not noise
        else                        { noiseEnergy  += e; ++noiseCount; }
    }

    const double tiny = 1.0e-20;
    const double signalMean = signalEnergy / (double) juce::jmax (1L, signalCount);
    const double noiseMean  = (noiseCount > 0) ? noiseEnergy / (double) noiseCount : tiny;
    q.irSnrDb    = 10.0f * (float) std::log10 ((signalMean + tiny) / (noiseMean + tiny));
    q.thdPercent = 100.0f * (float) std::sqrt (harmEnergy / (signalEnergy + tiny));

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
