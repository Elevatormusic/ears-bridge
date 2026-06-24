#pragma once
#include <algorithm>
#include <cmath>

// Pure, header-only decisions behind the noise-floor primitive. No platform/JUCE deps so the unit
// suite drives every branch (tests/test_noisefloor.cpp). RT-safe: plain arithmetic + a bounded sort.
//
// On-device ratification TODO: kQuietCeilingLin, kFloorBlendAlpha, the median percentile, and the
// sustain window (NoiseFloorTracker::kFloorSustainSec) are synthetic-tuned defaults — ratify on the rig.
namespace eb {

// "Quiet" = below a loose ABSOLUTE ceiling no real sweep block sits under. Reuses -24 dBFS
// (kGoodLevelLinear's value): a healthy sweep peaks above it; the gap / pre-sweep silence sit well
// below it. Keyed off an absolute ceiling, not a recent peak, so the FIRST (pre-sweep) window needs
// no prior loud reference. On-device-tunable.
constexpr float kQuietCeilingLin = 0.0631f;   // ~-24 dBFS
constexpr float kFloorBlendAlpha = 0.3f;      // per-fold EMA rate toward a new quiet-window estimate (DOWN)
constexpr float kFloorRiseFactor = 0.1f;      // UP moves use alpha*this -> floor resists ratcheting upward

// True iff `level` is finite and strictly below the ceiling. Non-finite is never quiet.
[[nodiscard]] inline bool isQuietBlock (float level, float ceiling) noexcept {
    return std::isfinite (level) && level < ceiling;
}

// Median of a quiet window's per-block levels. Median (not mean) rejects a transient bump a cough /
// creak would add (Prawda, Schlecht & Valimaki, "rule of two", JASA 2022). Sorts in place; returns 0
// for an empty/null window. n is small + bounded by the tracker's window cap, so the sort is cheap.
[[nodiscard]] inline float robustLowFloor (float* vals, int n) noexcept {
    if (vals == nullptr || n <= 0) return 0.0f;
    std::sort (vals, vals + n);
    return vals[n / 2];
}

// ASYMMETRIC EMA toward a new quiet-window estimate. An uninitialized floor (<=0) ADOPTS the candidate
// so the first baseline is exact. Tracks DOWN at `alpha` but UP at only `alpha*kFloorRiseFactor`, so a
// transiently-noisier (but still sub-ceiling) gap can't ratchet the floor upward and inflate the SNR
// denominator (the spec's "amp hiss is stable; resist upward spikes"); a genuinely-and-persistently
// higher floor still converges, just slowly.
[[nodiscard]] inline float blendFloor (float current, float candidate, float alpha) noexcept {
    if (current <= 0.0f) return candidate;
    const float a = (candidate < current) ? alpha : alpha * kFloorRiseFactor;   // fast down, slow up
    return current + a * (candidate - current);
}

// Single user-facing number: the POWER mean (quadratic mean) of the two channel floors -> dB. Power-
// correct for combining two levels; averaging dB values directly would be level-wrong. A tiny floor
// guards log(0) when both channels are silent.
[[nodiscard]] inline float averageFloorDb (float linL, float linR) noexcept {
    const float tiny = 1.0e-9f;
    const float p = 0.5f * (linL * linL + linR * linR);
    return 20.0f * std::log10 (std::sqrt (std::max (p, tiny)));
}

// RMS of a sample span [first, last), SKIPPING non-finite samples (a single NaN/Inf must not poison the
// estimate). Returns 0 for an empty / all-non-finite span. Pure; the offline SweepSchedule learner uses it.
[[nodiscard]] inline double rmsLinearOver (const float* x, int first, int last) noexcept {
    if (x == nullptr || last <= first) return 0.0;
    double s = 0.0; int cnt = 0;
    for (int i = first; i < last; ++i) {
        const double v = (double) x[i];
        if (std::isfinite (v)) { s += v * v; ++cnt; }
    }
    return cnt > 0 ? std::sqrt (s / (double) cnt) : 0.0;
}

// Linear amplitude -> dBFS, floored at floorDb for a (near-)zero input (no -inf / NaN).
[[nodiscard]] inline double linToDb (double lin, double floorDb = -240.0) noexcept {
    return lin > 1.0e-12 ? 20.0 * std::log10 (lin) : floorDb;
}

} // namespace eb
