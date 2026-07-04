// Task 4 (Reference-Based Measurement Monitor): the pure glue —
//  (1) Farina harmonic offsets (the offsets evaluateIr needs; T2 left them as a param),
//  (2) the workflow state machine (NotLearned/Learned/ReferenceStale/GradedClean/
//      GradedSuspect/NotGraded) + its transitions — honest by construction (never green
//      over a not-graded / mismatched / suspect state),
//  (3) gradeMeasurement: match-gate FIRST, then quality — verified with a synthetic
//      matching ESS+IR (GradedClean) and a wrong reference (ReferenceStale, no quality).
//
// All pure + synthetic (the live capture/measure loop is on-device).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>

#include "audio/RefMonitor.h"

using eb::farinaHarmonicOffsets;
using eb::nextRefMonState;
using eb::refMonBlocksGreen;
using eb::gradeMeasurement;
using eb::RefMonState;
using eb::RefMonInputs;
using eb::QualityBand;
using eb::RefMonColour;
using eb::classifySweepSnr;
using eb::classifyIrSnr;
using eb::classifyThd;
using eb::refMonColour;
using eb::aggregateVerdict;
using eb::qualityNote;

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Synthetic harness (mirrors tests/test_deconvolver.cpp)
// ---------------------------------------------------------------------------
static std::vector<float> makeEss (int n, double fs, double f1 = 20.0, double f2 = 20000.0) {
    std::vector<float> x ((size_t) n, 0.0f);
    const double T  = (double) n / fs;
    const double w1 = 2.0 * kPi * f1;
    const double w2 = 2.0 * kPi * f2;
    const double K  = std::log (w2 / w1);
    const double A  = w1 * T / K;
    for (int i = 0; i < n; ++i) {
        const double t   = (double) i / fs;
        const double phi = A * (std::exp ((t / T) * K) - 1.0);
        x[(size_t) i] = (float) std::sin (phi);
    }
    const int fade = std::min (n / 4, (int) std::lround (0.002 * fs));
    for (int i = 0; i < fade; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPi * (float) i / (float) fade));
        x[(size_t) i]           *= w;
        x[(size_t) (n - 1 - i)] *= w;
    }
    return x;
}

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

static std::vector<float> whiteNoise (int n, float amp, unsigned seed) {
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-amp, amp);
    std::vector<float> x ((size_t) n, 0.0f);
    for (auto& v : x) v = dist (rng);
    return x;
}

// ===========================================================================
// 1. Farina harmonic offsets — the offset math against hand-computed values
// ===========================================================================
TEST_CASE("farinaHarmonicOffsets: matches the closed-form dt_k = N*ln(k)/ln(f2/f1)") {
    // sampleRate cancels: off_k = round(N * ln(k) / ln(f2/f1)). Hand-compute for N=48000,
    // f1=20, f2=20000 (ln(f2/f1) = ln(1000) = 6.907755...).
    const int    N   = 48000;
    const double lnR = std::log (20000.0 / 20.0);
    const int exp2 = (int) std::lround (N * std::log (2.0) / lnR);   // = 4818
    const int exp3 = (int) std::lround (N * std::log (3.0) / lnR);   // = 7637
    const int exp4 = (int) std::lround (N * std::log (4.0) / lnR);   // = 9635

    auto offs = farinaHarmonicOffsets (N, 48000.0, 20.0, 20000.0);   // k = 2,3,4
    REQUIRE (offs.size() == 3);
    CHECK (offs[0] == exp2);
    CHECK (offs[1] == exp3);
    CHECK (offs[2] == exp4);
    // Sanity against literal hand values (so a sign/formula slip is caught, not just self-consistency):
    // off_k = round(48000*ln(k)/ln(1000)); ln(1000)=6.907755 -> k2 4817, k3 7634, k4 9633 (±1 ULP).
    CHECK (std::abs (offs[0] - 4817) <= 1);
    CHECK (std::abs (offs[1] - 7634) <= 1);
    CHECK (std::abs (offs[2] - 9633) <= 1);
    // Harmonics are ordered and strictly increasing (k=2 closest to the impulse).
    CHECK (offs[0] < offs[1]);
    CHECK (offs[1] < offs[2]);
}

TEST_CASE("farinaHarmonicOffsets: independent of sampleRate (offset in SAMPLES only scales with N)") {
    // Same N, same f1/f2 -> same sample offsets regardless of the rate passed (the rate cancels).
    auto a = farinaHarmonicOffsets (32768, 48000.0, 20.0, 20000.0);
    auto b = farinaHarmonicOffsets (32768, 96000.0, 20.0, 20000.0);
    REQUIRE (a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK (a[i] == b[i]);
}

TEST_CASE("farinaHarmonicOffsets: rejects degenerate params + drops out-of-range harmonics") {
    CHECK (farinaHarmonicOffsets (0,     48000.0).empty());        // no length
    CHECK (farinaHarmonicOffsets (48000, 0.0).empty());            // no rate
    CHECK (farinaHarmonicOffsets (48000, 48000.0, 20.0, 20.0).empty());   // f2 <= f1
    // A SHORT sweep: the 4th-harmonic offset can exceed N and must be dropped.
    auto few = farinaHarmonicOffsets (4000, 48000.0, 20.0, 20000.0);
    for (int o : few) { CHECK (o > 0); CHECK (o < 4000); }
}

// ===========================================================================
// 2. The workflow state machine — the truth table + honesty invariant
// ===========================================================================
TEST_CASE("nextRefMonState: no reference -> NotLearned (never graded)") {
    CHECK (nextRefMonState ({ /*loaded*/ false, false, false, false }) == RefMonState::NotLearned);
    // Even if downstream flags are (spuriously) set, no reference still means NotLearned.
    CHECK (nextRefMonState ({ false, true, true, false }) == RefMonState::NotLearned);
}

TEST_CASE("nextRefMonState: reference loaded but nothing measured -> Learned") {
    CHECK (nextRefMonState ({ true, /*measured*/ false, false, false }) == RefMonState::Learned);
}

TEST_CASE("nextRefMonState: measured + matched + clean -> GradedClean") {
    CHECK (nextRefMonState ({ true, true, /*matched*/ true, /*lowQuality*/ false }) == RefMonState::GradedClean);
}

TEST_CASE("nextRefMonState: measured + matched + low quality -> GradedSuspect") {
    CHECK (nextRefMonState ({ true, true, true, /*lowQuality*/ true }) == RefMonState::GradedSuspect);
}

TEST_CASE("nextRefMonState: measured + NOT matched -> ReferenceStale (re-learn, gate FIRST)") {
    // The gate runs before quality: a non-match is ReferenceStale even if lowQuality were somehow false.
    CHECK (nextRefMonState ({ true, true, /*matched*/ false, false }) == RefMonState::ReferenceStale);
    CHECK (nextRefMonState ({ true, true, /*matched*/ false, true  }) == RefMonState::ReferenceStale);
}

TEST_CASE("refMonBlocksGreen: GradedClean is the ONLY non-green-blocking state (honesty invariant)") {
    CHECK_FALSE (refMonBlocksGreen (RefMonState::GradedClean));   // the only state that allows green
    CHECK (refMonBlocksGreen (RefMonState::NotLearned));
    CHECK (refMonBlocksGreen (RefMonState::Learned));
    CHECK (refMonBlocksGreen (RefMonState::NotGraded));
    CHECK (refMonBlocksGreen (RefMonState::ReferenceStale));
    CHECK (refMonBlocksGreen (RefMonState::GradedSuspect));
}

// ===========================================================================
// 3. gradeMeasurement — match-gate FIRST, then quality
// ===========================================================================
// Build a (reference, response) pair from a clean direct-arrival IR, padding the reference so the
// FULL linear convolution is kept (truncating the convolution tail corrupts the deconvolution).
// Returns the padded reference + response, both length n+irLen-1.
static void makeCleanMeasurement (int sweepLen, double fs,
                                   std::vector<float>& refOut, std::vector<float>& respOut) {
    auto ref = makeEss (sweepLen, fs);
    std::vector<float> roomIr ((size_t) 256, 0.0f);
    roomIr[50] = 1.0f;                          // a single dominant direct arrival = a clean measurement
    respOut = convolve (ref, roomIr);           // length sweepLen + 255
    refOut.assign (respOut.size(), 0.0f);
    std::copy (ref.begin(), ref.end(), refOut.begin());
}

TEST_CASE("gradeMeasurement: a matching ESS + a clean IR -> GradedClean (logic path), valid quality") {
    // The redefined IR-SNR (a few-ms gate + per-sample mean power) reads a clean deconvolved ESS solidly
    // above the default 6 dB cutoff (the headphone-IR fix). The CLEAN logic path is pinned at a clearly-
    // cleared threshold (10 dB); the suspect path below forces the matched->suspect branch with a high
    // cutoff on the SAME clean data.
    const int    n  = 1 << 15;        // 32768
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (n, fs, ref, resp);
    const int m = (int) resp.size();

    auto g = gradeMeasurement (ref.data(), resp.data(), m, fs, 20.0, 20000.0,
                               /*minIrSnrDb*/ 10.0f, /*maxThdPct*/ 5.0f);
    CHECK (g.match.matched);                       // the gate passed (right sweep)
    CHECK (g.state == RefMonState::GradedClean);   // matched + clean (at a ratified-style threshold)
    CHECK_FALSE (g.quality.lowQuality);
    CHECK (g.quality.matched);                     // quality was actually computed
    CHECK (g.quality.irSnrDb > 10.0f);             // a real IR-SNR number is present and clears the cutoff
    CHECK_FALSE (refMonBlocksGreen (g.state));     // green is allowed for this verdict only
}

TEST_CASE("gradeMeasurement: matched but BELOW the IR-SNR cutoff -> GradedSuspect") {
    // The redefined metric means a CLEAN measurement now reads ABOVE the default 6 dB cutoff (the
    // headphone-IR fix), so we force the matched->suspect branch with a deliberately high cutoff that no
    // real measurement clears, instead of relying on a clean measurement falsely reading low.
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (n, fs, ref, resp);
    const int m = (int) resp.size();

    auto g = gradeMeasurement (ref.data(), resp.data(), m, fs, 20.0, 20000.0,
                               /*minIrSnrDb*/ 100.0f, /*maxThdPct*/ 5.0f);   // a cutoff nothing clears
    CHECK (g.match.matched);
    CHECK (g.state == RefMonState::GradedSuspect);
    CHECK (g.quality.lowQuality);
    CHECK (refMonBlocksGreen (g.state));           // suspect must NOT read as green
}

TEST_CASE("gradeMeasurement: a WRONG reference -> ReferenceStale, NO quality number") {
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    // refA is the learned reference; the response is to a DIFFERENT sweep (refB, narrower band)
    // convolved with a room IR — i.e. the user re-learnt against the wrong/stale sweep.
    auto refA = makeEss (n, fs, 20.0,  20000.0);
    auto refB = makeEss (n, fs, 100.0, 8000.0);
    std::vector<float> roomIr ((size_t) 256, 0.0f);
    roomIr[40] = 1.0f; roomIr[90] = 0.25f;
    auto resp = convolve (refB, roomIr);
    resp.resize ((size_t) n);

    auto g = gradeMeasurement (refA.data(), resp.data(), n, fs);
    CHECK_FALSE (g.match.matched);                     // the gate caught the mismatch
    CHECK (g.state == RefMonState::ReferenceStale);    // "re-learn", NOT a quality verdict
    CHECK_FALSE (g.quality.matched);                   // we did NOT grade a non-match
    CHECK_FALSE (g.quality.lowQuality);
    CHECK (g.quality.irSnrDb == Catch::Approx (0.0f)); // no quality number was produced
}

TEST_CASE("gradeMeasurement: white-noise response -> ReferenceStale (not a low-quality grade)") {
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    auto ref  = makeEss (n, fs);
    auto resp = whiteNoise (n, 0.5f, 99u);             // no sweep at all
    auto g = gradeMeasurement (ref.data(), resp.data(), n, fs);
    CHECK_FALSE (g.match.matched);
    CHECK (g.state == RefMonState::ReferenceStale);    // a non-match is a GATE failure, never "low quality"
    CHECK_FALSE (g.quality.lowQuality);
}

TEST_CASE("gradeMeasurement: null/empty inputs -> ReferenceStale (never green on no data)") {
    auto g = gradeMeasurement (nullptr, nullptr, 0, 48000.0);
    CHECK (g.state == RefMonState::ReferenceStale);
    CHECK (refMonBlocksGreen (g.state));
}

// ============================================================================
// 3-color measurement-quality grade (2026-06-22): sweepSNR gates, THD escalates
// at red, IR-SNR advisory. Fixtures = the on-device values.
// ============================================================================

TEST_CASE ("QualityBand classifiers map on-device values", "[refmon][bands]") {
    // sweepSNR: green >=25, red <18, orange [18,25). Clean 22-36, noisy 5-18.
    CHECK (classifySweepSnr (5.0f,  true)  == QualityBand::Red);
    CHECK (classifySweepSnr (16.7f, true)  == QualityBand::Red);
    CHECK (classifySweepSnr (18.0f, true)  == QualityBand::Orange);   // not < 18
    CHECK (classifySweepSnr (22.0f, true)  == QualityBand::Orange);
    CHECK (classifySweepSnr (25.0f, true)  == QualityBand::Green);    // >= 25
    CHECK (classifySweepSnr (36.0f, true)  == QualityBand::Green);
    CHECK (classifySweepSnr (5.0f,  false) == QualityBand::Unknown);  // invalid -> never penalize
    // IR-SNR: green >=50, red <35, orange [35,50). Clean 54-66.
    CHECK (classifyIrSnr (66.0f) == QualityBand::Green);
    CHECK (classifyIrSnr (50.0f) == QualityBand::Green);
    CHECK (classifyIrSnr (40.0f) == QualityBand::Orange);
    CHECK (classifyIrSnr (35.0f) == QualityBand::Orange);             // not < 35
    CHECK (classifyIrSnr (30.0f) == QualityBand::Red);
    // THD: green <=3, red >10, orange (3,10].
    CHECK (classifyThd (0.01f) == QualityBand::Green);
    CHECK (classifyThd (3.0f)  == QualityBand::Green);                // <= 3
    CHECK (classifyThd (6.0f)  == QualityBand::Orange);
    CHECK (classifyThd (10.0f) == QualityBand::Orange);              // not > 10
    CHECK (classifyThd (12.0f) == QualityBand::Red);
}

TEST_CASE ("GradedMarginal + refMonColour mapping", "[refmon][colour]") {
    CHECK (refMonColour (RefMonState::GradedClean)    == RefMonColour::Green);
    CHECK (refMonColour (RefMonState::GradedMarginal) == RefMonColour::Orange);
    CHECK (refMonColour (RefMonState::GradedSuspect)  == RefMonColour::Red);
    CHECK (refMonColour (RefMonState::ReferenceStale) == RefMonColour::Neutral);
    CHECK (refMonColour (RefMonState::NotLearned)     == RefMonColour::Neutral);
    CHECK (refMonBlocksGreen (RefMonState::GradedMarginal));   // orange blocks green too
    CHECK (refMonBlocksGreen (RefMonState::GradedSuspect));
    CHECK_FALSE (refMonBlocksGreen (RefMonState::GradedClean));
}

TEST_CASE ("aggregateVerdict: sweepSNR gates, THD escalates at red, IR-SNR never gates", "[refmon][verdict]") {
    auto V = [] (bool m, float snr, bool valid, float ir, float thd) {
        return aggregateVerdict (m, snr, valid, ir, thd);
    };
    CHECK (V (false, 30, true, 60, 0).state == RefMonState::ReferenceStale);   // gate first
    CHECK (V (false, 30, true, 60, 0).sweepSnrBand == QualityBand::Unknown);
    CHECK (V (true, 28, true, 60, 0.01f).state == RefMonState::GradedClean);    // all green
    CHECK (V (true, 22, true, 60, 0.01f).state == RefMonState::GradedMarginal); // snr orange
    CHECK (V (true, 5,  true, 56, 0.01f).state == RefMonState::GradedSuspect);  // snr red
    CHECK (V (true, 30, true, 60, 12).state    == RefMonState::GradedSuspect);  // THD red escalates
    CHECK (V (true, 30, true, 60, 6).state     == RefMonState::GradedClean);    // THD orange does NOT
    CHECK (V (true, 30, true, 30, 0.01f).state == RefMonState::GradedClean);    // IR-SNR red never gates
    CHECK (V (true, 30, true, 30, 0.01f).irSnrBand == QualityBand::Red);        // but its band shows red
    CHECK (V (true, 5, false, 60, 0.01f).state == RefMonState::GradedMarginal); // invalid snr -> Marginal, NEVER Clean
    CHECK (V (true, 5, false, 60, 0.01f).sweepSnrBand == QualityBand::Unknown);  // (integrity: an unverifiable trust gate can't grant green - review 2026-06-23)
}

TEST_CASE ("aggregateVerdict never-falsely-green: an UNVERIFIABLE sweepSNR falls to Marginal", "[refmon][verdict]") {
    // The trust-gate-abstains false-green: a matched-but-noisy capture whose window has no usable noise region
    // (sweepSnrValid=false) must NOT read Clean - IR-SNR is advisory and THD is ~0 for non-harmonic/tonal noise.
    for (float ir : { 30.0f, 60.0f })            // any IR-SNR (advisory) ...
        for (float thd : { 0.01f, 6.0f }) {      // ... and any sub-red THD ...
            auto v = aggregateVerdict (/*matched*/ true, /*snr*/ 0.0f, /*sweepSnrValid*/ false, ir, thd);
            CHECK (v.state != RefMonState::GradedClean);              // ... can never be green when SNR is unverifiable
            CHECK (v.state == RefMonState::GradedMarginal);
        }
    // THD-red still escalates past Marginal even when SNR is unverifiable.
    CHECK (aggregateVerdict (true, 0.0f, false, 60, 12.0f).state == RefMonState::GradedSuspect);
}

TEST_CASE ("aggregateVerdict honesty invariant: never GradedClean when sweepSNR or THD is red", "[refmon][verdict]") {
    for (float snr : { 5.0f, 16.7f, 18.0f, 22.0f, 28.0f, 36.0f })
        for (float thd : { 0.01f, 6.0f, 12.0f })
            for (float ir : { 30.0f, 60.0f }) {
                auto v = aggregateVerdict (true, snr, true, ir, thd);
                const bool snrRed = classifySweepSnr (snr, true) == QualityBand::Red;
                const bool thdRed = classifyThd (thd) == QualityBand::Red;
                if (snrRed || thdRed) CHECK (v.state != RefMonState::GradedClean);
            }
}

TEST_CASE ("qualityNote names the worst offender; IR-SNR advisory last", "[refmon][note]") {
    auto note = [] (bool m, float snr, bool v, float ir, float thd) {
        return qualityNote (aggregateVerdict (m, snr, v, ir, thd)).toStdString();
    };
    CHECK (note (false, 30, true, 60, 0).find ("re-learn") != std::string::npos);
    CHECK (note (true, 5,  true, 60, 0.01f).find ("Noisy capture") != std::string::npos);
    CHECK (note (true, 30, true, 60, 12).find ("distortion") != std::string::npos);
    CHECK (note (true, 22, true, 60, 0.01f).find ("Marginal SNR") != std::string::npos);
    // Both sweepSNR notes predict Dirac's "imprecise" verdict (low capture SNR -> phase correction dropped):
    // a low-SNR grade is the bridge's early warning for it (research-confirmed 2026-06-23).
    CHECK (note (true, 5,  true, 60, 0.01f).find ("imprecise") != std::string::npos);
    CHECK (note (true, 22, true, 60, 0.01f).find ("imprecise") != std::string::npos);
    CHECK (note (true, 30, true, 30, 0.01f).find ("Weak impulse") != std::string::npos);
    CHECK (note (true, 30, true, 60, 0.01f).empty());
}

TEST_CASE ("GradingOffHardware: blocks green, neutral colour, never inferred", "[refmon][hwdirac]") {
    // Hardware Dirac (DDRC-24 etc.): the box makes its own sweep -> no loopback reference -> grading off.
    CHECK (refMonBlocksGreen (RefMonState::GradingOffHardware));   // no reference grade -> never reads green
    CHECK (refMonColour (RefMonState::GradingOffHardware) == RefMonColour::Neutral);  // calm, NOT red/orange
    CHECK (RefMonState::GradingOffHardware != RefMonState::GradedSuspect);            // distinct from a warn
    // nextRefMonState NEVER returns it (published directly by the toggle path, never inferred from a grade):
    for (bool a : {false,true}) for (bool b : {false,true})
        for (bool c : {false,true}) for (bool d : {false,true})
            CHECK (nextRefMonState ({a,b,c,d}) != RefMonState::GradingOffHardware);
}

// ==================================================================================================
// #18: the Farina THD spots must key off the MEASURED sweep length, not the margin-padded buffer.
// The stored reference is trimmed with ~50 ms guard margins, so farinaHarmonicOffsets(bufferLen)
// displaced every harmonic image by margin*ln(k)/ln(f2/f1) — outside the old +-2-sample window —
// and thdPercent read ~0 on every real (distorted) measurement.
// ==================================================================================================
TEST_CASE("measuredSweepLength: recovers the active sweep inside a margin-padded reference [#18]") {
    const double fs = 48000.0;
    const int sweepLen = 96 * 1024;                 // ~2 s sweep (block-aligned for an exact expectation)
    const int margin   = 2400;                      // findActiveSpan's ~50 ms guard each side
    auto sweep = makeEss (sweepLen, fs);
    std::vector<float> padded ((size_t) (sweepLen + 2 * margin), 0.0f);
    std::copy (sweep.begin(), sweep.end(), padded.begin() + margin);

    const int measured = eb::measuredSweepLength (padded.data(), (int) padded.size());
    CHECK (measured > 0);
    // Block-quantized (1024): within a couple of blocks of the true active length.
    CHECK (std::abs (measured - sweepLen) <= 3 * 1024);

    // Degenerates: silence-only or empty -> 0 (callers fall back to the buffer length).
    std::vector<float> quiet (48000, 1.0e-6f);
    CHECK (eb::measuredSweepLength (quiet.data(), (int) quiet.size()) == 0);
    CHECK (eb::measuredSweepLength (nullptr, 0) == 0);
}

TEST_CASE("gradeMeasurement: real distortion is DETECTED through a margin-padded reference [#18]") {
    // The end-to-end regression: reference = margins + ESS + margins (the production shape after
    // findActiveSpan), response = the same chain through a memoryless nonlinearity. The harmonic
    // images land at offsets set by the TRUE sweep length; keying the spots off the padded buffer
    // length missed them all and THD read ~0. With the measured length + windowed spot search the
    // distortion must register.
    const double fs = 48000.0;
    const int sweepLen = 96 * 1024, margin = 2400;
    auto sweep = makeEss (sweepLen, fs);
    std::vector<float> ref ((size_t) (sweepLen + 2 * margin), 0.0f);
    std::copy (sweep.begin(), sweep.end(), ref.begin() + margin);

    auto distorted = ref;
    for (auto& v : distorted) v = 0.8f * v + 0.25f * v * v;   // strong 2nd-order distortion

    const auto g = eb::gradeMeasurement (ref.data(), distorted.data(), (int) ref.size(), fs);
    REQUIRE (g.match.matched);                                 // same sweep -> the gate passes
    INFO ("thdPercent = " << g.quality.thdPercent);
    CHECK (g.quality.thdPercent > 1.0f);                       // the distortion is VISIBLE, not ~0

    // And the clean control stays clean: same padded reference against itself reads ~0% THD.
    const auto clean = eb::gradeMeasurement (ref.data(), ref.data(), (int) ref.size(), fs);
    REQUIRE (clean.match.matched);
    INFO ("clean thdPercent = " << clean.quality.thdPercent);
    CHECK (clean.quality.thdPercent < 1.0f);
    CHECK (g.quality.thdPercent > 10.0f * (clean.quality.thdPercent + 0.01f));   // clear separation
}

// ===========================================================================
// SP3 — the GradeArtifacts out-param (Task 5 plumbing: MUST NOT change grading)
// ===========================================================================
TEST_CASE("GradeArtifacts: gradeMeasurement fills the IR + reference band, grading is UNCHANGED") {
    const int    n  = 1 << 15;        // 32768
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (n, fs, ref, resp);
    const int m = (int) resp.size();

    // Grade WITHOUT artifacts (the baseline verdict).
    const auto plain = eb::gradeMeasurement (ref.data(), resp.data(), m, fs);

    // Grade WITH artifacts — the plumbing must be byte-for-byte identical on state/irSnr/thd.
    eb::GradeArtifacts art;
    const auto withA = eb::gradeMeasurement (ref.data(), resp.data(), m, fs, 20.0, 20000.0,
                                             eb::kMinIrSnrDb, eb::kMaxThdPct, &art);
    CHECK (withA.state == plain.state);
    CHECK (withA.quality.irSnrDb   == plain.quality.irSnrDb);
    CHECK (withA.quality.thdPercent == plain.quality.thdPercent);
    CHECK (withA.match.matched == plain.match.matched);

    // The IR is handed out (padded pow-2 length >= m).
    CHECK (! art.ir.empty());
    CHECK ((int) art.ir.size() >= m);
    // gradedLen == the reference-length segment graded (== m here, the direct gradeMeasurement path).
    CHECK (art.gradedLen == m);
    // The derived reference band brackets the sweep's 20 Hz..20 kHz span. The SP2 band-edge derivation
    // sits just OUTSIDE the nominal sweep edges (the smoothed -12 dB run extends a hair past f1/f2), so
    // the low edge lands a little under 20 Hz — a low band of ~13 Hz and a high band of ~20 kHz is exactly
    // the reference's usable range, which is what the shape detectors must analyse.
    INFO ("bandLoHz=" << art.bandLoHz << " bandHiHz=" << art.bandHiHz);
    CHECK (art.bandLoHz >= 10.0);
    CHECK (art.bandLoHz <= 40.0);
    CHECK (art.bandHiHz >= 15000.0);
    CHECK (art.bandHiHz <= 21000.0);
}

TEST_CASE("GradeArtifacts: gradeMeasurementWindowAt reports the aligned windowStart, grade unchanged") {
    const int    refLen = 1 << 15;    // 32768
    const double fs     = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (refLen, fs, ref, resp);
    const int segLen = (int) resp.size();

    // Build a long window: leading room noise + the sweep somewhere inside.
    const int lead = 40000;           // >= 16384 so D5a's pre-sweep region is usable
    std::vector<float> window ((size_t) (lead + segLen), 0.0f);
    auto noise = whiteNoise (lead, 1.0e-4f, 7u);
    std::copy (noise.begin(), noise.end(), window.begin());
    std::copy (resp.begin(), resp.end(), window.begin() + lead);

    // Grade at the KNOWN offset (what decide()/matchAlign would locate).
    const auto plain = eb::gradeMeasurementWindowAt (ref.data(), segLen, window.data(), (int) window.size(),
                                                     lead, fs);
    eb::GradeArtifacts art;
    const auto withA = eb::gradeMeasurementWindowAt (ref.data(), segLen, window.data(), (int) window.size(),
                                                     lead, fs, 20.0, 20000.0, eb::kMinIrSnrDb, eb::kMaxThdPct, &art);
    CHECK (withA.state == plain.state);
    CHECK (withA.quality.irSnrDb == plain.quality.irSnrDb);
    CHECK (art.windowStart == lead);       // the pre-sweep noise region is [0, windowStart)
    CHECK (art.windowStart >= 16384);      // D5a eligibility (the noise region is long enough)
    CHECK (! art.ir.empty());
}
