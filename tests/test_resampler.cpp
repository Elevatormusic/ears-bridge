#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/PolyphaseResampler.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include <algorithm>

using Catch::Approx;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kPi = juce::MathConstants<double>::pi;

// Resample `in` at a FIXED ratio (input-samples-per-output) through our resampler, primed like
// ClockBridge (readPhase starts at L/2-1). Stops before reading past the input.
std::vector<float> resampleStream (eb::PolyphaseResampler& r, const std::vector<float>& in,
                                   double ratio, int numOut) {
    std::vector<float> out;
    out.reserve ((size_t) numOut);
    double phase = r.halfLength() - 1;
    for (int j = 0; j < numOut; ++j) {
        if ((int) std::floor (phase) + r.halfLength() >= (int) in.size()) break;
        out.push_back (r.sampleAt (in.data(), phase));
        phase += ratio;
    }
    return out;
}

std::vector<float> makeSine (int n, double f, double fs) {
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) std::sin (2.0 * kPi * f * i / fs);
    return v;
}

// dB of a (single-tone) signal's amplitude relative to a unit sine, via RMS over the steady region
// (sqrt(2)*RMS = amplitude). Grid-phase robust, no bin alignment needed.
double rmsAmplitudeDb (const std::vector<float>& y, int skip) {
    double s = 0.0; int n = 0;
    for (int i = skip; i < (int) y.size() - skip; ++i) { s += (double) y[(size_t) i] * y[(size_t) i]; ++n; }
    const double rms = std::sqrt (s / std::max (1, n));
    return 20.0 * std::log10 (std::max (1e-12, rms * std::sqrt (2.0)));
}
} // namespace

// ---- Task 4: passband flatness (48->48 drift) + Lagrange baseline-fail ---------------------------

TEST_CASE ("resampler passband flat within 0.10 dB 20Hz-20kHz (48->48 drift)", "[resampler][passband]") {
    const double fs = 48000.0, ratio = 10000.0 / 10001.0;     // near-unity drift exercises all 512 phases
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    const double tones[] = { 20, 50, 100, 500, 1000, 4000, 8000, 12000, 16000, 19000, 20000 };
    const int N = 1 << 18;                                     // long enough that 20 Hz RMS is precise
    double maxDev = 0.0;
    for (double f : tones) {
        auto in  = makeSine (N, f, fs);
        auto out = resampleStream (r, in, ratio, N - r.length() - 4);
        maxDev = std::max (maxDev, std::abs (rmsAmplitudeDb (out, 2048)));
    }
    INFO ("max passband deviation dB = " << maxDev);
    CHECK (maxDev <= 0.10);                                    // SPEC gate
}

TEST_CASE ("BASELINE: juce::LagrangeInterpolator FAILS the passband gate", "[resampler][baseline]") {
    const double fs = 48000.0, ratio = 10000.0 / 10001.0;
    const double tones[] = { 8000, 12000, 16000, 19000, 20000 };
    const int N = 1 << 18;
    double maxDev = 0.0;
    for (double f : tones) {
        auto in = makeSine (N, f, fs);
        juce::LagrangeInterpolator lag;
        std::vector<float> out ((size_t) (N - 8));
        lag.process (ratio, in.data(), out.data(), (int) out.size());
        maxDev = std::max (maxDev, std::abs (rmsAmplitudeDb (out, 2048)));
    }
    INFO ("Lagrange max passband deviation dB = " << maxDev);
    CHECK (maxDev > 0.10);                                     // proves the gate has teeth
}

// ---- Task 1: prepare() + the (P+1)xL Kaiser-windowed-sinc prototype table -------------------------

TEST_CASE ("PolyphaseResampler: geometry per rate pair", "[resampler][table]") {
    eb::PolyphaseResampler r;
    r.prepare (48000.0, 48000.0);                 // unity
    CHECK (r.phases()     == 512);
    CHECK (r.length()     == 96);                 // L_unity (even, x4)
    CHECK (r.halfLength() == 48);
    CHECK (r.cutoff()     == Approx (1.0));
    CHECK (r.groupDelay() == Approx (47.5));      // (L-1)/2

    r.prepare (96000.0, 48000.0);                 // 2:1 downsample
    CHECK (r.cutoff() == Approx (0.5));
    CHECK (r.length() == 192);                    // round_to_x4(96/0.5)
    CHECK (r.length() % 4 == 0);

    r.prepare (192000.0, 48000.0);                // 4:1 worst case
    CHECK (r.cutoff() == Approx (0.25));
    CHECK (r.length() == 384);                    // kMaxLen
}

TEST_CASE ("PolyphaseResampler: each of the P rows sums to 1.0 (DC gain, normalized)", "[resampler][table]") {
    eb::PolyphaseResampler r;
    for (double cap : { 48000.0, 88200.0, 96000.0, 192000.0 }) {
        r.prepare (cap, 48000.0);
        const int L = r.length();
        for (int p = 0; p < r.phases(); ++p) {          // rows 0..P-1
            double s = 0.0;
            const float* c = r.row (p);
            for (int k = 0; k < L; ++k) s += c[k];
            CHECK_THAT (s, WithinAbs (1.0, 1e-5));      // risk #5: no per-mu amplitude modulation
        }
    }
}

TEST_CASE ("PolyphaseResampler: guard row + finite taps + centered phase-0 peak", "[resampler][table]") {
    eb::PolyphaseResampler r;
    r.prepare (48000.0, 48000.0);
    const int L = r.length(), P = r.phases();
    const float* g0 = r.row (0);
    const float* gP = r.row (P);                         // the guard row

    // Guard row: c[P][k] = c[0][k-1], c[P][0] = 0 -> makes the row+1 blend valid with no special case.
    CHECK (gP[0] == 0.0f);
    for (int k = 1; k < L; ++k) CHECK_THAT ((double) gP[k], WithinAbs ((double) g0[k - 1], 1e-9));

    // Every tap of every row (incl. guard) is finite.
    for (int p = 0; p <= P; ++p) {
        const float* c = r.row (p);
        for (int k = 0; k < L; ++k) REQUIRE (std::isfinite (c[k]));
    }

    // Phase-0 (on-grid) row peaks at the center tap -> linear-phase sinc centered.
    int peak = 0;
    for (int k = 1; k < L; ++k) if (std::abs (g0[k]) > std::abs (g0[peak])) peak = k;
    CHECK (peak >= L / 2 - 1);
    CHECK (peak <= L / 2);
}

// ---- Task 2: sampleAt() — per-output MAC + linear inter-phase blend ------------------------------

TEST_CASE ("PolyphaseResampler::sampleAt: DC transparency (rows sum to 1)", "[resampler][mac]") {
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    std::vector<float> in (4096, 0.7f);
    const int base = 1000;
    for (double frac : { 0.0, 0.123, 0.5, 0.777, 0.999 })
        CHECK_THAT ((double) r.sampleAt (in.data(), base + frac), WithinAbs (0.7, 1e-4));
}

TEST_CASE ("PolyphaseResampler::sampleAt: preserves a band-limited sine's amplitude", "[resampler][mac]") {
    // A broken MAC (wrong taps / centering) cannot reconstruct a full-amplitude in-band sine. The
    // rigorous linear-phase / constant-group-delay check is the FFT test in Task 5.
    constexpr double pi = juce::MathConstants<double>::pi;
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    const int N = 8192;
    std::vector<float> in (N);
    for (int i = 0; i < N; ++i) in[i] = (float) std::sin (2.0 * pi * 1000.0 * i / 48000.0);
    double mn = 1e9, mx = -1e9, phase = r.halfLength() - 1;
    const double ratio = 0.9997;                          // near-unity drift
    for (int j = 0; j < N - r.length() - 4; ++j) {
        const double y = r.sampleAt (in.data(), phase);
        if (phase > 1000.0 && phase < N - 200.0) { mn = std::min (mn, y); mx = std::max (mx, y); }
        phase += ratio;
    }
    CHECK_THAT (mx, WithinAbs ( 1.0, 5e-3));              // unity passband gain at 1 kHz, correct interpolation
    CHECK_THAT (mn, WithinAbs (-1.0, 5e-3));
}

TEST_CASE ("PolyphaseResampler::sampleAt: continuous across the phase wrap (guard row)", "[resampler][mac]") {
    constexpr double pi = juce::MathConstants<double>::pi;
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    std::vector<float> in (4096);
    for (int i = 0; i < 4096; ++i) in[i] = (float) std::sin (2.0 * pi * 1000.0 * i / 48000.0);
    const int base = 2000;
    const double a = r.sampleAt (in.data(), base + 0.999999);   // row ~ P-1, mu -> 1
    const double b = r.sampleAt (in.data(), base + 1.000000);   // n+1, mu = 0  -> must be continuous
    CHECK_THAT (a, WithinAbs (b, 5e-4));
}
