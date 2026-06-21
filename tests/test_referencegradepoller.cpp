// Headless unit tests for eb::ReferenceGradePoller — the grade-poll DECISION extracted from
// MainComponent::pollReferenceGrade() so the match + stable-match-debounce + grade logic can be verified
// WITHOUT the GUI or hardware. The "grade never fires" regression (it waited for silence) lived in that GUI
// method and could only be found by driving real hardware; these tests exercise the same decision in-process.
//
// The poller's contract (mirrors the GUI poll EXACTLY):
//   1. MATCH-GATE FIRST — referenceMatches publishes coherence/matched every poll (no absolute level gate).
//   2. STABLE-MATCH debounce — a grade fires once when the match holds across TWO consecutive polls; a dropped
//      match (!matched) re-arms the session so a fresh sweep can grade again.
//   3. gradeMeasurementWindow — runs once per match-session; a non-sweep / wrong-ref window -> ReferenceStale.
//
// Scenarios (the spec set): clean->grades, low-level->grades (no level gate), noisy->grades (noise-robust),
// non-sweep->no clean grade, wrong-ref->ReferenceStale, debounce->exactly one grade.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "audio/ReferenceGradePoller.h"

#include <vector>
#include <cmath>
#include <algorithm>

static constexpr double kPi = 3.14159265358979323846;

// --- Synthetic harness (mirrors tests/test_deconvolver.cpp / test_refmon_live.cpp) ----------------
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

// Scale a buffer so its PEAK |sample| equals targetPeak (so a test can place a matching response at a low level).
static void scaleToPeak (std::vector<float>& x, float targetPeak) {
    float pk = 0.0f;
    for (float v : x) pk = std::max (pk, std::abs (v));
    if (pk <= 0.0f) return;
    const float g = targetPeak / pk;
    for (float& v : x) v *= g;
}

// Deterministic white noise in [-amp, amp] (xorshift-ish LCG, matching test_refmon_live's seeded noise).
static void addNoise (std::vector<float>& x, float amp, unsigned seed) {
    for (auto& v : x) { seed = seed * 1664525u + 1013904223u; v += amp * ((float) (seed >> 9) / (float) (1u << 23) - 0.5f); }
}

// Build a clean (reference, response-window) pair from a single-tap room IR. The response is the room's
// response to the reference sweep; both are the same length (the full linear convolution) so the poller's
// window holds the sweep with a compact correlation lobe.
static void makeCleanMeasurement (int sweepLen, double fs,
                                  std::vector<float>& refOut, std::vector<float>& respOut) {
    auto ref = makeEss (sweepLen, fs);
    std::vector<float> roomIr ((size_t) 256, 0.0f);
    roomIr[50] = 1.0f;
    respOut = convolve (ref, roomIr);              // length sweepLen + 255
    refOut.assign (respOut.size(), 0.0f);
    std::copy (ref.begin(), ref.end(), refOut.begin());
}

using eb::RefMonState;

// ==================================================================================================
// CLEAN MATCH: a matching response grades on the second consecutive matched poll (the stable-match edge).
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a clean matching window grades on two consecutive matched polls") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);
    scaleToPeak (resp, 0.0562f);                   // ~-25 dBFS (the clean-but-modest level)
    addNoise (resp, 0.001f, 7u);                   // a touch of noise

    eb::ReferenceGradePoller p;
    // Poll 1: matched, but only one matched poll so far -> no grade yet (debounce).
    auto r1 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    CHECK (r1.matched);
    CHECK_FALSE (r1.didGrade);
    CHECK (r1.coherence > 0.0f);
    // Poll 2: matched on two consecutive polls -> grades.
    auto r2 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    CHECK (r2.matched);
    CHECK (r2.didGrade);
    CHECK ((r2.state == RefMonState::GradedClean || r2.state == RefMonState::GradedSuspect));
    CHECK (r2.state != RefMonState::ReferenceStale);
    CHECK_FALSE (r2.mismatch);
    CHECK (std::isfinite (r2.irSnrDb));
    CHECK (r2.coherence > 0.0f);
}

// ==================================================================================================
// LOW LEVEL: a far quieter (~-40 dBFS) matching window STILL grades — there is NO absolute level gate.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a low-level (~-40 dBFS) matching window still grades (no level gate)") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);
    scaleToPeak (resp, 0.01f);                      // -40 dBFS — far below any historical -24 dBFS arm

    eb::ReferenceGradePoller p;
    p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);   // poll 1: arm
    auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);   // poll 2: grade
    CHECK (r.didGrade);                              // the match fired the grade DESPITE the low level
    CHECK ((r.state == RefMonState::GradedClean || r.state == RefMonState::GradedSuspect));
    CHECK_FALSE (r.mismatch);
}

// ==================================================================================================
// NOISY: a matching window buried in heavy noise still grades (the match-gate is noise-robust). The
// coherence / IR-SNR are reported (info only) — what matters is that the grade fires and is not stale.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a noisy-but-matching window grades (match is noise-robust)") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);
    scaleToPeak (resp, 0.3f);                        // healthy sweep level
    addNoise (resp, 0.15f, 4242u);                   // heavy additive noise (~half the sweep peak)

    eb::ReferenceGradePoller p;
    auto r1 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    auto r2 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    CHECK (r1.matched);
    CHECK (r2.didGrade);                             // a grade still fires through the noise
    CHECK (r2.state != RefMonState::ReferenceStale); // and it is NOT a mismatch
    CHECK (std::isfinite (r2.irSnrDb));
}

// ==================================================================================================
// NON-SWEEP: a white-noise response (no sweep structure) must NOT grade clean. The match-gate has no
// compact correlation lobe to lock onto, so no stable match forms -> no grade fires.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a non-sweep (white-noise) window does NOT grade clean") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, ignore;
    makeCleanMeasurement (sweepLen, fs, ref, ignore);
    std::vector<float> resp ((size_t) (sweepLen + 255), 0.0f);
    addNoise (resp, 0.4f, 1234567u);                 // pure noise — no sweep

    eb::ReferenceGradePoller p;
    bool everGradedClean = false;
    for (int i = 0; i < 4; ++i) {                    // several polls — none should grade clean
        auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
        if (r.didGrade)
            everGradedClean = everGradedClean
                || (r.state == RefMonState::GradedClean || r.state == RefMonState::GradedSuspect);
    }
    CHECK_FALSE (everGradedClean);                   // never a clean/suspect grade from non-sweep noise
}

// ==================================================================================================
// WRONG REFERENCE: grading a window against a DIFFERENT sweep yields ReferenceStale (never clean). If the
// coarse detector locks (different sweeps can correlate loosely), the precise gate inside gradeMeasurementWindow
// rejects the aligned segment -> ReferenceStale; otherwise no grade fires. Either way: NOT graded clean.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a different-sweep window is never graded clean (ReferenceStale if it grades)") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    // The learned reference is a 20 Hz..20 kHz sweep; the measured window is the room response to a
    // DIFFERENT sweep (100 Hz..8 kHz) — a stale/wrong reference.
    auto refA  = makeEss (sweepLen, fs, 20.0,  20000.0);
    auto refB  = makeEss (sweepLen, fs, 100.0, 8000.0);
    std::vector<float> roomIr ((size_t) 256, 0.0f); roomIr[40] = 1.0f;
    auto resp = convolve (refB, roomIr);
    std::vector<float> ref (resp.size(), 0.0f);
    std::copy (refA.begin(), refA.end(), ref.begin());

    eb::ReferenceGradePoller p;
    bool everGradedClean = false;
    for (int i = 0; i < 4; ++i) {
        auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
        if (r.didGrade) {
            CHECK (r.state == RefMonState::ReferenceStale);   // a grade from a wrong ref is ALWAYS stale
            CHECK (r.mismatch);
            everGradedClean = everGradedClean
                || (r.state == RefMonState::GradedClean || r.state == RefMonState::GradedSuspect);
        }
    }
    CHECK_FALSE (everGradedClean);
}

// ==================================================================================================
// DEBOUNCE: a sustained match across MANY polls grades EXACTLY ONCE. The latch holds until the match drops
// (a non-match), which re-arms the session so a fresh sweep can grade again.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a sustained match grades exactly once (debounce)") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);
    scaleToPeak (resp, 0.3f);

    eb::ReferenceGradePoller p;
    int grades = 0;
    for (int i = 0; i < 8; ++i) {                    // eight sustained matched polls
        auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
        if (r.didGrade) ++grades;
    }
    CHECK (grades == 1);                             // exactly ONE grade across the sustained match

    // Drop the match with a non-sweep noise window: this re-arms the session (clears the latch).
    std::vector<float> noise ((size_t) resp.size(), 0.0f);
    addNoise (noise, 0.4f, 99u);
    for (int i = 0; i < 3; ++i)
        p.poll (noise.data(), (int) noise.size(), ref.data(), (int) ref.size(), fs);

    // A fresh sustained match must grade again — exactly once more.
    int grades2 = 0;
    for (int i = 0; i < 4; ++i) {
        auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
        if (r.didGrade) ++grades2;
    }
    CHECK (grades2 == 1);                            // re-armed: the next match session grades once more
}

// ==================================================================================================
// reset() clears the debounce, so the first match after a reset needs two polls again (not one).
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: reset() re-arms the two-poll debounce") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);
    scaleToPeak (resp, 0.3f);

    eb::ReferenceGradePoller p;
    p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);            // poll 1 (arm)
    auto r2 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);  // poll 2 (grade)
    CHECK (r2.didGrade);

    p.reset();   // a Start/Stop: a fresh run starts un-matched + un-graded
    auto a1 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    CHECK_FALSE (a1.didGrade);                       // first poll after reset: no grade (needs two again)
    auto a2 = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    CHECK (a2.didGrade);
}

// Guard: empty inputs never grade and never crash.
TEST_CASE("ReferenceGradePoller: empty window or reference never grades") {
    std::vector<float> ref, resp;
    makeCleanMeasurement (1 << 14, 48000.0, ref, resp);
    eb::ReferenceGradePoller p;
    auto r0 = p.poll (nullptr, 0, ref.data(), (int) ref.size(), 48000.0);
    CHECK_FALSE (r0.matched);
    CHECK_FALSE (r0.didGrade);
    auto r1 = p.poll (resp.data(), (int) resp.size(), nullptr, 0, 48000.0);
    CHECK_FALSE (r1.didGrade);
}
