#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <random>
#include <cmath>

#include "gui/IrQualityStatus.h"

// Task 2 (Reference-Based Measurement Monitor): the IR-quality verdict — IR-SNR +
// harmonic distortion (THD), as GUIDANCE (never invalidating). The verdict GRADES the
// recovered impulse response, but ONLY when the match-gate already said matched==true:
// an unmatched reference is a GATE failure (re-learn), NOT a quality verdict. Thresholds
// are PROVISIONAL (on-device ratification). CLAUDE.md negative-test rule: the clean IR
// must NOT warn; a noisy IR and a distorted IR must.
//
// IR layout (per src/audio/Deconvolver.cpp): the linear (main) impulse sits near index 0,
// and the Farina k-th harmonic lands at a NEGATIVE-time offset before it — which in the
// circular IR wraps to (mainPeakIndex - offset + n) % n. The caller (which knows the
// sweep params) passes the harmonic offsets; the tests plant energy there.

using eb::evaluateIr;
using eb::irQualityNote;
using eb::IrQuality;
using eb::kMinIrSnrDb;
using eb::kMaxThdPct;

// A clean compact IR: one sharp main impulse + a tiny tail, no harmonics.
static std::vector<float> cleanIr (int n, int peakIdx = 64) {
    std::vector<float> ir ((size_t) n, 0.0f);
    ir[(size_t) peakIdx] = 1.0f;            // sharp main impulse
    ir[(size_t) peakIdx + 1] = 0.05f;       // a tiny lobe so the peak window has a bit of width
    ir[(size_t) peakIdx - 1] = 0.05f;
    // a negligible tail
    std::mt19937 rng (1u);
    std::uniform_real_distribution<float> d (-1e-4f, 1e-4f);
    for (int i = 0; i < n; ++i)
        if (std::abs (i - peakIdx) > 2) ir[(size_t) i] += d (rng);
    return ir;
}

// The harmonic offsets we plant at / exclude from the tail (offsets BEFORE the main peak).
static const std::vector<int> kHarm = { 900, 1400, 1800 };   // k=2,3,4 spots (synthetic)

// A SPREAD headphone-like IR: energy decaying over ~5 ms (~240 samples @48k) from the peak, plus a
// quiet noise floor. The OLD +/-2-sample metric read this NEGATIVE (the spread response counted as
// "noise"); the redefinition reads it solidly positive. THIS is the headphone bug fix.
static std::vector<float> spreadHeadphoneIr (int n, int peakIdx = 64) {
    std::vector<float> ir ((size_t) n, 0.0f);
    for (int k = 0; k < 240 && peakIdx + k < n; ++k)              // ~5 ms decaying, oscillating (colored)
        ir[(size_t) (peakIdx + k)] = std::exp (-(float) k / 60.0f) * ((k & 1) ? -1.0f : 1.0f);
    std::mt19937 rng (3u);                                        // a quiet noise floor everywhere
    std::uniform_real_distribution<float> d (-1.0e-3f, 1.0e-3f);
    for (int i = 0; i < n; ++i) ir[(size_t) i] += d (rng);
    return ir;
}

TEST_CASE("evaluateIr: a clean compact IR grades GOOD (high IR-SNR, ~0 THD, no warn)") {
    const int n = 1 << 13;                  // 8192
    auto ir = cleanIr (n);
    auto q = evaluateIr (ir.data(), n, /*matched*/ true, kMinIrSnrDb, kMaxThdPct, kHarm);
    CHECK (q.matched);
    CHECK (q.irSnrDb > 30.0f);              // a sharp impulse over a negligible tail
    CHECK (q.thdPercent == Catch::Approx (0.0f).margin (0.5f));
    CHECK_FALSE (q.lowQuality);
    CHECK (irQualityNote (q).isEmpty());    // a clean grade leaves the label blank
}

TEST_CASE("evaluateIr: a NOISY IR grades low (low IR-SNR -> warn)") {
    const int n = 1 << 13;
    const int peakIdx = 64;
    std::vector<float> ir ((size_t) n, 0.0f);
    ir[(size_t) peakIdx] = 1.0f;
    // a LARGE random tail away from the peak (drags the IR-SNR below threshold)
    std::mt19937 rng (5u);
    std::uniform_real_distribution<float> d (-0.5f, 0.5f);
    for (int i = 0; i < n; ++i)
        if (std::abs (i - peakIdx) > 2) ir[(size_t) i] = d (rng);

    auto q = evaluateIr (ir.data(), n, true, kMinIrSnrDb, kMaxThdPct, kHarm);
    CHECK (q.matched);
    CHECK (q.irSnrDb < kMinIrSnrDb);        // buried in the tail
    CHECK (q.lowQuality);
    const auto note = irQualityNote (q);
    CHECK_FALSE (note.isEmpty());
    CHECK (note.containsIgnoreCase ("quality"));
    CHECK (note.contains (juce::String (juce::roundToInt (q.irSnrDb))));  // names the dB
}

TEST_CASE("evaluateIr: a DISTORTED IR grades low (planted harmonics -> high THD)") {
    const int n = 1 << 13;
    const int peakIdx = 2000;               // far enough that peakIdx - offset stays in range
    std::vector<float> ir ((size_t) n, 0.0f);
    ir[(size_t) peakIdx] = 1.0f;            // the main (linear) impulse
    // plant strong energy at the harmonic spots (offsets before the main peak)
    for (int off : kHarm)
        ir[(size_t) (peakIdx - off)] = 0.3f;   // ~52% THD if summed in quadrature

    auto q = evaluateIr (ir.data(), n, true, kMinIrSnrDb, kMaxThdPct, kHarm);
    CHECK (q.matched);
    CHECK (q.thdPercent > kMaxThdPct);      // the harmonics dominate
    CHECK (q.lowQuality);
    const auto note = irQualityNote (q);
    CHECK_FALSE (note.isEmpty());
    CHECK (note.containsIgnoreCase ("distortion"));
}

TEST_CASE("evaluateIr: matched==FALSE is NOT graded -> re-learn note, no quality number") {
    const int n = 1 << 13;
    auto ir = cleanIr (n);                  // even a pristine IR
    auto q = evaluateIr (ir.data(), n, /*matched*/ false, kMinIrSnrDb, kMaxThdPct, kHarm);
    CHECK_FALSE (q.matched);
    CHECK_FALSE (q.lowQuality);             // we do NOT grade a non-match (the gate governs)
    const auto note = irQualityNote (q);
    CHECK_FALSE (note.isEmpty());
    CHECK (note.containsIgnoreCase ("re-learn"));        // the distinct gate-failure message
    CHECK_FALSE (note.containsIgnoreCase ("quality"));   // NOT a quality verdict
}

TEST_CASE("evaluateIr boundary: IR-SNR exactly at the threshold is NOT low (strict <)") {
    const int n = 1 << 13;
    auto ir = spreadHeadphoneIr (n);                 // any clean IR with a known positive IR-SNR
    const float s = evaluateIr (ir.data(), n, true, kMinIrSnrDb, kMaxThdPct, kHarm, 48000.0).irSnrDb;
    // lowQuality is irSnrDb < min (STRICT): at min == s it is NOT low; just above s it IS.
    CHECK_FALSE (evaluateIr (ir.data(), n, true, /*min*/ s,        kMaxThdPct, kHarm, 48000.0).lowQuality);
    CHECK       (evaluateIr (ir.data(), n, true, /*min*/ s + 1.0f, kMaxThdPct, kHarm, 48000.0).lowQuality);
}

TEST_CASE("kMinIrSnrDb / kMaxThdPct are the documented provisional v1 thresholds") {
    CHECK (kMinIrSnrDb == Catch::Approx (6.0f));    // provisional (was 20 - the old +/-2-sample metric); ratify on-device
    CHECK (kMaxThdPct  == Catch::Approx (5.0f));     // provisional, on-device ratification pending
}

TEST_CASE("evaluateIr: a SPREAD headphone IR grades GOOD (the headphone bug fix)") {
    const int n = 1 << 13;
    auto ir = spreadHeadphoneIr (n);
    auto q = evaluateIr (ir.data(), n, /*matched*/ true, kMinIrSnrDb, kMaxThdPct, kHarm, /*rate*/ 48000.0);
    CHECK (q.matched);
    CHECK (q.irSnrDb > kMinIrSnrDb);    // clean spread IR -> clearly positive (the OLD metric read NEGATIVE)
    CHECK_FALSE (q.lowQuality);
}

TEST_CASE("evaluateIr: per-sample normalization - lengthening the noise window doesn't move IR-SNR") {
    // Same signal + same noise DENSITY in two IRs of different length: IR-SNR must match (mean power,
    // not summed energy). The OLD summed-tail metric drifted ~3 dB per length doubling.
    auto make = [] (int n) {
        std::vector<float> ir ((size_t) n, 0.0f);
        ir[(size_t) 64] = 1.0f; ir[(size_t) 65] = 0.5f; ir[(size_t) 66] = 0.25f;   // a small spread signal
        std::mt19937 rng (7u);
        std::uniform_real_distribution<float> d (-1.0e-3f, 1.0e-3f);
        for (int i = 0; i < n; ++i) ir[(size_t) i] += d (rng);
        return ir;
    };
    auto a = make (1 << 12), b = make (1 << 13);    // 4096 vs 8192 - twice the noise window, same density
    auto qa = evaluateIr (a.data(), (int) a.size(), true, kMinIrSnrDb, kMaxThdPct, {}, 48000.0);
    auto qb = evaluateIr (b.data(), (int) b.size(), true, kMinIrSnrDb, kMaxThdPct, {}, 48000.0);
    CHECK (qb.irSnrDb == Catch::Approx (qa.irSnrDb).margin (1.5f));   // length-invariant
}
