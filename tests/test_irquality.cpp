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

TEST_CASE("evaluateIr boundary: IR-SNR exactly at kMinIrSnrDb is NOT low (>= is fine)") {
    const int n = 1 << 13;
    const int peakIdx = 64;
    std::vector<float> ir ((size_t) n, 0.0f);
    // Construct so peakWindowEnergy / tailEnergy = 10^(20/10) = 100 exactly.
    // Window = [peakIdx-1 .. peakIdx+1]; put all peak energy in the centre tap.
    // tailEnergy spread over (n - tailExcluded) samples; pick a single tail tap to make it exact.
    ir[(size_t) peakIdx] = 10.0f;           // peak energy = 100
    ir[(size_t) (peakIdx + 500)] = 1.0f;    // tail energy = 1 -> ratio 100 -> 20 dB exactly
    auto q = evaluateIr (ir.data(), n, true, kMinIrSnrDb, kMaxThdPct, kHarm);
    CHECK (q.irSnrDb == Catch::Approx (kMinIrSnrDb).margin (0.05f));
    CHECK_FALSE (q.lowQuality);             // strictly-less-than threshold => boundary is acceptable
    CHECK (irQualityNote (q).isEmpty());
}

TEST_CASE("kMinIrSnrDb / kMaxThdPct are the documented provisional v1 thresholds") {
    CHECK (kMinIrSnrDb == Catch::Approx (20.0f));   // provisional, on-device ratification pending
    CHECK (kMaxThdPct  == Catch::Approx (5.0f));     // provisional, on-device ratification pending
}
