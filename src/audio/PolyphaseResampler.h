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

    // Produce one output sample at fractional input position readPhase (in capture samples). RT-safe,
    // no alloc. CALLER guarantees input[floor(readPhase) - L/2 + 1 .. + L/2] are valid (L/2-1 left
    // history, L/2 right). row in [0, P-1]; the guard row P keeps row+1 in-range with no special case.
    float sampleAt (const float* input, double readPhase) const noexcept {
        const int    n    = (int) std::floor (readPhase);
        const double ph   = (readPhase - n) * kPhases;             // [0, P)
        const int    row  = (int) ph;                              // [0, P-1]
        const float  mu   = (float) (ph - row);
        const float* a    = table_.data() + (size_t) row       * L_;
        const float* b    = table_.data() + (size_t) (row + 1) * L_;   // guard row makes row+1 valid at row=P-1
        const float* x    = input + (n - L_ / 2 + 1);
        float sa = 0.0f, sb = 0.0f;                                // dual MAC: rows `row` and `row+1`
        for (int k = 0; k < L_; ++k) { sa += a[k] * x[k]; sb += b[k] * x[k]; }
        return sa + mu * (sb - sa);                                // linear inter-phase blend
    }

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
        const int    L = L_, P = kPhases;
        const double i0b  = i0 (kBeta);
        const double half = L / 2.0;                           // Kaiser window half-width (taps)
        table_.assign ((size_t) (P + 1) * L, 0.0f);

        // Build P+1 rows. Row p is the windowed-sinc fractional-delay filter for frac = p/P: it reads the
        // input at continuous position n + frac, i.e. coeff[k] multiplies input[n - L/2 + 1 + k] with
        // coeff[k] = fc * sinc(fc * (frac + L/2 - 1 - k)) * kaiser(k centered on the sinc). The sinc center
        // sits at tap kc = (L/2 - 1) + frac, so the read lands exactly at n + frac (NOT n + 0.999 - frac).
        // Row P (frac = 1.0) is the natural guard row: it reads input[n+1], continuous with row 0 of n+1
        // (so the row/row+1 blend never needs a special case at the wrap). Each row normalized to sum 1.0.
        for (int p = 0; p <= P; ++p) {
            float* c = table_.data() + (size_t) p * L;
            const double frac = (double) p / P;                // p/P in [0, 1]
            const double kc   = (L / 2 - 1) + frac;            // sinc center tap (= where arg == 0)
            double sum = 0.0;
            for (int k = 0; k < L; ++k) {
                const double arg = frac + (L / 2 - 1) - k;     // kc - k  (input-sample offset, +frac forward)
                const double wp  = (k - kc) / half;            // normalized window position ~ [-1, 1]
                const double w   = i0 (kBeta * std::sqrt (juce::jmax (0.0, 1.0 - wp * wp))) / i0b;
                const double h   = fc_ * sinc (fc_ * arg) * w;
                c[k] = (float) h;
                sum += h;
            }
            const float g = (float) (1.0 / juce::jmax (1e-12, sum));   // per-row DC normalization (risk #5)
            for (int k = 0; k < L; ++k) c[k] *= g;
        }
    }

    std::vector<float> table_;     // (P+1) * L_ contiguous floats; rebuilt on prepare()
    int    L_  = kUnityLen;
    double fc_ = 1.0;
};

} // namespace eb
