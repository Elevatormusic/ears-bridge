// Plan 5 review-fix: the LIVE grading loop, end-to-end through the AudioEngine capture seam.
//
// These tests prove the wiring the whole-branch review found "wired-but-dead":
//   1. A synthetic MATCHING ESS response driven through the capture seam (with a reference loaded)
//      is buffered during SweepActive, snapshotted at the sweep-complete edge, copied out via
//      copyGradingResponse(), graded with gradeMeasurement, and published -> a MATCHED ("graded")
//      state with a sane IR-SNR (NEVER ReferenceStale / NotGraded).
//   2. A WRONG-reference response -> ReferenceStale, NO quality number.
//   3. NO reference loaded -> NotGraded (the engine publishes it at the edge; no response buffered).
//   4. Fix 2: with kIrThresholdsRatified == false a clean (~13 dB) measurement does NOT drive a warn
//      (its pure verdict is lowQuality, but the gate keeps it info-only); a mismatch still re-learns.
//   5. RT-safety: the in-sweep response copy is a pre-allocated memcpy (alloc-free on the capture path).
//
// The test mirrors exactly what MainComponent::pollReferenceGrade() does on the message/worker thread:
// copyGradingResponse() -> gradeMeasurement() -> publishReferenceGrade(). It drives the response THROUGH
// the real capture seam (processCaptureBlockForTest), so the response-buffer-during-sweep path is live.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include "audio/RefMonitor.h"
#include "gui/IrQualityStatus.h"   // eb::kIrThresholdsRatified (the Fix-2 presentation gate)
#include <vector>
#include <cmath>
#include <algorithm>

using Catch::Matchers::WithinAbs;

static constexpr double kPi = 3.14159265358979323846;

// --- Synthetic harness (mirrors tests/test_refmonitor.cpp / test_deconvolver.cpp) -----------------
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

// Build a (reference, response) pair from a clean direct-arrival IR (same construction the pure
// RefMonitor tests use), both length n+255 so the FULL linear convolution is kept.
static void makeCleanMeasurement (int sweepLen, double fs,
                                  std::vector<float>& refOut, std::vector<float>& respOut) {
    auto ref = makeEss (sweepLen, fs);
    std::vector<float> roomIr ((size_t) 256, 0.0f);
    roomIr[50] = 1.0f;
    respOut = convolve (ref, roomIr);              // length sweepLen + 255
    refOut.assign (respOut.size(), 0.0f);
    std::copy (ref.begin(), ref.end(), refOut.begin());
}

// Drive a response through the REAL capture seam (the engine buffers the in-sweep mic input). The
// response itself is the sweep signal (as on-device): a quiet warm-up lets the floor settle, then the
// response is fed block-by-block. The session arms after a sustained rise (~a dozen blocks); the engine
// buffers the response ONLY once SweepActive, so the first armed block is captured as response[offset].
// We return that `offset` (samples consumed before capture began) so the caller can align the reference
// to the captured segment (reference[offset:] vs the captured buffer) for a clean deconvolution.
// Then terminal silence drives Complete (firing the sweep-complete edge that snapshots the response).
static int driveResponseThroughSweep (eb::AudioEngine& e, const std::vector<float>& response, int N) {
    std::vector<float> mono ((size_t) N, 0.0f);
    std::vector<float> silence ((size_t) N, 0.0f);

    // 1) Warm-up: settle the floor so the arm is allowed (kArmWarmupBlocks Preflight blocks).
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks; ++i)
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);

    // 2) Feed the response on the LEFT channel (the capture buffers the left = the mono response path).
    //    Track how many samples were consumed BEFORE the arming block (the offset the engine did not
    //    capture). consumeSweepStarted() resets the engine's write index to 0 on the arming block, so the
    //    captured buffer begins exactly at the first SweepActive block = response[offset].
    const int total = (int) response.size();
    int offset = 0;
    bool armed = false;
    for (int off = 0; off < total; off += N) {
        const int cnt = std::min (N, total - off);
        std::vector<float> blk ((size_t) N, 0.0f);
        std::copy (response.begin() + off, response.begin() + off + cnt, blk.begin());
        const bool wasActive = e.sweepActive();
        e.processCaptureBlockForTest (blk.data(), silence.data(), mono.data(), N);
        if (! armed && ! wasActive && e.sweepActive()) {   // THIS block flipped Preflight -> SweepActive
            offset = off;                                  // the arming block is captured as response[offset]
            armed = true;
        }
    }
    REQUIRE (armed);   // the response must have armed the sweep

    // 3) Terminal silence -> sustained below-floor run -> Complete (fires consumeSweepComplete; the engine
    //    snapshots the captured length + sets responseReady_ when a reference is loaded).
    for (int i = 0; i < 200; ++i)
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
    return offset;
}

// Replays exactly what MainComponent::pollReferenceGrade() does once a sweep completed with a reference
// loaded: copy the response out of the engine, grade it against the reference, publish the verdict. The
// reference is aligned to the captured segment by `offset` (the samples the engine did not capture before
// SweepActive) — on-device the GUI grades the captured response against the learned reference; here we
// align so the synthetic deconvolution starts at the same sweep sample.
static eb::MeasurementGrade gradeAndPublish (eb::AudioEngine& e, const std::vector<float>& referenceFull,
                                             int offset, double rate) {
    REQUIRE (e.consumePendingGrade());                 // a sweep completed with a reference loaded
    REQUIRE (e.gradingResponseReady());                // a fresh response snapshot is ready

    std::vector<float> reference (referenceFull.begin() + std::min (offset, (int) referenceFull.size()),
                                  referenceFull.end());
    std::vector<float> response (reference.size(), 0.0f);
    const int got = e.copyGradingResponse (response.data(), (int) response.size());
    REQUIRE (got > 0);
    const int n = std::min ((int) reference.size(), (int) response.size());
    auto g = eb::gradeMeasurement (reference.data(), response.data(), n, rate);
    e.publishReferenceGrade ((int) g.state, g.quality.irSnrDb, g.quality.thdPercent,
                             g.state == eb::RefMonState::ReferenceStale, g.quality.lowQuality);
    return g;
}

// ==================================================================================================
// 1. Live grade via the seam: a MATCHING response -> a graded (matched) state with a sane IR-SNR
// ==================================================================================================
TEST_CASE("Live grade: a matching ESS response through the seam grades as MATCHED with a real IR-SNR") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;     // 32768
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    e.setReferenceLoaded (true);                       // a reference is loaded -> the edge will buffer+grade
    const int offset = driveResponseThroughSweep (e, resp, N);
    gradeAndPublish (e, ref, offset, fs);

    const auto state = (eb::RefMonState) e.refMonState();
    // "graded" = the match-gate passed: GradedClean (ratified-clean) OR GradedSuspect (clean but below the
    // PROVISIONAL cutoff while unratified). It must NOT be ReferenceStale or NotGraded.
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedSuspect));
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));   // matched -> no mismatch flag
    // A real deconvolved IR-SNR number was published. It reads POSITIVE for a clean direct-arrival (the
    // exact dB is lower than the offline ~13 dB because the seam captures from the arm point, losing the
    // low-frequency sweep lead-in — that is the genuine on-device behaviour the live path exercises).
    INFO ("published IR-SNR = " << e.refIrSnrDb() << " dB, THD = " << e.refThdPercent() << "%");
    CHECK (e.refIrSnrDb() > 0.0f);                     // a sane, positive IR quality number was published
    CHECK (std::isfinite (e.refIrSnrDb()));
    CHECK (e.refThdPercent() >= 0.0f);
}

// ==================================================================================================
// 2. Live grade via the seam: a WRONG reference -> ReferenceStale, NO quality number
// ==================================================================================================
TEST_CASE("Live grade: a WRONG-reference response through the seam grades ReferenceStale (no quality)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    // The learned reference is a 20 Hz..20 kHz sweep; the response is to a DIFFERENT (narrow-band) sweep.
    auto refA = makeEss (sweepLen, fs, 20.0,  20000.0);
    auto refB = makeEss (sweepLen, fs, 100.0, 8000.0);
    std::vector<float> roomIr ((size_t) 256, 0.0f); roomIr[40] = 1.0f; roomIr[90] = 0.25f;
    auto resp = convolve (refB, roomIr);
    resp.resize ((size_t) sweepLen);
    refA.resize ((size_t) sweepLen);                   // grade equal-length segments

    e.setReferenceLoaded (true);
    const int offset = driveResponseThroughSweep (e, resp, N);
    gradeAndPublish (e, refA, offset, fs);

    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::ReferenceStale);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));         // the gate flagged the mismatch
    CHECK (e.refIrSnrDb() == Catch::Approx (0.0f));                            // NO quality number on a non-match
}

// ==================================================================================================
// 3. NO reference loaded -> NotGraded (the engine publishes it at the edge; nothing buffered)
// ==================================================================================================
TEST_CASE("Live grade: NO reference loaded -> NotGraded, no pending grade, no response buffered") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 14;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    // referenceLoaded stays false (the default) -> the edge publishes NotGraded and buffers nothing.
    driveResponseThroughSweep (e, resp, N);

    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::NotGraded);
    CHECK_FALSE (e.consumePendingGrade());             // no grade was requested
    CHECK_FALSE (e.gradingResponseReady());            // no response snapshot was made ready
    CHECK (eb::refMonBlocksGreen ((eb::RefMonState) e.refMonState()));   // NotGraded never reads green
}

// ==================================================================================================
// 4. Fix 2: while UNRATIFIED, a clean (~13 dB) measurement is info-only (lowQuality but NO warn);
//    a mismatch still re-learns. We pin the gate constant + the pure verdict the GUI gates on.
// ==================================================================================================
TEST_CASE("Fix 2: the IR thresholds are PROVISIONAL (unratified) so a clean measurement is info-only") {
    // The presentation gate is OFF until on-device ratification: a clean ~13 dB measurement must not warn.
    CHECK_FALSE (eb::kIrThresholdsRatified);
}

TEST_CASE("Fix 2: a clean measurement's PURE verdict is lowQuality at the default cutoff, but the gate makes it info-only") {
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (n, fs, ref, resp);
    const int m = (int) resp.size();

    // At the DEFAULT (provisional 20 dB) cutoff the real ESS deconvolution (~13 dB) reads lowQuality ->
    // GradedSuspect in the pure module. This is the FALSE-warn the gate suppresses: while kIrThresholdsRatified
    // is false the GUI presents GradedSuspect as INFO (numbers shown, neutral tone), never a warn-coloured
    // "low quality" string. The lowQuality computation STAYS live in the pure module for ratification.
    auto g = eb::gradeMeasurement (ref.data(), resp.data(), m, fs);   // default kMinIrSnrDb = 20 dB
    CHECK (g.match.matched);
    CHECK (g.state == eb::RefMonState::GradedSuspect);
    CHECK (g.quality.lowQuality);                       // the pure verdict (kept for the campaign)
    CHECK (g.quality.irSnrDb > 5.0f);                   // ~13 dB: a clean measurement, NOT actually bad
    // The presentation gate is the thing that keeps this from warning the user (asserted above).
    CHECK_FALSE (eb::kIrThresholdsRatified);
}

TEST_CASE("Fix 2: a mismatch STILL produces the re-learn gate verdict (the gate is always valid)") {
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    auto refA = makeEss (n, fs, 20.0,  20000.0);
    auto refB = makeEss (n, fs, 100.0, 8000.0);
    std::vector<float> roomIr ((size_t) 256, 0.0f); roomIr[40] = 1.0f;
    auto resp = convolve (refB, roomIr);
    resp.resize ((size_t) n);
    auto g = eb::gradeMeasurement (refA.data(), resp.data(), n, fs);
    CHECK (g.state == eb::RefMonState::ReferenceStale);   // a mismatch re-learns regardless of the gate
    CHECK (eb::irQualityNote (g.quality).startsWith ("Reference doesn't match"));
}

// NOTE: the RT-safety / allocation-free assertion for the in-sweep response-buffer copy lives in
// tests/test_audioengine_callbacks.cpp, which already owns the global operator new/delete counter
// harness (a program can only replace operator new ONCE — defining it here too would be a multiple-
// definition link error). See "buffering the in-sweep response ... is allocation-free" there.
