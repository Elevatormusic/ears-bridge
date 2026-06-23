#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

namespace eb {

// Measurement-grade Kaiser-windowed-sinc POLYPHASE resampler KERNEL EVALUATOR. Owns the prototype
// table + the per-output MAC; the CALLER (ClockBridge) owns the fractional read pointer, so all
// stream continuity lives in one place. Header-only, pure, RT-safe after prepare().
//
// Two decoupled design problems: (A) the Kaiser beta + length L set the stopband/passband; (B) the
// phase count P + linear inter-phase interpolation set the fractional-delay image floor. Cutoff scales
// to fc = min(1, render/capture) so downsampling is anti-aliased. Symmetric prototype => exact linear
// phase => constant group delay (the property that fixes the Lagrange cross-sweep phase imprecision).
// Design + parameters: docs/research/2026-06-23-measurement-grade-asrc.md.
class PolyphaseResampler {
public:
    static constexpr int    kPhases   = 512;     // P  (~108 dB fractional-delay image floor)
    static constexpr int    kUnityLen = 96;      // L at fc=1.0 (even + multiple of 4, ~96 dB margin)
    static constexpr int    kMaxLen   = 384;     // L at fc=0.25 (192->48), the worst case
    static constexpr double kBeta     = 9.62;    // Kaiser beta designed for ~96 dB stopband (6 dB margin)

    PolyphaseResampler() { table_.reserve ((size_t) (kPhases + 1) * kMaxLen); }   // reserve worst case ONCE

    // Design the prototype for one rate pair (SETUP THREAD ONLY; allocates only on first build / grow).
    void prepare (double captureRate, double renderRate) {
        fc_ = juce::jmin (1.0, renderRate / juce::jmax (1.0, captureRate));
        int L = (int) std::lround ((double) kUnityLen / juce::jmax (0.25, fc_));
        L = ((L + 3) / 4) * 4;                                  // round up to a multiple of 4
        L_ = juce::jlimit (kUnityLen, kMaxLen, L);
        buildTable();
    }

    int    length()     const noexcept { return L_; }
    int    halfLength() const noexcept { return L_ / 2; }
    int    phases()     const noexcept { return kPhases; }
    double cutoff()     const noexcept { return fc_; }
    double groupDelay() const noexcept { return (L_ - 1) / 2.0; }       // constant, frequency-independent
    const float* row (int p) const noexcept { return table_.data() + (size_t) p * L_; }   // p in [0, P]

private:
    static double i0 (double x) noexcept {                     // modified Bessel I0 via the series sum
        double t = 1.0, sum = 1.0;                             // term_0 = 1
        for (int k = 1; k < 100; ++k) {
            t *= (0.5 * x) / k;                                // term_k = (x/2)^k / k!
            const double add = t * t;                          // I0 = sum of term_k^2
            sum += add;
            if (add < 1e-17 * sum) break;
        }
        return sum;
    }
    static double sinc (double x) noexcept {
        constexpr double pi = juce::MathConstants<double>::pi;
        return std::abs (x) < 1e-12 ? 1.0 : std::sin (pi * x) / (pi * x);
    }

    void buildTable() {
        const int    L = L_, P = kPhases, Ltot = L * P;
        const double cIdx = (Ltot - 1) / 2.0;                  // prototype center (in prototype samples)
        const double i0b  = i0 (kBeta);
        table_.assign ((size_t) (P + 1) * L, 0.0f);

        // Rows 0..P-1: row(p)[k] = proto[k*P + p], proto[i] = fc * sinc(fc*arg) * kaiser(i), arg in INPUT
        // samples = (i - center)/P. Each row is normalized to sum 1.0 (unity DC gain, no per-mu ripple).
        for (int p = 0; p < P; ++p) {
            float* c = table_.data() + (size_t) p * L;
            double sum = 0.0;
            for (int k = 0; k < L; ++k) {
                const int    i   = k * P + p;
                const double arg = (i - cIdx) / P;
                const double r   = (2.0 * i / (Ltot - 1)) - 1.0;                 // in [-1, 1]
                const double w   = i0 (kBeta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / i0b;
                const double h   = fc_ * sinc (fc_ * arg) * w;
                c[k] = (float) h;
                sum += h;
            }
            const float g = (float) (1.0 / juce::jmax (1e-12, sum));            // per-row DC normalization
            for (int k = 0; k < L; ++k) c[k] *= g;
        }

        // Guard row P: c[P][k] = c[0][k-1] (phase-0 coefficients advanced one input tap; c[P][0] = 0).
        // Lets the linear blend between row and row+1 stay in-range with NO special case at row = P-1.
        const float* g0 = table_.data();                       // row 0 (already normalized)
        float*       gP = table_.data() + (size_t) P * L;
        gP[0] = 0.0f;
        for (int k = 1; k < L; ++k) gP[k] = g0[k - 1];
    }

    std::vector<float> table_;     // (P+1) * L_ contiguous floats; rebuilt on prepare()
    int    L_  = kUnityLen;
    double fc_ = 1.0;
};

} // namespace eb
