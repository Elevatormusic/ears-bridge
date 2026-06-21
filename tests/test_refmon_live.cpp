// Plan 5 / #7 fix: the LIVE grading loop, end-to-end through the AudioEngine capture seam, now driven
// by REFERENCE-MATCH detection instead of an absolute-level arm.
//
// The headline regression (Task 4): the OLD trigger armed only when the block peak crossed an ABSOLUTE
// -24 dBFS floor (kGradeActiveLinear == kSweepStartLinear) for 16 sustained blocks. On hardware, after
// the user lowered the level to stop mic clipping, a measurement sits at ~-25 dBFS — BELOW that arm — so
// it NEVER fired and the status stuck on "Listening...". The fix removes the absolute arm entirely: the
// capture thread buffers the mic response CONTINUOUSLY into a rolling ring and periodically publishes a
// snapshot, and the GUI poll grades from a `referenceMatches` cross-correlation of the snapshot against
// the learned reference — which fires at ANY level. The match itself is the detector; there is NO level
// gate anywhere in the grade path.
//
// These tests prove:
//   1. LOW-LEVEL grades (the Task-4 headline): a MATCHING ESS response at ~-25 dBFS peak — BELOW the old
//      -24 dBFS arm, which would NEVER have fired — still grades (GradedClean/Suspect, sane IR-SNR).
//   2. A normal (full-amplitude) matching ESS response also grades as MATCHED.
//   3. A WRONG-reference response -> ReferenceStale, NO quality number (the match-gate rejects it).
//   4. A short loud TRANSIENT (finger-snap-like) -> NOT graded clean (referenceMatches rejects it).
//   5. NO reference loaded -> NotGraded (the engine publishes it at the edge; nothing buffered/triggered).
//   6. Debounce: ONE settled sequence -> exactly ONE grade (the gradedThisSequence_ latch).
//   7. gradeSignalPresent() is true during signal, false in silence (the cosmetic activity flag).
//   8. Fix 2 presentation-gate invariants (unchanged pure-module behaviour).
//
// The match-poll helper here mirrors what MainComponent::pollReferenceGrade() does off the audio thread:
// snapshot the ring WINDOW (copyGradingResponse) -> referenceMatches (publish coherence) -> if matched
// AND settled (!gradeSignalPresent) AND not-already-graded -> gradeMeasurementWindow -> publishReferenceGrade.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include "audio/RefMonitor.h"
#include "audio/Deconvolver.h"      // eb::referenceMatches (the match-poll detector)
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

// Scale a buffer so its PEAK |sample| equals targetPeak (so a test can place a matching response BELOW
// the old absolute arm). Returns the achieved peak (== targetPeak unless the input was all-zero).
static float scaleToPeak (std::vector<float>& x, float targetPeak) {
    float pk = 0.0f;
    for (float v : x) pk = std::max (pk, std::abs (v));
    if (pk <= 0.0f) return 0.0f;
    const float g = targetPeak / pk;
    for (float& v : x) v *= g;
    return targetPeak;
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
//   quiet warm-up -> the response block-by-block -> trailing silence (so the signal SETTLES and the ring,
//   now holding the COMPLETE sequence, is what the next published snapshot reflects). To mirror on-device,
//   we drain the engine's published snapshot during the trailing silence so the capture thread re-snapshots
//   the COMPLETE ring (the producer re-publishes only after the consumer drains the prior window). NO
//   absolute level arm is involved — the ring is buffered continuously regardless of level. Returns whether
//   SweepActive ever armed (so a test can ASSERT a low response never armed it).
static bool driveResponseThroughSeam (eb::AudioEngine& e, const std::vector<float>& response, int N) {
    std::vector<float> mono ((size_t) N, 0.0f);
    std::vector<float> silence ((size_t) N, 0.0f);
    bool everArmed = false;
    std::vector<float> scratch ((size_t) std::lround (e.gradingResponseRate() * 28.0) + N, 0.0f);

    // 1) Quiet warm-up so the floor settles (matches an on-device pre-sweep window).
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks; ++i) {
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
        if (e.sweepActive()) everArmed = true;
    }

    // 2) Feed the response on the LEFT channel (the active ear defaults to left, so the ring buffers it).
    //    Drain any snapshot the capture thread published mid-response so the FINAL published snapshot can
    //    reflect the complete sweep (otherwise a stale early-sweep window would stick around).
    const int total = (int) response.size();
    for (int off = 0; off < total; off += N) {
        const int cnt = std::min (N, total - off);
        std::vector<float> blk ((size_t) N, 0.0f);
        std::copy (response.begin() + off, response.begin() + off + cnt, blk.begin());
        e.processCaptureBlockForTest (blk.data(), silence.data(), mono.data(), N);
        if (e.sweepActive()) everArmed = true;
        if (e.gradingResponseReady()) e.copyGradingResponse (scratch.data(), (int) scratch.size());  // drain stale mid-sweep window
    }

    // 3) Trailing silence so the signal SETTLES (gradeSignalPresent() goes false). Keep draining so the
    //    capture thread re-snapshots the now-COMPLETE ring; stop draining once a settled snapshot is ready
    //    so it remains available for the test's match poll. 300 blocks @ 512/48k ~= 3.2 s.
    bool haveSettledSnapshot = false;
    for (int i = 0; i < 300; ++i) {
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
        if (e.sweepActive()) everArmed = true;
        if (! haveSettledSnapshot && e.gradingResponseReady() && ! e.gradeSignalPresent())
            haveSettledSnapshot = true;   // leave THIS settled, complete-sweep window for the poll
        else if (! haveSettledSnapshot && e.gradingResponseReady())
            e.copyGradingResponse (scratch.data(), (int) scratch.size());   // still-active window: drain, wait for settle
    }
    return everArmed;
}

// The match-poll debounce state, mirroring MainComponent's gradedThisSequence_ / gradeWasSettled_ members.
// A fresh run starts settled (silent) + un-graded.
struct PollDebounce { bool gradedThisSequence = false; bool wasSettled = true; };

// Replays MainComponent::pollReferenceGrade(): track the settled/active edge (debounce), copy the published
// ring WINDOW out, run the match-gate (referenceMatches) — the DETECTOR — publish the coherence, and ONLY
// when matched AND the signal has SETTLED (!gradeSignalPresent) AND not already graded this sequence does it
// grade + publish. Returns the grade. There is NO absolute level gate here: the match alone fires the grade.
static eb::MeasurementGrade matchPollAndPublish (eb::AudioEngine& e, const std::vector<float>& reference,
                                                 double rate, bool& gradedOut, PollDebounce& db) {
    gradedOut = false;
    eb::MeasurementGrade g;   // default NotGraded

    // Debounce edge: a settled->active transition starts a NEW sweep sequence (clear the graded latch).
    const bool settled = ! e.gradeSignalPresent();
    if (db.wasSettled && ! settled) db.gradedThisSequence = false;
    db.wasSettled = settled;

    if (! e.gradingResponseReady()) return g;   // no fresh window published by the capture thread

    std::vector<float> window ((size_t) std::lround (e.gradingResponseRate() * 28.0), 0.0f);
    const int got = e.copyGradingResponse (window.data(), (int) window.size());
    if (got <= 0) return g;
    window.resize ((size_t) got);

    // The DETECTOR: cross-correlate the window against the learned reference. Publish the coherence
    // (so the diagnostic log sees it), exactly as the GUI poll does via setLastMatchCoherence.
    const auto m = eb::referenceMatches (reference.data(), window.data(),
                                         std::min ((int) reference.size(), (int) window.size()));
    e.setLastMatchCoherence (m.coherence);
    if (! m.matched || ! settled || db.gradedThisSequence) return g;   // gate FIRST; settled; not re-graded
    db.gradedThisSequence = true;

    g = eb::gradeMeasurementWindow (reference.data(), (int) reference.size(),
                                    window.data(), (int) window.size(), rate);
    e.publishReferenceGrade ((int) g.state, g.quality.irSnrDb, g.quality.thdPercent,
                             g.state == eb::RefMonState::ReferenceStale, g.quality.lowQuality);
    gradedOut = true;
    return g;
}

// ==================================================================================================
// 1. HEADLINE (Task 4): a LOW-LEVEL matching response grades, even though its peak sits BELOW the old
//    absolute -24 dBFS arm (which would NEVER have fired). The match itself is the detector.
// ==================================================================================================
TEST_CASE("Live grade (Task 4 headline): a LOW-level matching ESS response grades below the old -24 dBFS arm") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;     // 32768
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    // Place the response at ~-25 dBFS peak: BELOW the old kGradeActiveLinear == kSweepStartLinear (0.0631
    // == -24 dBFS) absolute arm. -25 dBFS == 10^(-25/20) ~= 0.0562. The old 16-sustained-block arm would
    // NEVER complete at this level (every block sits under the floor), so it could never have fired —
    // yet the match-based detector grades it anyway.
    const float lowPeak = 0.0562f;       // -25 dBFS
    const float achieved = scaleToPeak (resp, lowPeak);
    REQUIRE (achieved < eb::MeasurementSession::kSweepStartLinear);   // strictly below the OLD arm

    e.setReferenceLoaded (true);
    const bool everArmed = driveResponseThroughSeam (e, resp, N);
    CHECK_FALSE (everArmed);                            // the low level never armed the rise-ratio arm either

    bool graded = false;
    PollDebounce db;
    auto g = matchPollAndPublish (e, ref, fs, graded, db);
    CHECK (graded);                                     // the match fired the grade DESPITE the low level
    const auto state = (eb::RefMonState) e.refMonState();
    INFO ("published IR-SNR = " << e.refIrSnrDb() << " dB; gradeState = " << (int) g.state
          << "; coherence = " << e.lastMatchCoherence());
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedSuspect));
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.refIrSnrDb() > 0.0f);
    CHECK (std::isfinite (e.refIrSnrDb()));
    CHECK (e.lastMatchCoherence() > 0.0f);              // the poll published the match coherence
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
    bool graded = false;
    PollDebounce db;
    matchPollAndPublish (e, ref, fs, graded, db);
    CHECK (graded);

    const auto state = (eb::RefMonState) e.refMonState();
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedSuspect));
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.refIrSnrDb() > 0.0f);
    CHECK (std::isfinite (e.refIrSnrDb()));
}

// ==================================================================================================
// 3. A WRONG reference -> ReferenceStale, NO quality number. The match-gate runs FIRST (inside
//    gradeMeasurementWindow, on the precisely aligned segment): a non-match yields ReferenceStale with
//    NO quality verdict, never a low-quality grade.
// ==================================================================================================
TEST_CASE("Live grade: a WRONG-reference response through the seam grades ReferenceStale (no quality)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    // The learned reference is a real sweep; the "measurement" is NOT this sweep's response — it is a
    // band of NOISE (the user measured against a stale/wrong reference, or the loopback failed). It has
    // no sweep structure, so the matched-filter cross-correlation has no compact lobe -> the gate rejects
    // it at ANY alignment, and the match-gate (running FIRST) yields ReferenceStale with NO quality number.
    std::vector<float> ref, ignore;
    makeCleanMeasurement (sweepLen, fs, ref, ignore);
    std::vector<float> resp ((size_t) (sweepLen + 255), 0.0f);
    unsigned seed = 1234567u;
    for (auto& v : resp) { seed = seed * 1664525u + 1013904223u; v = 0.4f * ((float) (seed >> 9) / (float) (1u << 23) - 0.5f); }

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, resp, N);
    bool graded = false;
    PollDebounce db;
    auto g = matchPollAndPublish (e, ref, fs, graded, db);

    // The match-gate (FIRST) rejects the non-sweep noise: no clean/suspect grade, no quality number. The
    // coarse detector may or may not enter gradeMeasurementWindow; either way the precise gate inside it
    // returns ReferenceStale (never a low-quality grade), and the published state is NOT a clean capture.
    CHECK (g.state != eb::RefMonState::GradedClean);
    CHECK (g.state != eb::RefMonState::GradedSuspect);
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::GradedClean);
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::GradedSuspect);
    CHECK (e.refIrSnrDb() == Catch::Approx (0.0f));     // no quality number was produced from a non-match
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

    // The "measurement" is NOT a sweep: a couple of short loud bursts (snaps), then nothing. It is loud
    // (clears any cosmetic floor) but it is not the sweep -> the match-gate must reject it.
    std::vector<float> snap ((size_t) (N * 40), 0.0f);
    for (int b = 0; b < 20; ++b) snap[(size_t) (b * N + 10)] = 0.8f;   // sparse loud impulses across ~20 blocks

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, snap, N);

    bool graded = false;
    PollDebounce db;
    auto g = matchPollAndPublish (e, ref, fs, graded, db);
    CHECK_FALSE (graded);                              // the snap is not the sweep -> no grade fired
    CHECK_FALSE (g.match.matched);
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::GradedClean);
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::GradedSuspect);
}

// ==================================================================================================
// 5. NO reference loaded -> NotGraded (the engine publishes it at the edge; nothing buffered/triggered).
// ==================================================================================================
TEST_CASE("Live grade: NO reference loaded -> NotGraded, no window buffered") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 14;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    driveResponseThroughSeam (e, resp, N);             // referenceLoaded stays false (the default)

    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::NotGraded);
    CHECK_FALSE (e.gradingResponseReady());            // no window snapshot was made ready (no reference)
    CHECK (eb::refMonBlocksGreen ((eb::RefMonState) e.refMonState()));
}

// ==================================================================================================
// 6. Debounce: ONE settled sequence grades EXACTLY ONCE. A second poll over the same (still-settled,
//    unchanged) window does NOT re-grade — the gradedThisSequence_ latch holds until fresh activity.
// ==================================================================================================
TEST_CASE("Live grade debounce: one settled sequence grades exactly once") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    e.setReferenceLoaded (true);
    driveResponseThroughSeam (e, resp, N);

    bool graded1 = false;
    PollDebounce db;
    matchPollAndPublish (e, ref, fs, graded1, db);
    CHECK (graded1);                                   // first poll grades the settled sequence

    // A second poll without any fresh signal activity must NOT re-grade: even though the capture thread may
    // re-publish a snapshot of the same (still sweep-containing) ring, the gradedThisSequence_ debounce holds
    // until a fresh settled->active edge starts a NEW sequence — which never happens here (all silence).
    std::vector<float> silence ((size_t) N, 0.0f), mono ((size_t) N, 0.0f);
    for (int i = 0; i < 60; ++i)
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
    bool graded2 = false;
    matchPollAndPublish (e, ref, fs, graded2, db);
    CHECK_FALSE (graded2);                             // no re-grade of the same settled sequence
}

// ==================================================================================================
// 7. The cosmetic activity flag: gradeSignalPresent() is TRUE while signal is above the low floor and
//    FALSE in silence. It drives ONLY the "Sweep in progress..." status — never the grade.
// ==================================================================================================
TEST_CASE("gradeSignalPresent(): true during signal, false in silence (cosmetic activity flag)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);
    e.setReferenceLoaded (true);

    std::vector<float> silence ((size_t) N, 0.0f), mono ((size_t) N, 0.0f);

    // Silence -> not present.
    e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
    CHECK_FALSE (e.gradeSignalPresent());

    // A block well above the low activity floor (~-50 dBFS) but BELOW the old -24 dBFS arm -> present.
    std::vector<float> sig ((size_t) N, 0.02f);        // -34 dBFS, above ~-50 floor, below -24 arm
    REQUIRE (0.02f < eb::MeasurementSession::kSweepStartLinear);
    e.processCaptureBlockForTest (sig.data(), silence.data(), mono.data(), N);
    CHECK (e.gradeSignalPresent());

    // Back to silence -> not present again (single-block responsiveness).
    e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
    CHECK_FALSE (e.gradeSignalPresent());
}

// ==================================================================================================
// Task 2: the read-only diagnostic getter lastInputBlockPeak() reflects the live input block peak.
// ==================================================================================================
TEST_CASE("Task 2 diag getter: lastInputBlockPeak() reflects the driven block peak; lastMatchCoherence() defaults to 0") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    // Before any block, both getters read their default 0.
    CHECK (e.lastInputBlockPeak()  == Catch::Approx (0.0f));
    CHECK (e.lastMatchCoherence()  == Catch::Approx (0.0f));

    // Drive a block whose peak is a known value: a constant 0.5 on the left, quieter 0.25 on the right.
    // blockPeak is the max |sample| over BOTH channels, so the expected peak is 0.5.
    const float knownPeak = 0.5f;
    std::vector<float> inL ((size_t) N, knownPeak);
    std::vector<float> inR ((size_t) N, 0.25f);
    std::vector<float> mono ((size_t) N, 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), N);

    // The getter mirrors the live input level (within the 1/1000 fixed-point quantization tolerance).
    CHECK (e.lastInputBlockPeak() == Catch::Approx (knownPeak).margin (0.001f));
    // lastMatchCoherence stays at its default until Task 4's match poll sets it.
    CHECK (e.lastMatchCoherence() == Catch::Approx (0.0f));
}

// ==================================================================================================
// 8. Fix 2: the IR thresholds stay PROVISIONAL (unratified) so a clean measurement is info-only.
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
// harness (a program can only replace operator new ONCE).
