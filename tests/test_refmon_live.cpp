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
// snapshot the ring WINDOW (copyGradingResponse) -> referenceMatches (publish coherence) -> if the match is
// STABLE (matched on TWO consecutive polls) AND not-already-graded -> gradeMeasurementWindow -> publish.
// The grade triggers on a STABLE MATCH, NOT on silence: the old !gradeSignalPresent ("settled") gate is gone
// because the EARS mic never goes silent (it always reads ambient above the cosmetic floor), so a real run
// never settled and never graded even at coherence 0.99 (the on-device bug). matchPollAndPublish must be
// called TWICE for a grade to fire (two matched polls confirm the full sweep landed, not still rising).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include "audio/RefMonitor.h"
#include "audio/Deconvolver.h"            // eb::referenceMatches (the match-poll detector)
#include "audio/ReferenceGradePoller.h"   // eb::ReferenceGradePoller (the ALIGNED match + grade decision)
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

// Bug-1 keep-alive: a block of present-level signal on the RIGHT (INACTIVE-ear) channel only. blockPeak (and
// thus gradeSignalPresent()) covers BOTH channels, so a present right channel keeps gradeSignalPresent() TRUE;
// but the grading ring buffers only the ACTIVE ear (left by default), so the LEFT channel stays SILENT and the
// graded window remains clean (sweep + silence, the sweep's compact correlation lobe intact -> still matches).
// This is the faithful "the mic never goes silent, yet the captured sweep is clean" condition.
static void fillKeepAlivePresentRight (std::vector<float>& left, std::vector<float>& right) {
    std::fill (left.begin(),  left.end(),  0.0f);     // active ear (buffered) stays clean
    std::fill (right.begin(), right.end(), 0.02f);    // inactive ear present (-34 dBFS) -> gradeSignalPresent TRUE
}

// Bug-1 harness: drive a CONTINUOUS matching response whose level NEVER drops below the activity floor, so
// gradeSignalPresent() stays TRUE the whole time (the run NEVER "settles"). This is the on-device case the
// old !gradeSignalPresent silence-gate could never grade. We feed the complete sweep on the active (left) ear,
// then keep a present keep-alive on the INACTIVE (right) ear so the signal never goes silent — while the active
// ear (the graded ring) sees only silence after the sweep, so the published window stays clean and matching.
// Returns the steady-present flag (the caller asserts gradeSignalPresent() is TRUE -> never settled).
static bool driveContinuousNeverSettles (eb::AudioEngine& e, const std::vector<float>& response, int N) {
    std::vector<float> mono ((size_t) N, 0.0f);
    std::vector<float> silence ((size_t) N, 0.0f);
    std::vector<float> scratch ((size_t) std::lround (e.gradingResponseRate() * 28.0) + N, 0.0f);

    // 1) Feed the COMPLETE response on the LEFT (active) channel, draining stale mid-sweep windows.
    const int total = (int) response.size();
    for (int off = 0; off < total; off += N) {
        const int cnt = std::min (N, total - off);
        std::vector<float> blk ((size_t) N, 0.0f);
        std::copy (response.begin() + off, response.begin() + off + cnt, blk.begin());
        e.processCaptureBlockForTest (blk.data(), silence.data(), mono.data(), N);
        if (e.gradingResponseReady()) e.copyGradingResponse (scratch.data(), (int) scratch.size());
    }

    // 2) Keep the INACTIVE ear (right) present so the signal NEVER settles, while the active (left) ear is
    //    silent. Drain stale windows until one holds the complete sweep, then leave it ready for the poll.
    std::vector<float> keepL ((size_t) N, 0.0f), keepR ((size_t) N, 0.0f);
    fillKeepAlivePresentRight (keepL, keepR);
    for (int i = 0; i < 200; ++i) {
        e.processCaptureBlockForTest (keepL.data(), keepR.data(), mono.data(), N);
        if (e.gradingResponseReady()) {
            if (i >= 60) break;   // settle a bit, then leave THIS ready window for the poll (do NOT drain it)
            e.copyGradingResponse (scratch.data(), (int) scratch.size());   // drain early stale windows
        }
    }
    // The signal is STILL sounding (the last block was present on the right ear) -> never settled.
    return e.gradeSignalPresent();
}

// The match-poll debounce state, mirroring MainComponent's lastPollMatched_ / gradedThisSession_ members.
// A fresh run starts un-matched + un-graded (so two consecutive matched polls are required to grade).
struct PollDebounce { bool lastPollMatched = false; bool gradedThisSession = false; };

// Replays MainComponent::pollReferenceGrade(): copy the published ring WINDOW out, run the match-gate
// (referenceMatches) — the DETECTOR — publish the coherence, and grade ONLY on a STABLE MATCH (matched on
// THIS poll AND the previous poll) that has not already graded this match session. There is NO settled/silence
// gate and NO absolute level gate: the match alone fires the grade, and it must be stable across two polls.
// A dropped match clears the session latch so a fresh sweep can grade again. Call TWICE to grade.
static eb::MeasurementGrade matchPollAndPublish (eb::AudioEngine& e, const std::vector<float>& reference,
                                                 double rate, bool& gradedOut, PollDebounce& db) {
    gradedOut = false;
    eb::MeasurementGrade g;   // default NotGraded

    if (! e.gradingResponseReady()) return g;   // no fresh window published by the capture thread

    std::vector<float> window ((size_t) std::lround (e.gradingResponseRate() * 28.0), 0.0f);
    const int got = e.copyGradingResponse (window.data(), (int) window.size());
    if (got <= 0) return g;
    window.resize ((size_t) got);

    // The DETECTOR, routed through the ALIGNED poller (Fix 1): decide() cross-correlates to LOCATE the sweep
    // anywhere in the snapshot (the ring is oldest->newest, so the just-finished sweep sits at the END, OUTSIDE
    // the old prefix the inline referenceMatches read), then runs the match-gate on the aligned segment. Publish
    // the coherence exactly as the GUI poll does. The PollDebounce struct here carries the same two-poll state the
    // poller holds internally, so we drive decide() and replicate its debounce against this struct.
    eb::ReferenceGradePoller poller;
    const auto d = poller.decide (window.data(), (int) window.size(),
                                  reference.data(), (int) reference.size());
    e.setLastMatchCoherence (d.coherence);

    // STABLE-MATCH debounce (mirrors pollReferenceGrade): the match-gate runs FIRST. A dropped match re-arms
    // the session; a grade fires once when the match holds across TWO consecutive polls.
    if (! d.matched) db.gradedThisSession = false;
    const bool stableMatch = d.matched && db.lastPollMatched;
    db.lastPollMatched = d.matched;
    if (! stableMatch || db.gradedThisSession) return g;   // gate FIRST; stable across two polls; not re-graded
    db.gradedThisSession = true;

    // Grade at the SAME offset decide() located (the gate and the grade agree on where the sweep is; no 2nd xcorr).
    const auto gp = eb::ReferenceGradePoller::gradeWindow (window.data(), (int) window.size(),
                                                           reference.data(), (int) reference.size(), rate,
                                                           d.alignOffset);
    g.state = gp.state;
    g.quality.irSnrDb   = gp.irSnrDb;
    g.quality.thdPercent = gp.thdPercent;
    g.quality.lowQuality = gp.lowQuality;
    g.match.matched = (gp.state == eb::RefMonState::GradedClean || gp.state == eb::RefMonState::GradedMarginal
                       || gp.state == eb::RefMonState::GradedSuspect);   // any graded quality verdict == it matched
    e.publishReferenceGrade ((int) g.state, g.quality.irSnrDb, g.quality.thdPercent,
                             g.state == eb::RefMonState::ReferenceStale, g.quality.lowQuality);
    gradedOut = true;
    return g;
}

// Convenience: run matchPollAndPublish TWICE (mirrors two consecutive throttled ~2 s polls). The first poll
// CONSUMES the ready window (clearing responseReady_) and only records lastPollMatched; the SECOND poll is the
// one that can grade. On hardware the capture thread keeps streaming between the throttled polls and
// re-publishes a fresh window, so here we PUMP a short burst of present-level blocks between the two polls to
// trigger that re-snapshot — the sweep is still inside the 28 s ring, so the republished window still matches.
// Returns the grade from whichever poll graded (or the last poll's grade if none did).
static eb::MeasurementGrade matchPollTwiceAndPublish (eb::AudioEngine& e, const std::vector<float>& reference,
                                                      double rate, bool& gradedOut, PollDebounce& db) {
    bool g1 = false, g2 = false;
    auto r1 = matchPollAndPublish (e, reference, rate, g1, db);

    // Mirror the capture thread re-publishing between the throttled polls: pump >1 snapshot-period of SILENCE
    // so a fresh window (still holding the sweep in the 28 s ring) is made ready for poll 2 — without
    // contaminating it, so referenceMatches still sees the same prominent sweep lobe. The 0.5 s snapshot
    // cadence at 512/48k is ~47 blocks; 80 covers it with margin.
    const int N = 512;
    std::vector<float> silence ((size_t) N, 0.0f), mono ((size_t) N, 0.0f);
    for (int i = 0; i < 80; ++i)
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);

    auto r2 = matchPollAndPublish (e, reference, rate, g2, db);
    gradedOut = g1 || g2;
    return g1 ? r1 : r2;
}

// Bug-1 two-poll: run the match poll TWICE while KEEPING SIGNAL PRESENT between the polls (present keep-alive
// on the INACTIVE ear only), so gradeSignalPresent() stays TRUE across both polls and at grade time — the
// exact condition the removed silence-gate could never grade. The active ear stays silent, so the republished
// window (the active-ear ring) is still the clean sweep and matches. Returns the grade from whichever graded.
static eb::MeasurementGrade bug1PollTwicePresent (eb::AudioEngine& e, const std::vector<float>& reference,
                                                  double rate, bool& gradedOut, PollDebounce& db) {
    bool g1 = false, g2 = false;
    auto r1 = matchPollAndPublish (e, reference, rate, g1, db);

    // Pump present-on-the-right keep-alive so a fresh window is republished for poll 2 AND the signal stays
    // present (gradeSignalPresent() TRUE at grade time) — while the active (left) ring stays clean. The first
    // poll already drained the prior window, so one re-snapshot publishes during this burst and stays ready.
    const int N = 512;
    std::vector<float> mono ((size_t) N, 0.0f);
    std::vector<float> keepL ((size_t) N, 0.0f), keepR ((size_t) N, 0.0f);
    fillKeepAlivePresentRight (keepL, keepR);
    for (int i = 0; i < 80; ++i)
        e.processCaptureBlockForTest (keepL.data(), keepR.data(), mono.data(), N);

    auto r2 = matchPollAndPublish (e, reference, rate, g2, db);
    gradedOut = g1 || g2;
    return g1 ? r1 : r2;
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
    auto g = matchPollTwiceAndPublish (e, ref, fs, graded, db);
    CHECK (graded);                                     // the match fired the grade DESPITE the low level
    const auto state = (eb::RefMonState) e.refMonState();
    INFO ("published IR-SNR = " << e.refIrSnrDb() << " dB; gradeState = " << (int) g.state
          << "; coherence = " << e.lastMatchCoherence());
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedMarginal
            || state == eb::RefMonState::GradedSuspect));   // any quality verdict; no-noise-region synthetic window -> Marginal
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
    matchPollTwiceAndPublish (e, ref, fs, graded, db);
    CHECK (graded);

    const auto state = (eb::RefMonState) e.refMonState();
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedMarginal
            || state == eb::RefMonState::GradedSuspect));   // any quality verdict; no-noise-region synthetic window -> Marginal
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.refIrSnrDb() > 0.0f);
    CHECK (std::isfinite (e.refIrSnrDb()));
}

// ==================================================================================================
// 2b. BUG-1 HEADLINE (the on-device regression): a CONTINUOUS matching response that NEVER settles still
//     grades. gradeSignalPresent() stays TRUE the WHOLE time (the EARS mic never goes silent), so the OLD
//     !gradeSignalPresent silence-gate could NEVER have fired — yet two consecutive matched polls grade it.
//     This proves the silence-gate is GONE and grading now triggers on a STABLE MATCH.
// ==================================================================================================
TEST_CASE("Bug 1: a continuous response that NEVER settles still grades on a stable match (silence-gate removed)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);

    e.setReferenceLoaded (true);
    const bool everPresent = driveContinuousNeverSettles (e, resp, N);
    REQUIRE (everPresent);                              // the signal is STILL present at the end -> never settled
    REQUIRE (e.gradeSignalPresent());                  // gradeSignalPresent() is TRUE -> the old gate could not fire

    // Two consecutive matched polls (the new stable-match trigger). With gradeSignalPresent() TRUE throughout,
    // the OLD code's `! settled` gate would have returned before grading on EVERY poll. It now grades.
    bool graded = false;
    PollDebounce db;
    auto g = bug1PollTwicePresent (e, ref, fs, graded, db);
    INFO ("published IR-SNR = " << e.refIrSnrDb() << " dB; gradeState = " << (int) g.state
          << "; coherence = " << e.lastMatchCoherence() << "; signalPresent = " << e.gradeSignalPresent());
    CHECK (graded);                                    // a grade IS published despite never settling
    CHECK (e.gradeSignalPresent());                    // and the signal is STILL present (never settled) at grade time
    const auto state = (eb::RefMonState) e.refMonState();
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedMarginal
            || state == eb::RefMonState::GradedSuspect));   // any quality verdict; no-noise-region synthetic window -> Marginal
    CHECK (state != eb::RefMonState::NotGraded);
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (e.refIrSnrDb() > 0.0f);                     // an IR-SNR number was produced (the grade ran)
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
    auto g = matchPollTwiceAndPublish (e, ref, fs, graded, db);

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
    auto g = matchPollTwiceAndPublish (e, ref, fs, graded, db);
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
// 6. Debounce: a SUSTAINED MATCH grades EXACTLY ONCE, until the match drops and returns. Once a match
//    session has graded, further polls over the same (still-matching) window do NOT re-grade — the
//    gradedThisSession_ latch holds until referenceMatches reports a non-match (a fresh sweep), which clears
//    it so the next sustained match can grade again.
// ==================================================================================================
TEST_CASE("Live grade debounce: a sustained match grades exactly once until it drops and returns") {
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
    matchPollTwiceAndPublish (e, ref, fs, graded1, db);   // two matched polls -> grades once
    CHECK (graded1);                                       // first sustained match grades

    // Further polls over the same still-matching window must NOT re-grade: the gradedThisSession_ debounce
    // holds until the match drops (a non-match). The window still contains the sweep, so each of these polls
    // still matches — yet no grade fires.
    for (int i = 0; i < 4; ++i) {
        bool g = false;
        matchPollAndPublish (e, ref, fs, g, db);
        CHECK_FALSE (g);                                   // no re-grade while the same match session holds
    }

    // Drop the match: re-prepare (resets the grading ring to empty -- a fresh run) and drive a NON-matching
    // NOISE response. A poll over that window is a non-match, which CLEARS the session latch. Re-preparing
    // keeps the loaded reference; it just gives a clean, un-wrapped ring (the measurement-restart case).
    e.prepareForTest (fs, N);
    std::vector<float> noise ((size_t) (sweepLen + 255), 0.0f);
    unsigned seed = 99u;
    for (auto& v : noise) { seed = seed * 1664525u + 1013904223u; v = 0.4f * ((float) (seed >> 9) / (float) (1u << 23) - 0.5f); }
    driveResponseThroughSeam (e, noise, N);
    bool gNoise = false;
    matchPollTwiceAndPublish (e, ref, fs, gNoise, db);     // non-match -> clears the session latch, no grade
    CHECK_FALSE (gNoise);

    // A fresh matching sweep into the clean ring must re-arm and grade again (exactly once per match session).
    e.prepareForTest (fs, N);
    driveResponseThroughSeam (e, resp, N);
    bool graded2 = false;
    matchPollTwiceAndPublish (e, ref, fs, graded2, db);
    CHECK (graded2);                                       // re-armed: the new match session grades again
}

// ==================================================================================================
// 6b. BUG-2: the state does NOT revert to NotLearned when a measurement starts. setReferenceLoaded(true)
//     publishes Learned, but the prepare-callbacks seam (mirroring start()'s hm.prepare) used to RESET the
//     published state back to NotLearned — so during a run the state LIED (NotLearned despite a reference
//     loaded). The seam now re-publishes the honest state after hm.prepare: Learned with a reference,
//     NotLearned without.
// ==================================================================================================
TEST_CASE("Bug 2: state stays Learned (not NotLearned) after a run starts with a reference loaded") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;

    // A reference IS loaded before the run begins (the user learned one). Then the run starts (the seam runs
    // the same hm.prepare() that start() does). The published state must be Learned, NOT the NotLearned that
    // hm.prepare() resets to.
    e.setReferenceLoaded (true);
    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::Learned);   // honest right after learn
    e.prepareCallbacksForTest (fs, N, 8192);                                 // mirrors start(): hm.prepare + re-publish
    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::Learned);   // NOT reverted to NotLearned
    CHECK ((eb::RefMonState) e.refMonState() != eb::RefMonState::NotLearned);
}

TEST_CASE("Bug 2: state is NotLearned after a run starts with NO reference loaded") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;

    // No reference loaded (the default). After the run starts, the honest state is NotLearned.
    e.prepareCallbacksForTest (fs, N, 8192);
    CHECK ((eb::RefMonState) e.refMonState() == eb::RefMonState::NotLearned);
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
// 8. Fix 2: the IR thresholds stay PROVISIONAL (unratified); a clean measurement now grades CLEAN
//    (the redefined headphone IR-SNR — it falsely read "suspect" before the metric fix).
// ==================================================================================================
TEST_CASE("Fix 2: the IR thresholds are PROVISIONAL (unratified) so a clean measurement is info-only") {
    CHECK_FALSE (eb::kIrThresholdsRatified);
}

TEST_CASE("Fix 2: a clean measurement grades CLEAN at the default cutoff (redefined headphone IR-SNR)") {
    const int    n  = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (n, fs, ref, resp);
    const int m = (int) resp.size();

    auto g = eb::gradeMeasurement (ref.data(), resp.data(), m, fs);   // default kMinIrSnrDb = 6 dB
    CHECK (g.match.matched);
    CHECK (g.state == eb::RefMonState::GradedClean);   // the FIX: a clean measurement now clears the cutoff
    CHECK_FALSE (g.quality.lowQuality);
    CHECK (g.quality.irSnrDb > eb::kMinIrSnrDb);        // reads ABOVE the default cutoff (was falsely below)
    CHECK_FALSE (eb::kIrThresholdsRatified);            // thresholds still provisional (unratified)
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

// ==================================================================================================
// NEG-2 (Fix 1, engine-level wrap): drive a matching sweep WRITTEN ACROSS THE RING WRITE-HEAD (wrapped),
// then assert the published oldest->newest snapshot + the aligned poll still grade it. Before Fix 1 the gate
// read only the snapshot PREFIX; a wrapped sweep ends up near the snapshot's END (oldest->newest order), so
// the prefix-only gate would read coherence ~= 0 and never grade. The aligned poll locates it anywhere.
// ==================================================================================================
TEST_CASE("Live grade [NEG-2]: a sweep written ACROSS the ring wrap still snapshots + grades (aligned)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const int    sweepLen = 1 << 15;     // 32768
    const double fs = 48000.0;
    e.prepareForTest (fs, N);

    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);
    scaleToPeak (resp, 0.3f);

    e.setReferenceLoaded (true);

    std::vector<float> mono ((size_t) N, 0.0f), silence ((size_t) N, 0.0f);
    std::vector<float> scratch ((size_t) std::lround (fs * 28.0) + N, 0.0f);

    // 1) Advance the ring write-head to NEAR the end so the next (response) write WRAPS. The ring holds
    //    kGradingSeconds (28 s) = 1,344,000 samples at 48k; feed silence to within ~half a sweep of the end.
    const long long ringLen = (long long) std::lround (fs * 28.0);
    const long long respLen  = (long long) resp.size();
    const long long target   = ringLen - respLen / 2;      // head here -> the response straddles the wrap
    long long fed = 0;
    while (fed < target) {
        const int cnt = (int) std::min ((long long) N, target - fed);
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), cnt);
        if (e.gradingResponseReady()) e.copyGradingResponse (scratch.data(), (int) scratch.size());  // drain
        fed += cnt;
    }

    // 2) Feed the response: it crosses the ring end and wraps to the front.
    const int total = (int) resp.size();
    for (int off = 0; off < total; off += N) {
        const int cnt = std::min (N, total - off);
        std::vector<float> blk ((size_t) N, 0.0f);
        std::copy (resp.begin() + off, resp.begin() + off + cnt, blk.begin());
        e.processCaptureBlockForTest (blk.data(), silence.data(), mono.data(), cnt);
        if (e.gradingResponseReady()) e.copyGradingResponse (scratch.data(), (int) scratch.size());  // drain mid-sweep
    }

    // 3) Trailing silence so a COMPLETE-sweep snapshot is published (oldest->newest, sweep reassembled in order).
    //    Drain EVERY snapshot through ~700 blocks (@512/48k ~= 7.5 s) so the ring head advances and the (short,
    //    synthetic) sweep's reassembled START lands in the FIRST HALF of the next snapshot — mirroring production,
    //    where a 25 s sweep in a 28 s ring starts ~3 s in (well under the zero-padded FFT half-length).
    //    crossCorrelateAlign maps a lag past that half to a negative delay, which a real long sweep never reaches;
    //    we replicate that geometry so the synthetic short sweep behaves like the real one.
    for (int i = 0; i < 700; ++i) {
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);
        if (e.gradingResponseReady()) e.copyGradingResponse (scratch.data(), (int) scratch.size());  // always drain
    }
    // Now prime ONE fresh snapshot and leave it ready for the poll (do NOT drain this one).
    for (int i = 0; i < 80 && ! e.gradingResponseReady(); ++i)
        e.processCaptureBlockForTest (silence.data(), silence.data(), mono.data(), N);

    // 4) The aligned poll grades the wrapped-then-reassembled sweep (two consecutive matched polls).
    bool graded = false;
    PollDebounce db;
    auto g = matchPollTwiceAndPublish (e, ref, fs, graded, db);
    INFO ("wrapped-ring grade state = " << (int) g.state << "; coherence = " << e.lastMatchCoherence());
    CHECK (graded);
    const auto state = (eb::RefMonState) e.refMonState();
    CHECK ((state == eb::RefMonState::GradedClean || state == eb::RefMonState::GradedMarginal
            || state == eb::RefMonState::GradedSuspect));   // any quality verdict; no-noise-region synthetic window -> Marginal
    CHECK (state != eb::RefMonState::ReferenceStale);
    CHECK (e.lastMatchCoherence() > 0.0f);
}

// ==================================================================================================
// #30: the POSITIVE path of the RefMismatch / RefLowQuality guidance flags. The suite proved the flags
// stay CLEAR on a clean grade (CHECK_FALSE above) but never that a bad grade actually RAISES them —
// a deleted raiseRefMismatch()/raiseRefLowQuality() call would have kept every test green.
// ==================================================================================================
TEST_CASE("publishReferenceGrade: mismatch raises RefMismatch, guidance-only, cleared on prepare [#30]") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);
    REQUIRE_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));

    // The rate-guard / failed-match publish path: ReferenceStale with mismatch=true (either ear).
    e.publishReferenceGrade (1, (int) eb::RefMonState::ReferenceStale, 0.0f, 0.0f,
                             /*mismatch*/ true, /*lowQuality*/ false);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefLowQuality));
    CHECK (e.cleanCapture());   // GUIDANCE only: a bad reference grade never invalidates the capture

    // Re-prepare = a new run: the sticky guidance flag must not leak into it.
    e.prepareForTest (48000.0, 512);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
}

TEST_CASE("publishReferenceGrade: suspect quality raises RefLowQuality, guidance-only, cleared on prepare [#30]") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);
    REQUIRE_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefLowQuality));

    // The matched-but-suspect publish path: a graded verdict with lowQuality=true.
    e.publishReferenceGrade (0, (int) eb::RefMonState::GradedSuspect, 12.0f, 0.4f,
                             /*mismatch*/ false, /*lowQuality*/ true);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::RefLowQuality));
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefMismatch));
    CHECK (e.cleanCapture());   // GUIDANCE only

    e.prepareForTest (48000.0, 512);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::RefLowQuality));
}

// NOTE: the RT-safety / allocation-free assertion for the in-callback ring write lives in
// tests/test_audioengine_callbacks.cpp, which already owns the global operator new/delete counter
// harness (a program can only replace operator new ONCE).
