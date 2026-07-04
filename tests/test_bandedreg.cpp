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

// ---------------------------------------------------------------------------
// Integration: the banded default inside eb::deconvolve (Task 2).
// ---------------------------------------------------------------------------
#include <random>

static constexpr double kPiB = 3.14159265358979323846;

// Farina ESS (mirrors tests/test_deconvolver.cpp's makeEss - kept local so this file stands alone).
static std::vector<float> makeEssB (int n, double fs, double f1 = 20.0, double f2 = 20000.0) {
    std::vector<float> x ((size_t) n, 0.0f);
    const double T  = (double) n / fs;
    const double w1 = 2.0 * kPiB * f1, w2 = 2.0 * kPiB * f2;
    const double K  = std::log (w2 / w1);
    const double A  = w1 * T / K;
    for (int i = 0; i < n; ++i) {
        const double t = (double) i / fs;
        x[(size_t) i] = (float) std::sin (A * (std::exp ((t / T) * K) - 1.0));
    }
    const int fade = std::min (n / 4, (int) std::lround (0.002 * fs));
    for (int i = 0; i < fade; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPiB * (float) i / (float) fade));
        x[(size_t) i] *= w;
        x[(size_t) (n - 1 - i)] *= w;
    }
    return x;
}

static std::vector<float> convolveB (const std::vector<float>& sig, const std::vector<float>& ir) {
    std::vector<float> y (sig.size() + ir.size() - 1, 0.0f);
    for (size_t i = 0; i < sig.size(); ++i) {
        const float s = sig[i];
        if (s == 0.0f) continue;
        for (size_t k = 0; k < ir.size(); ++k) y[i + k] += s * ir[k];
    }
    return y;
}

static void addNoiseB (std::vector<float>& x, float amp, unsigned seed) {
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-amp, amp);
    for (auto& v : x) v += dist (rng);
}

static double rmsOver (const std::vector<float>& x, int from, int to) {
    double s = 0.0; int c = 0;
    for (int i = from; i < to && i < (int) x.size(); ++i) { s += (double) x[(size_t) i] * x[(size_t) i]; ++c; }
    return c > 0 ? std::sqrt (s / c) : 0.0;
}

TEST_CASE("deconvolve banded default matches the legacy inverse on a clean in-band measurement") {
    const int n = 12000;                             // NOT a power of two: fftSize 16384, no wrap
    auto ess = makeEssB (n, 48000.0);
    std::vector<float> ir ((size_t) n, 0.0f);
    ir[50] = 1.0f; ir[120] = 0.4f; ir[200] = -0.2f;
    auto resp = convolveB (ess, ir); resp.resize ((size_t) n);

    auto banded = eb::deconvolve (ess.data(), resp.data(), n);                 // new default
    auto legacy = eb::deconvolve (ess.data(), resp.data(), n, 1.0e-3f);        // escape hatch
    REQUIRE (banded.size() == legacy.size());
    // Same linear IR, honestly compared: raw tap AMPLITUDES legitimately differ a little (the
    // flat 1e-3 kept reconstructing from the fade-skirt residue ABOVE f2 - exactly the F1 noise
    // zone - so its "delta" integrates ~an extra sixth of bandwidth; the banded eps truncates at
    // the derived band edge). Tap POSITIONS and tap RATIOS normalize that bandwidth factor out.
    auto peakNear = [] (const std::vector<float>& v, int c) {
        int best = c; float m = 0.0f;
        for (int i = c - 4; i <= c + 4; ++i)
            if (std::abs (v[(size_t) i]) > m) { m = std::abs (v[(size_t) i]); best = i; }
        return best;
    };
    CHECK (std::abs (peakNear (banded, 50)  - peakNear (legacy, 50))  <= 1);
    CHECK (std::abs (peakNear (banded, 120) - peakNear (legacy, 120)) <= 1);
    const float rB = banded[120] / banded[50], rL = legacy[120] / legacy[50];
    const float sB = banded[200] / banded[50], sL = legacy[200] / legacy[50];
    INFO ("ratios banded=" << rB << "/" << sB << " legacy=" << rL << "/" << sL
          << "  tap50 banded=" << banded[50] << " legacy=" << legacy[50]);
    CHECK (std::abs (rB - rL) < 0.10f * std::abs (rL));
    CHECK (std::abs (sB - sL) < 0.10f * std::abs (sL));
    // The absolute tap survives within the bandwidth-truncation factor (never collapses).
    CHECK (banded[50] > 0.70f * legacy[50]);
    CHECK (banded[50] < 1.05f * legacy[50]);
}

TEST_CASE("deconvolve banded default SUPPRESSES the negative-time noise the flat eps boosted") {
    // Production-like sweep LENGTH matters here: the banded residue in the negative-lag zone is
    // the honest IN-band noise floor, whose level relative to the legacy out-of-band boost falls
    // with in-band energy per bin (i.e. with sweep duration). At n=12000 (0.25 s) the measured
    // ratio was 0.235 - the mechanism working, but short-sweep geometry; at 1.5 s (below) the
    // separation is production-representative. The response uses explicit sparse taps so the
    // build stays O(n).
    const int n = 72000;                             // 1.5 s at 48k; fftSize 131072, no wrap
    auto ess = makeEssB (n, 48000.0);
    std::vector<float> resp ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) {
        const float s = ess[(size_t) i];
        if (i + 50  < n) resp[(size_t) (i + 50)]  += s;
        if (i + 120 < n) resp[(size_t) (i + 120)] += 0.4f * s;
    }
    addNoiseB (resp, 5.0e-4f, 42u);                  // ~-66 dBFS floor: the SIM F1 dose

    auto banded = eb::deconvolve (ess.data(), resp.data(), n);
    auto legacy = eb::deconvolve (ess.data(), resp.data(), n, 1.0e-3f);
    const int fftSize = (int) banded.size();         // 131072
    // The negative-lag zone (circular top of the buffer) is where flat-reg-boosted out-of-band
    // noise landed (the F1 mechanism). Farina offsets for this length reach ~1.2*off4 ~ 17k back.
    const double negBanded = rmsOver (banded, fftSize - 15000, fftSize - 1000);
    const double negLegacy = rmsOver (legacy, fftSize - 15000, fftSize - 1000);
    INFO ("neg-lag RMS banded=" << negBanded << " legacy=" << negLegacy
          << "  ratio=" << (negLegacy > 0.0 ? negBanded / negLegacy : -1.0));
    // >=14 dB quieter is the load-bearing separation (the rig's F1 dose-response implies more;
    // the residue here is the IN-band noise floor, which banding deliberately leaves honest).
    CHECK (negBanded < 0.2 * negLegacy);
    // And the linear IR is still there.
    CHECK (std::abs (banded[50]) > 0.5f);
}

TEST_CASE("deconvolve banded default is deterministic and falls back flat on a degenerate ref") {
    const int n = 12000;
    auto ess = makeEssB (n, 48000.0);
    std::vector<float> resp = ess;                   // any same-length response
    auto a = eb::deconvolve (ess.data(), resp.data(), n);
    auto b = eb::deconvolve (ess.data(), resp.data(), n);
    REQUIRE (a.size() == b.size());
    CHECK (std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);

    // Degenerate reference (all zeros): the banded derivation refuses -> the sentinel path must
    // fall back to EXACTLY the legacy flat behavior (never worse than today).
    std::vector<float> zeroRef ((size_t) n, 0.0f);
    auto viaSentinel = eb::deconvolve (zeroRef.data(), resp.data(), n);
    auto viaLegacy   = eb::deconvolve (zeroRef.data(), resp.data(), n, 1.0e-3f);
    REQUIRE (viaSentinel.size() == viaLegacy.size());
    CHECK (std::memcmp (viaSentinel.data(), viaLegacy.data(),
                        viaSentinel.size() * sizeof (float)) == 0);
}
