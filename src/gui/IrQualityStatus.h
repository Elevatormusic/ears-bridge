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
//   THD   — for an exponential-sine-sweep (Farina) deconvolution, distortion products land at
//            NEGATIVE TIME before the linear impulse (dt_k = T*ln(k)/ln(w2/w1) for the k-th
//            harmonic). #18: at a 48 kHz session they are NOT tight spikes at those offsets —
//            the harmonic sweeps run past the reference band (regularization-boosted) and past
//            Nyquist (aliased), smearing into a broad negative-time cluster — so the metric is
//            the MIRROR-COMPENSATED ENERGY of the whole negative-time region bounded by the
//            largest Farina offset: thd% = 100*sqrt(max(0, regionE - mirrorMean*regionN)/signalE).
//            The caller passes harmonicOffsetsSamples computed from the MEASURED sweep length
//            (measuredSweepLength) — they bound the region; empty => THD not assessed (0).
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
    float thdPercent = 0.0f;    // negative-time (distortion-region) energy as % of the main impulse (#18)
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

    // #18: distortion products live at NEGATIVE TIME before the linear impulse (Farina), but at a 48 kHz
    // session they are NOT +-2-sample spikes at the textbook offsets: the harmonic sweeps run past the
    // reference's band (their >f2 content is divided by ~nothing-but-regularization and BOOSTED) and past
    // Nyquist (aliased down-chirps), smearing the distortion energy into a broad negative-time cluster.
    // The old fixed +-2 spot windows therefore read thdPercent ~ 0 on every real measurement (the audit's
    // finding: the spots were also displaced by the reference trim margin). The honest metric is the
    // MIRROR-COMPENSATED ENERGY of the whole negative-time distortion REGION:
    //     region  = d in [-(1.2 * maxHarmonicOffset), -signalSpan)   (before the impulse, past the gate)
    //     mirror  = d in (signalSpan, signalSpan + regionWidth]      (the equidistant positive-time twin)
    //     thd%    = 100 * sqrt( max(0, regionEnergy - mirrorMean * regionCount) / signalEnergy )
    // The subtraction removes everything SYMMETRIC around the impulse (the noise floor AND the slow
    // band-limitation undulation from the reference having no content below f1 / above f2), so a clean
    // capture reads ~0 while spread/misplaced/aliased distortion — negative-time-only — registers. The
    // Farina offsets (from the MEASURED sweep length) only BOUND the region; empty => THD not assessed.
    // The compensation baseline is the MIRROR region — the same width on the POSITIVE-time side, right
    // after the signal gate. Band-limitation artifacts of the deconvolution (the reference has no
    // content below f1 or above f2, so the recovered impulse rides on slow symmetric undulations) and
    // the plain noise floor appear on BOTH sides of the impulse; distortion products appear ONLY at
    // negative time. Subtracting the mirror's per-sample mean removes exactly the symmetric part, so a
    // clean capture reads ~0% while spread/misplaced/aliased distortion registers. (A far-field noise
    // mean under-compensates: the sub-f1 undulation is strongest near the peak.)
    int maxOff = 0;
    for (int off : harmonicOffsetsSamples) maxOff = std::max (maxOff, off);
    const int distFar = std::min ((int) std::lround (1.2 * (double) maxOff),
                                  std::max (0, n / 2 - 1));             // farthest-back distortion bound
    auto circDelta = [&] (int i) {                    // shortest circular signed distance (as the gate)
        int d = i - peakIdx;
        if (d >  n / 2) d -= n;
        if (d < -n / 2) d += n;
        return d;
    };
    // The region starts at -signalSpan (NOT -preMargin) so it is EQUIDISTANT with the mirror: the
    // impulse's symmetric skirt inside +-signalSpan is neither distortion nor baseline (its positive
    // half sits inside the signal gate), and the nearest real image (k=2) is many thousands of samples
    // out. Without this the pre-peak skirt counted as distortion with no counterpart in the mirror.
    const int distWidth = std::max (0, distFar - signalSpan);
    auto inDistortionRegion = [&] (int i) {
        if (maxOff <= 0) return false;
        const int d = circDelta (i);
        return d < -signalSpan && d >= -distFar;
    };
    auto inMirrorRegion = [&] (int i) {               // positive-time counterpart, same width, past the gate
        if (maxOff <= 0) return false;
        const int d = circDelta (i);
        return d > signalSpan && d <= signalSpan + distWidth;
    };

    // --- per-sample MEAN-power energies (signal / noise each divided by its OWN sample count) ---
    double signalEnergy = 0.0, noiseEnergy = 0.0, distEnergy = 0.0, mirrorEnergy = 0.0;
    long   signalCount  = 0,   noiseCount  = 0,   distCount  = 0,   mirrorCount  = 0;
    for (int i = 0; i < n; ++i) {
        const double e = (double) ir[i] * ir[i];
        if (inSignalGate (i))            { signalEnergy += e; ++signalCount; }
        else if (inDistortionRegion (i)) { distEnergy   += e; ++distCount;   }   // neither signal nor noise
        else {
            if (inMirrorRegion (i))      { mirrorEnergy += e; ++mirrorCount; }   // also ordinary noise
            noiseEnergy += e; ++noiseCount;
        }
    }

    const double tiny = 1.0e-20;
    const double signalMean = signalEnergy / (double) juce::jmax (1L, signalCount);
    const double noiseMean  = (noiseCount > 0) ? noiseEnergy / (double) noiseCount : tiny;
    const double baseMean   = (mirrorCount > 0) ? mirrorEnergy / (double) mirrorCount : noiseMean;
    const double distExcess = std::max (0.0, distEnergy - baseMean * (double) distCount);
    q.irSnrDb    = 10.0f * (float) std::log10 ((signalMean + tiny) / (noiseMean + tiny));
    q.thdPercent = 100.0f * (float) std::sqrt (distExcess / (signalEnergy + tiny));

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
