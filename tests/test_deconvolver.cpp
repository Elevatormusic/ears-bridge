// Task 1 (Reference-Based Measurement Monitor): the deconvolver + the
// `referenceMatches` match-gate — the linchpin. Pure DSP, fully synthetic.
//
// Harness: an exponential sine sweep (Farina ESS) + a linear convolution helper.
// Tests: recover a known IR, cross-correlation alignment, MATCH (positive),
// MISMATCH (the headline negative — a wrong sweep / white noise must NOT pass),
// and noisy-but-matching (the gate asks "right sweep?", not "is it clean?").

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>

#include "audio/Deconvolver.h"

using Catch::Matchers::WithinAbs;

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Synthetic harness
// ---------------------------------------------------------------------------

// Exponential (log) sine sweep, Farina form. The instantaneous phase is
//   phi(t) = (w1*T / ln(w2/w1)) * (exp((t/T)*ln(w2/w1)) - 1)
// with w1 = 2*pi*f1, w2 = 2*pi*f2, T = n/fs. A short raised-cosine fade in/out
// removes the edge clicks that would otherwise smear the deconvolution.
static std::vector<float> makeEss (int n, double fs, double f1 = 20.0, double f2 = 20000.0) {
    std::vector<float> x ((size_t) n, 0.0f);
    const double T  = (double) n / fs;
    const double w1 = 2.0 * kPi * f1;
    const double w2 = 2.0 * kPi * f2;
    const double K  = std::log (w2 / w1);          // ln(w2/w1)
    const double A  = w1 * T / K;
    for (int i = 0; i < n; ++i) {
        const double t   = (double) i / fs;
        const double phi = A * (std::exp ((t / T) * K) - 1.0);
        x[(size_t) i] = (float) std::sin (phi);
    }
    // raised-cosine fade in/out (~2 ms)
    const int fade = std::min (n / 4, (int) std::lround (0.002 * fs));
    for (int i = 0; i < fade; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPi * (float) i / (float) fade));
        x[(size_t) i]              *= w;
        x[(size_t) (n - 1 - i)]    *= w;
    }
    return x;
}

// Direct linear convolution. Output length = sig + ir - 1.
static std::vector<float> convolve (const std::vector<float>& sig, const std::vector<float>& ir) {
    const int ns = (int) sig.size(), ni = (int) ir.size();
    std::vector<float> y ((size_t) (ns + ni - 1), 0.0f);
    for (int i = 0; i < ns; ++i) {
        const float s = sig[(size_t) i];
        if (s == 0.0f) continue;
        for (int k = 0; k < ni; ++k)
            y[(size_t) (i + k)] += s * ir[(size_t) k];
    }
    return y;
}

// Build a sparse IR of length n with planted taps {index -> value}.
static std::vector<float> sparseIr (int n, const std::vector<std::pair<int,float>>& taps) {
    std::vector<float> ir ((size_t) n, 0.0f);
    for (auto& t : taps) ir[(size_t) t.first] = t.second;
    return ir;
}

// Deterministic white noise in [-amp, amp].
static std::vector<float> whiteNoise (int n, float amp, unsigned seed) {
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-amp, amp);
    std::vector<float> x ((size_t) n, 0.0f);
    for (auto& v : x) v = dist (rng);
    return x;
}

static float rmsOf (const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += (double) v * v;
    return (float) std::sqrt (s / std::max<size_t> (1, x.size()));
}

// Add white noise scaled to a target SNR (dB) relative to the signal RMS.
static std::vector<float> addNoiseAtSnr (std::vector<float> sig, float snrDb, unsigned seed) {
    const float sigRms   = rmsOf (sig);
    const float noiseRms  = sigRms / std::pow (10.0f, snrDb / 20.0f);
    auto noise = whiteNoise ((int) sig.size(), 1.0f, seed);
    const float nrms = rmsOf (noise);
    const float scale = (nrms > 0.0f) ? (noiseRms / nrms) : 0.0f;
    for (size_t i = 0; i < sig.size(); ++i) sig[i] += scale * noise[i];
    return sig;
}

// ---------------------------------------------------------------------------
// Test 1 — recover a known sparse IR
// ---------------------------------------------------------------------------
TEST_CASE("deconvolve recovers a known sparse IR (peaks, signs, ratios)") {
    const int n = 1 << 14;                 // 16384
    const double fs = 48000.0;
    auto ess = makeEss (n, fs);

    auto ir = sparseIr (n, { {50, 1.0f}, {120, 0.4f}, {200, -0.2f} });
    auto resp = convolve (ess, ir);        // length n+n-1
    resp.resize ((size_t) n);              // keep the first n (covers all taps)

    auto rec = eb::deconvolve (ess.data(), resp.data(), n);
    REQUIRE ((int) rec.size() >= n);

    // The recovered IR should peak at 50 / 120 / 200 with the planted signs.
    auto peakNear = [&] (int center, int rad) {
        int best = center; float bestMag = 0.0f;
        for (int i = std::max (0, center - rad); i <= center + rad && i < (int) rec.size(); ++i)
            if (std::abs (rec[(size_t) i]) > bestMag) { bestMag = std::abs (rec[(size_t) i]); best = i; }
        return std::make_pair (best, rec[(size_t) best]);
    };
    auto [i50,  v50]  = peakNear (50,  4);
    auto [i120, v120] = peakNear (120, 4);
    auto [i200, v200] = peakNear (200, 4);

    CHECK (std::abs (i50  - 50)  <= 2);
    CHECK (std::abs (i120 - 120) <= 2);
    CHECK (std::abs (i200 - 200) <= 2);

    // signs
    CHECK (v50  > 0.0f);
    CHECK (v120 > 0.0f);
    CHECK (v200 < 0.0f);

    // approximate amplitude ratios (regularization smears, so allow ~30%)
    CHECK_THAT (v120 / v50,        WithinAbs ( 0.4f, 0.15f));
    CHECK_THAT (v200 / v50,        WithinAbs (-0.2f, 0.10f));
}

// ---------------------------------------------------------------------------
// Test 2 — cross-correlation alignment
// ---------------------------------------------------------------------------
TEST_CASE("crossCorrelateAlign finds a 137-sample bulk delay with high coherence") {
    const int n = 1 << 13;                 // 8192
    const double fs = 48000.0;
    auto ess = makeEss (n, fs);

    const int delay = 137;
    std::vector<float> resp ((size_t) n, 0.0f);
    for (int i = 0; i + delay < n; ++i) resp[(size_t) (i + delay)] = ess[(size_t) i];

    auto a = eb::crossCorrelateAlign (ess.data(), n, resp.data(), n);
    CHECK (std::abs (a.delaySamples - delay) <= 1);
    CHECK (a.coherence > 0.8f);
}

// ---------------------------------------------------------------------------
// Test 3 — MATCH (positive, load-bearing)
// ---------------------------------------------------------------------------
TEST_CASE("referenceMatches PASSES a matching response with small noise") {
    const int n = 1 << 14;
    const double fs = 48000.0;
    auto ess = makeEss (n, fs);
    auto ir  = sparseIr (n, { {64, 1.0f}, {130, 0.3f} });

    auto resp = convolve (ess, ir);
    resp.resize ((size_t) n);
    resp = addNoiseAtSnr (resp, 40.0f, 1u);     // small noise

    auto v = eb::referenceMatches (ess.data(), resp.data(), n);
    CHECK (v.matched);
    CHECK (v.coherence >= eb::kMatchCoherenceMin);
    CHECK (v.mainLobeConcentration >= eb::kMainLobeMin);
}

// ---------------------------------------------------------------------------
// Test 4 — MISMATCH (the headline negative)
// ---------------------------------------------------------------------------
TEST_CASE("referenceMatches REJECTS a different sweep (wrong reference)") {
    const int n = 1 << 14;
    const double fs = 48000.0;
    auto essA = makeEss (n, fs, 20.0,  20000.0);   // the reference we *think* we have
    auto essB = makeEss (n, fs, 40.0,  16000.0);   // a genuinely different sweep

    auto ir   = sparseIr (n, { {64, 1.0f}, {130, 0.3f} });
    auto resp = convolve (essB, ir);               // response actually used essB
    resp.resize ((size_t) n);

    // Deconvolving essB's response against essA must NOT look like a clean IR.
    auto v = eb::referenceMatches (essA.data(), resp.data(), n);
    INFO ("mismatch coherence=" << v.coherence << " mainLobe=" << v.mainLobeConcentration);
    CHECK_FALSE (v.matched);
}

TEST_CASE("referenceMatches REJECTS white noise (no sweep at all)") {
    const int n = 1 << 14;
    const double fs = 48000.0;
    auto ess   = makeEss (n, fs);
    auto noise = whiteNoise (n, 1.0f, 7u);

    auto v = eb::referenceMatches (ess.data(), noise.data(), n);
    INFO ("noise coherence=" << v.coherence << " mainLobe=" << v.mainLobeConcentration);
    CHECK_FALSE (v.matched);
}

// ---------------------------------------------------------------------------
// Test 5 — noisy-but-matching still passes (separates MISMATCH from NOISE)
// ---------------------------------------------------------------------------
TEST_CASE("referenceMatches still PASSES a matching sweep buried in heavy noise (SNR ~6 dB)") {
    const int n = 1 << 14;
    const double fs = 48000.0;
    auto ess = makeEss (n, fs);
    auto ir  = sparseIr (n, { {72, 1.0f}, {150, 0.3f} });

    auto resp = convolve (ess, ir);
    resp.resize ((size_t) n);
    resp = addNoiseAtSnr (resp, 6.0f, 11u);        // heavy noise

    auto v = eb::referenceMatches (ess.data(), resp.data(), n);
    INFO ("noisy-match coherence=" << v.coherence << " mainLobe=" << v.mainLobeConcentration);
    CHECK (v.matched);     // it IS the right sweep, just dirty — Task 2 grades the dirt
}
