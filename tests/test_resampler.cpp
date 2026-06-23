#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/PolyphaseResampler.h"
#include <cmath>
#include <vector>

using Catch::Approx;
using Catch::Matchers::WithinAbs;

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
