// Plan 5 / #7 fix: the LIVE grading loop, end-to-end through the AudioEngine capture seam, now driven
// by DETERMINISTIC reference-match detection instead of the level-rise arm.
//
// The headline regression: a GRADUAL ESS response (ramps up from a low level — the kind the old
// rise-ratio arm MISSES, so SweepActive never arms) still grades, because the engine buffers the mic
// response CONTINUOUSLY into a rolling ring and fires the grade on an absolute-energy activity edge
// (sustained signal, then trailing silence). The match-gate (referenceMatches, inside
// gradeMeasurementWindow) is the correctness decision: a non-sweep transient (a finger snap) fails it.
//
// These tests prove:
//   1. GRADUAL sweep grades: a low-and-rising ESS response -> a graded (matched) state WITHOUT SweepActive
//      ever arming (the #7 headline). Sane, positive IR-SNR.
//   2. A normal (full-amplitude) matching ESS response through the seam -> graded (matched).
//   3. A WRONG-reference response -> ReferenceStale, NO quality number.
//   4. A short loud TRANSIENT (finger-snap-like) -> NOT graded clean (referenceMatches rejects it).
//   5. NO reference loaded -> NotGraded (the engine publishes it at the edge; nothing buffered).
//   6. Debounce: one completed sequence -> exactly one pending grade (not repeated).
//   7. Fix 2 presentation-gate invariants (unchanged pure-module behaviour).
//
// The test mirrors what MainComponent::pollReferenceGrade() does on the worker: copyGradingResponse()
// (the long ring WINDOW) -> gradeMeasurementWindow() (align + match-gate + grade) -> publishReferenceGrade().

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

// Build a (reference, response) pair from a clean direct-arrival IR (a single tap). Both length
// sweepLen + 255 so the FULL linear convolution is kept; the response is the room response to `ref`.
static void makeCleanMeasurement (int sweepLen, double fs,
                                  std::vector<float>& refOut, std::vector<float>& respOut) {
    auto ref = makeEss (sweepLen, fs);
    std::vector<float> roomIr ((size_t) 256, 0.0f);
    roomIr[50] = 1.0f;
    respOut = convolve (ref, roomIr);              // length sweepLen + 255
    refOut.assign (respOut.size(), 0.0f);
    std::copy (ref.begin(), ref.end(), refOut.begin());
}

// Drive a response through the REAL capture seam with the new CONTINUOUS-ring detection. We feed:
//   quiet warm-up -> the response block-by-block -> trailing silence (enough to fire the activity edge).
// The engine buffers the response continuously (NOT gated on SweepActive), so the grade fires on the
// trailing-silence edge whether or not the level arm ever armed. Returns whether SweepActive ever armed
// (so a test can ASSERT the gradual case never armed it, proving independence from the rise-ratio arm).
static bool driveResponseThroughSeam (eb::AudioEngine& e, const std::vector<float>& response, int N) {
    std::vector<float> mono ((size_t) N, 0.0f);
    std::vector<float> silence ((size_t) N, 0.0f);
    bool everArmed = false;

    // 1) Quiet warm-up so the floor settles (matches an on-device pre-sweep window).
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks; ++i) {
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
        if (e.sweepActive()) everArmed = true;
    }

    // 2) Feed the response on the LEFT channel (the capture buffers the left = the mono response path).
    const int total = (int) response.size();
    for (int off = 0; off < total; off += N) {
        const int cnt = std::min (N, total - off);
        std::vector<float> blk ((size_t) N, 0.0f);
        std::copy (response.begin() + off, response.begin() + off + cnt, blk.begin());
        e.processCaptureBlockForTest (blk.data(), silence.data(), mono.data(), N);
        if (e.sweepActive()) everArmed = true;
    }

    // 3) Trailing silence > Dirac's inter-sweep gap -> the activity edge fires (pendingGrade_ + ready).
    //    300 blocks @ 512/48k ~= 3.2 s, comfortably past the ~2 s kGradeSilenceSeconds.
    for (int i = 0; i < 300; ++i) {
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
        if (e.sweepActive()) everArmed = true;
    }
    return everArmed;
}

// Replays MainComponent::pollReferenceGrade(): copy the long ring WINDOW out, align+grade it against the
// reference via gradeMeasurementWindow, publish the verdict. Returns the grade so a test can inspect it.
static eb::MeasurementGrade gradeAndPublish (eb::AudioEngine& e, const std::vector<float>& reference,
                                             double rate) {
    REQUIRE (e.consumePendingGrade());                 // a sequence completed with a reference loaded
    REQUIRE (e.gradingResponseReady());                // a fresh window snapshot is ready

    std::vector<float> window ((size_t) std::lround (e.gradingResponseRate() * 28.0), 0.0f);
    const int got = e.copyGradingResponse (window.data(), (int) window.size());
    REQUIRE (got > 0);
    window.resize ((size_t) got);
    auto g = eb::gradeMeasurementWindow (reference.data(), (int) reference.size(),
                                         window.data(), (int) window.size(), rate);
    e.publishReferenceGrade ((int) g.state, g.quality.irSnrDb, g.quality.thdPercent,
                             g.state == eb::RefMonState::ReferenceStale, g.quality.lowQuality);
    return g;
}

// ==================================================================================================
// 1. HEADLINE (#7): a GRADUAL sweep response grades WITHOUT the level arm ever arming SweepActive.
// ==================================================================================================
TEST_CASE("Live grade #7: a GRADUAL (low-and-rising) ESS response grades even though SweepActive never arms") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;     // 32768
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    // Make the response GRADUAL: a long, SHALLOW linear amplitude ramp from ~0 up to ~0.2 across the
    // sweep — the kind of slow rise a real Dirac log-sweep makes. The rise-ratio arm tracks it with its
    // floor EWMA (0.95*floor + 0.05*peak) so peak never clears floor*kArmRiseRatio for the sustained run,
    // and SweepActive NEVER arms (the #7 bug — verified: the level arm misses this shape). But the response
    // still rises above the ABSOLUTE activity floor (kSweepStartLinear == kGradeActiveLinear), so the
    // deterministic activity edge fires and grades it. (A steeper 0->0.5 ramp DOES arm the level arm — the
    // shallow ramp is the honest reproduction of the slow on-device rise the arm cannot see.)
    const int m = (int) resp.size();
    for (int i = 0; i < m; ++i) {
        const float env = 0.2f * (float) i / (float) m;   // 0 -> 0.2 shallow linear ramp (defeats the rise-ratio arm)
        resp[(size_t) i] *= env;
    }

    e.setReferenceLoaded (true);
    const bool everArmed = driveResponseThroughSeam (e, resp, N);
    CHECK_FALSE (everArmed);                            // the headline: the level arm NEVER armed on the gradual sweep

    auto g = gradeAndPublish (e, ref, fs);
    const auto state = (eb::RefMonState) e.refMonState();
    INFO ("published IR-SNR = " << e.refIrSnrDb() << " dB; gradeState = " << (int) g.state);
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedSuspect));
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.refIrSnrDb() > 0.0f);
    CHECK (std::isfinite (e.refIrSnrDb()));
}

// ==================================================================================================
// 2. A normal (full-amplitude) matching response also grades as MATCHED.
// ==================================================================================================
TEST_CASE("Live grade: a full-amplitude matching ESS response through the seam grades as MATCHED") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, resp, N);
    gradeAndPublish (e, ref, fs);

    const auto state = (eb::RefMonState) e.refMonState();
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedSuspect));
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.refIrSnrDb() > 0.0f);
    CHECK (std::isfinite (e.refIrSnrDb()));
}

// ==================================================================================================
// 3. A WRONG reference -> ReferenceStale, NO quality number (the match-gate rejects it).
// ==================================================================================================
TEST_CASE("Live grade: a WRONG-reference response through the seam grades ReferenceStale (no quality)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    auto refA = makeEss (sweepLen, fs, 20.0,  20000.0);
    auto refB = makeEss (sweepLen, fs, 100.0, 8000.0);
    std::vector<float> roomIr ((size_t) 256, 0.0f); roomIr[40] = 1.0f; roomIr[90] = 0.25f;
    auto resp = convolve (refB, roomIr);

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, resp, N);
    gradeAndPublish (e, refA, fs);

    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::ReferenceStale);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.refIrSnrDb() == Catch::Approx (0.0f));
}

// ==================================================================================================
// 4. A short loud TRANSIENT (finger-snap-like) must NOT grade clean (referenceMatches rejects it).
// ==================================================================================================
TEST_CASE("Live grade: a finger-snap transient does NOT grade clean (the match-gate rejects it)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);    // the learned reference is a real sweep

    // The "measurement" is NOT a sweep: a couple of short loud bursts (snaps), then nothing. It clears the
    // absolute activity floor (so the edge could fire) but it is not the sweep -> the gate must reject it.
    std::vector<float> snap ((size_t) (N * 40), 0.0f);
    for (int b = 0; b < 20; ++b) snap[(size_t) (b * N + 10)] = 0.8f;   // sparse loud impulses across ~20 blocks

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, snap, N);

    // The activity edge MAY fire (loud enough), but the grade must NOT be a clean match.
    if (e.consumePendingGrade()) {
        std::vector<float> window ((size_t) std::lround (e.gradingResponseRate() * 28.0), 0.0f);
        const int got = e.copyGradingResponse (window.data(), (int) window.size());
        window.resize ((size_t) std::max (1, got));
        auto g = eb::gradeMeasurementWindow (ref.data(), (int) ref.size(),
                                             window.data(), (int) window.size(), fs);
        CHECK (g.state != eb::RefMonState::GradedClean);
        CHECK (g.state != eb::RefMonState::GradedSuspect);
        CHECK_FALSE (g.match.matched);                 // the snap is not the sweep -> the gate fails
    }
    // Either way, no clean/suspect verdict was published from a snap.
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::GradedClean);
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::GradedSuspect);
}

// ==================================================================================================
// 5. NO reference loaded -> NotGraded (the engine publishes it at the edge; nothing buffered/triggered).
// ==================================================================================================
TEST_CASE("Live grade: NO reference loaded -> NotGraded, no pending grade, no window buffered") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 14;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    driveResponseThroughSeam (e, resp, N);             // referenceLoaded stays false (the default)

    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::NotGraded);
    CHECK_FALSE (e.consumePendingGrade());             // no grade was requested
    CHECK_FALSE (e.gradingResponseReady());            // no window snapshot was made ready
    CHECK (eb::refMonBlocksGreen ((eb::RefMonState) e.refMonState()));
}

// ==================================================================================================
// 6. Debounce: ONE completed sequence fires EXACTLY ONE pending grade (not repeated).
// ==================================================================================================
TEST_CASE("Live grade debounce: one completed sequence fires exactly one pending grade") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, resp, N);

    CHECK (e.consumePendingGrade());                   // exactly one grade pending after the sequence
    CHECK_FALSE (e.consumePendingGrade());             // it was a single edge, not repeated

    // Further trailing silence (without fresh sustained activity) must NOT re-fire the trigger.
    std::vector<float> silence ((size_t) N, 0.0f), mono ((size_t) N, 0.0f);
    for (int i = 0; i < 300; ++i)
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
    CHECK_FALSE (e.consumePendingGrade());             // no re-grade of the same (already-ended) sequence
}

// ==================================================================================================
// 7. Fix 2: the IR thresholds stay PROVISIONAL (unratified) so a clean measurement is info-only.
// ==================================================================================================
TEST_CASE("Fix 2: the IR thresholds are PROVISIONAL (unratified) so a clean measurement is info-only") {
    CHECK_FALSE (eb::kIrThresholdsRatified);
}

TEST_CASE("Fix 2: a clean measurement's PURE verdict is lowQuality at the default cutoff, but the gate makes it info-only") {
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (n, fs, ref, resp);
    const int m = (int) resp.size();

    auto g = eb::gradeMeasurement (ref.data(), resp.data(), m, fs);   // default kMinIrSnrDb = 20 dB
    CHECK (g.match.matched);
    CHECK (g.state == eb::RefMonState::GradedSuspect);
    CHECK (g.quality.lowQuality);
    CHECK (g.quality.irSnrDb > 5.0f);
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
    CHECK (g.state == eb::RefMonState::ReferenceStale);
    CHECK (eb::irQualityNote (g.quality).startsWith ("Reference doesn't match"));
}

// NOTE: the RT-safety / allocation-free assertion for the in-callback ring write lives in
// tests/test_audioengine_callbacks.cpp, which already owns the global operator new/delete counter
// harness (a program can only replace operator new ONCE). See "buffering the in-sweep response ... is
// allocation-free" there.
