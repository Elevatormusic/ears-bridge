// Sub-project 2 (frequency-dependent deconvolution regularization): the pure banded-eps
// derivation. Research-validated Kirkeby-Nelson scheme (spec 2026-07-02): fractional-octave
// smoothing -> tilt-whitening (x k, cancels the ESS 1/f power tilt) -> 12 dB median-relative
// band classification (longest contiguous run) -> eps_in = A*1e-6 / eps_out = A*10 with
// half-octave raised-cosine log-eps transitions. Everything in bin-index space (f is
// proportional to k, so octaves are bin RATIOS and no sample rate is needed).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>

#include "audio/Deconvolver.h"

// Analytic one-sided ESS-like power spectrum: P(k) = A0*kLo/k inside [kLo,kHi] (the 1/f power
// tilt, P[kLo] = A0 is the in-band maximum), a floor everywhere else. The hard cliff at the
// edges is fine: the derivation's own fractional-octave smoothing softens it, and the edge
// tolerance below absorbs the smear.
static std::vector<float> essPower (int numBins, int kLo, int kHi, float A0 = 1.0f,
                                    float floorRel = 1e-12f) {
    std::vector<float> P ((size_t) numBins, A0 * floorRel);
    for (int k = kLo; k <= kHi && k < numBins; ++k)
        P[(size_t) k] = A0 * (float) kLo / (float) k;
    return P;
}

TEST_CASE("banded eps derives the band of a wideband ESS-like spectrum") {
    const int numBins = 16385;                       // fftSize 32768, one-sided
    const int kLo = 110, kHi = 13650;                // ~20 Hz..20 kHz at 48k/32768 (bin space)
    auto br = eb::deriveBandedRegularization (essPower (numBins, kLo, kHi).data(), numBins);
    REQUIRE (br.valid);
    REQUIRE ((int) br.epsilon.size() == numBins);
    INFO ("binLo=" << br.binLo << " binHi=" << br.binHi);
    // Edges within 1/3 octave of the truth BOTH ways: the raw-spectrum edge refinement pins the
    // top edge even though the smoothing smear + whitening would otherwise drag it to the rail
    // (the first build measured exactly that: binHi reached numBins-2 before refinement).
    CHECK (br.binLo >= (int) (kLo / 1.26)); CHECK (br.binLo <= (int) (kLo * 1.26));
    CHECK (br.binHi >= (int) (kHi / 1.26)); CHECK (br.binHi <= (int) (kHi * 1.26));
}

TEST_CASE("banded eps self-derives a NARROW band from a narrowband reference") {
    const int numBins = 16385;
    const int kLo = 1100, kHi = 4400;                // a 2-octave sweep (the S9 wrong-ref shape)
    auto br = eb::deriveBandedRegularization (essPower (numBins, kLo, kHi).data(), numBins);
    REQUIRE (br.valid);
    INFO ("binLo=" << br.binLo << " binHi=" << br.binHi);
    CHECK (br.binLo >= (int) (kLo / 1.26)); CHECK (br.binLo <= (int) (kLo * 1.26));
    CHECK (br.binHi >= (int) (kHi / 1.26)); CHECK (br.binHi <= (int) (kHi * 1.26));
}

TEST_CASE("banded eps levels: pure inverse in-band, firm attenuation out-of-band, monotone ramp") {
    const int numBins = 16385;
    const int kLo = 110, kHi = 13650;
    const float A0 = 0.37f;                          // arbitrary non-unit anchor scale
    auto br = eb::deriveBandedRegularization (essPower (numBins, kLo, kHi, A0).data(), numBins);
    REQUIRE (br.valid);
    const int mid = (br.binLo + br.binHi) / 2;
    // In-band: eps ~ A*1e-6 (A = the smoothed in-band max ~ A0; allow x10 either way for the
    // smoothing's dent at the edges).
    CHECK (br.epsilon[(size_t) mid] > A0 * 1e-7f);
    CHECK (br.epsilon[(size_t) mid] < A0 * 1e-5f);
    // Far out-of-band LOW side: full eps_out ~ A*10 (bin 8 sits well below the ramp foot).
    CHECK (br.epsilon[(size_t) 8] > A0 * 0.5f);
    // HIGH side: kHi is only ~1/4 octave below the rail, so numBins-2 sits MID-RAMP by design
    // (the half-octave crossfade extends past the spectrum end). The load-bearing property is
    // that the rail-adjacent bin is firmly OUT of the pure-inverse regime - orders of magnitude
    // above eps_in - not that it reached the full eps_out plateau.
    CHECK (br.epsilon[(size_t) numBins - 2] > br.epsilon[(size_t) mid] * 100.0f);
    // The low-side ramp is monotone non-increasing walking INTO the band.
    const int a = std::max (2, (int) (br.binLo / 1.41));      // inside the half-octave ramp
    const int b = std::max (a + 1, (int) (br.binLo / 1.10));  // deeper into the ramp
    CHECK (br.epsilon[(size_t) a] >= br.epsilon[(size_t) b]);
    CHECK (br.epsilon[(size_t) b] >= br.epsilon[(size_t) (br.binLo + 8)]);
}

TEST_CASE("banded eps is scale-invariant: scaling the power scales eps, band unchanged") {
    const int numBins = 8193;
    const int kLo = 60, kHi = 6800;
    auto p1 = essPower (numBins, kLo, kHi, 1.0f);
    auto p2 = p1; for (auto& v : p2) v *= 100.0f;    // reference 10x louder -> power 100x
    auto a = eb::deriveBandedRegularization (p1.data(), numBins);
    auto b = eb::deriveBandedRegularization (p2.data(), numBins);
    REQUIRE (a.valid); REQUIRE (b.valid);
    CHECK (a.binLo == b.binLo);
    CHECK (a.binHi == b.binHi);
    for (int k = 1; k < numBins - 1; k += 97) {      // stride: keep the loop cheap
        const float ratio = b.epsilon[(size_t) k] / a.epsilon[(size_t) k];
        CHECK (ratio > 99.0f); CHECK (ratio < 101.0f);
    }
}

TEST_CASE("banded eps refuses degenerate spectra (guards)") {
    // All-zero power: no anchor -> invalid.
    std::vector<float> zeros ((size_t) 4096, 0.0f);
    CHECK_FALSE (eb::deriveBandedRegularization (zeros.data(), 4096).valid);
    // Too few bins.
    std::vector<float> tiny ((size_t) 8, 1.0f);
    CHECK_FALSE (eb::deriveBandedRegularization (tiny.data(), 8).valid);
    // Null.
    CHECK_FALSE (eb::deriveBandedRegularization (nullptr, 4096).valid);
    // A single narrow LF spike (width 3 at bin 100): the smoothed run stays under the 64-bin
    // minimum -> invalid (an impulse is not a sweep).
    std::vector<float> spike ((size_t) 8192, 1e-12f);
    spike[99] = spike[100] = spike[101] = 1.0f;
    CHECK_FALSE (eb::deriveBandedRegularization (spike.data(), 8192).valid);
}

TEST_CASE("banded eps treats DC and Nyquist as out-of-band") {
    const int numBins = 8193;
    auto br = eb::deriveBandedRegularization (essPower (numBins, 60, 6800).data(), numBins);
    REQUIRE (br.valid);
    // Both rails sit at (or within the ramp toward) eps_out - never the in-band tiny eps.
    CHECK (br.epsilon[0] > br.epsilon[(size_t) ((br.binLo + br.binHi) / 2)] * 100.0f);
    CHECK (br.epsilon[(size_t) numBins - 1] > br.epsilon[(size_t) ((br.binLo + br.binHi) / 2)] * 100.0f);
}
