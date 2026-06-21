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
#include "gui/SnrStatus.h"   // eb::kMinSweepSnrDb — the GUI threshold the sweep-SNR predicate uses

#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>

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

// Build a (reference, window) pair where the sweep response sits at a GIVEN sample OFFSET inside a long window,
// with `leadNoise`-amplitude room noise filling the leading silence (and the tail). This is the rolling-ring
// SHAPE the poller actually receives on hardware (oldest->newest, the just-finished sweep near the END) — the
// old makeCleanMeasurement put the sweep at index 0 (the window PREFIX), which is exactly why every scenario
// passed despite the prefix-only match bug. The reference is the bare sweep at refLen; the window is `winLen`
// long with the room response written starting at `offset`.
static void makeOffsetMeasurement (int sweepLen, double fs, int offset, int winLen, float leadNoise,
                                   std::vector<float>& refOut, std::vector<float>& winOut, unsigned seed = 31u) {
    auto ref = makeEss (sweepLen, fs);
    std::vector<float> roomIr ((size_t) 256, 0.0f);
    roomIr[50] = 1.0f;
    auto resp = convolve (ref, roomIr);            // length sweepLen + 255 (the room response to the sweep)

    refOut.assign ((size_t) sweepLen, 0.0f);
    std::copy (ref.begin(), ref.end(), refOut.begin());

    winOut.assign ((size_t) winLen, 0.0f);
    if (leadNoise > 0.0f) addNoise (winOut, leadNoise, seed);   // room noise across the whole window first
    for (int i = 0; i < (int) resp.size(); ++i) {              // then drop the sweep response in at `offset`
        const int j = offset + i;
        if (j >= 0 && j < winLen) winOut[(size_t) j] += resp[(size_t) i];
    }
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

// ==================================================================================================
// NEG-1 (catches Fix 1): the sweep is placed LATE — in the final third of a long window, past refLen — with
// leading room noise filling the silence. The OLD gate matched only window[0..refLen) (the prefix), so it read
// coherence ~= 0 on a late sweep and NEVER graded. The aligned gate locates the sweep anywhere -> it matches
// and grades. This FAILS before Fix 1 (decide().matched would be false because the prefix held only noise).
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a LATE sweep (final third, past refLen) still matches and grades [NEG-1]") {
    const int    sweepLen = 1 << 15;                                   // refLen
    const double fs = 48000.0;
    // A window 3x the sweep length, with the sweep response dropped in at the END so it sits in the FINAL
    // THIRD — well past refLen, i.e. entirely OUTSIDE the old prefix-only match region (window[0..refLen)).
    // The leading two-thirds is low-level room noise (the rolling-ring shape: oldest noise -> newest sweep).
    const int winLen = sweepLen * 3;
    const int offset = winLen - (sweepLen + 256);                      // whole response near the END (~2/3 in)
    std::vector<float> ref, window;
    makeOffsetMeasurement (sweepLen, fs, offset, winLen, /*leadNoise*/ 0.002f, ref, window);

    eb::ReferenceGradePoller p;
    auto r1 = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);
    CHECK (r1.matched);                              // the LATE sweep is found (prefix-only match is gone)
    CHECK (r1.alignOffset > (int) ref.size());       // located well past the reference-length prefix
    auto r2 = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);
    CHECK (r2.didGrade);                             // and it grades on the second consecutive matched poll
    CHECK (r2.state != RefMonState::ReferenceStale);
}

// ==================================================================================================
// NEG-3: a window that is mostly silence with a short sweep at the VERY END must grade (not read as a
// non-match because the prefix is empty). Same root cause as NEG-1, minimal-lead variant.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: mostly-silence window with a sweep at the very END grades [NEG-3]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    // A window ~1.5x the sweep, sweep dropped at the very END over pure silence. (The cross-correlation maps a
    // lag past half the zero-padded FFT length to a NEGATIVE delay, which the production 28 s window — sweep in
    // its first half — never hits; this keeps the sweep in the positive-lag half while still exercising the
    // "empty prefix, sweep at the end" case the old prefix-only gate failed.)
    const int winLen = sweepLen + sweepLen / 2;                        // 1.5x sweep -> sweep-at-end stays < fftSize/2
    const int offset = winLen - (sweepLen + 256);                      // sweep at the very end
    std::vector<float> ref, window;
    makeOffsetMeasurement (sweepLen, fs, offset, winLen, /*leadNoise*/ 0.0f, ref, window);   // pure silence lead

    eb::ReferenceGradePoller p;
    auto r1 = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);
    auto r2 = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);
    CHECK (r1.matched);
    CHECK (r2.didGrade);
    CHECK (r2.state != RefMonState::ReferenceStale);
}

// ==================================================================================================
// NEG-5 (Fix 2): the rate guard. A response captured at a DIFFERENT sample rate than the reference must NOT be
// graded clean — it is the same chirp stretched, which can clear the match cutoffs and read a false "verified".
// The guard lives in the GUI poll (the poller is rate-agnostic: it sees one resampled stream), so we test the
// rate-compare condition that the GUI applies BEFORE calling decide(): differing rates -> do not grade.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a wrong-rate response is gated out before grading (rate guard) [NEG-5]") {
    // The GUI guard (MainComponent::pollReferenceGrade): grade only if the rates agree within 1 Hz.
    auto rateAllowsGrade = [] (double referenceRate, double responseRate) {
        return ! (referenceRate > 0.0 && responseRate > 0.0 && std::abs (responseRate - referenceRate) > 1.0);
    };
    CHECK_FALSE (rateAllowsGrade (48000.0, 44100.0));   // 44.1k response vs 48k reference -> NOT graded
    CHECK_FALSE (rateAllowsGrade (44100.0, 48000.0));   // and the reverse
    CHECK       (rateAllowsGrade (48000.0, 48000.0));   // matched rates -> grade allowed
    CHECK       (rateAllowsGrade (48000.0, 48000.5));   // within tolerance -> still allowed

    // And confirm the underlying hazard the guard prevents: a sweep matched against itself at the wrong rate
    // can still match the gate (so the guard, not the gate, is what stops the false grade). The reference is a
    // 48k sweep; the "response" is a sweep generated at 44.1k over the SAME sample count — same chirp, stretched.
    const int    sweepLen = 1 << 15;
    std::vector<float> ref, ignore;
    makeCleanMeasurement (sweepLen, 48000.0, ref, ignore);
    auto stretched = makeEss (sweepLen, 44100.0);       // the SAME N samples at a different rate -> stretched chirp
    std::vector<float> window (ref.size(), 0.0f);
    std::copy (stretched.begin(), stretched.end(), window.begin());
    auto m = eb::referenceMatches (ref.data(), window.data(), (int) std::min (ref.size(), window.size()));
    INFO ("wrong-rate match coherence=" << m.coherence << " matched=" << m.matched);
    // We do NOT assert m.matched either way (it is rate-dependent); the POINT is the rate guard catches this
    // case regardless, which the asserts above prove.
}

// ==================================================================================================
// NEG-7: a steady full-scale 1 kHz SINE (a pure tone, not a sweep) must NOT match. A sine's autocorrelation is
// sharp/periodic and could spuriously concentrate energy; the gate must still reject it (it is not THIS sweep).
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a full-scale 1 kHz sine (not a sweep) does not match [NEG-7]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, ignore;
    makeCleanMeasurement (sweepLen, fs, ref, ignore);

    std::vector<float> window (ref.size(), 0.0f);
    for (int i = 0; i < (int) window.size(); ++i)
        window[(size_t) i] = (float) std::sin (2.0 * kPi * 1000.0 * (double) i / fs);   // steady full-scale tone

    eb::ReferenceGradePoller p;
    auto d = p.decide (window.data(), (int) window.size(), ref.data(), (int) ref.size());
    CHECK_FALSE (d.matched);                         // a tone is not the sweep -> no match
    bool everGradedClean = false;
    for (int i = 0; i < 4; ++i) {
        auto r = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);
        if (r.didGrade)
            everGradedClean = everGradedClean
                || (r.state == RefMonState::GradedClean || r.state == RefMonState::GradedSuspect);
    }
    CHECK_FALSE (everGradedClean);
}

// ==================================================================================================
// NEG-8 (Fix 3): a window carrying NaN/Inf samples must NOT match, must not crash, and must not read
// coherence=1.0 (the degenerate restRms==0 false-match). The poller rejects non-finite content up front.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a NaN/Inf window never matches (no crash, no coherence=1.0) [NEG-8]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);   // a real matching response...
    scaleToPeak (resp, 0.3f);
    resp[resp.size() / 2]     = std::numeric_limits<float>::quiet_NaN();   // ...corrupted with a NaN
    resp[resp.size() / 3]     = std::numeric_limits<float>::infinity();    // ...and an Inf

    eb::ReferenceGradePoller p;
    auto d = p.decide (resp.data(), (int) resp.size(), ref.data(), (int) ref.size());
    CHECK_FALSE (d.matched);                          // non-finite content is never a match
    CHECK (d.coherence < 1.0f);                       // and never the degenerate coherence=1.0
    auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);
    CHECK_FALSE (r.didGrade);                          // no grade fires from a NaN/Inf window
}

// The GUI predicate that raises the LowSnr guidance flag (MainComponent::pollReferenceGrade): flag iff the SNR
// is VALID and the sweep ran below the provisional threshold. Mirror it so the tests assert the exact decision.
static bool wouldRaiseLowSnr (const eb::GradePollResult& r) {
    return r.sweepSnrValid && r.sweepSnrDb < eb::kMinSweepSnrDb;
}

// ==================================================================================================
// SNR-CLEAN (the match-window sweep-SNR fix): a LOUD sweep over QUIET leading room noise grades with a HIGH
// sweepSnrDb and the LowSnr predicate is FALSE. The SNR is computed from the SAME aligned window the grade
// keyed off (sweep region vs leading-noise region) — it fires on a real sweep WITHOUT the dead level arm.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a clean (loud sweep, quiet noise) window has high sweep SNR, LowSnr NOT raised [SNR-CLEAN]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    const int    winLen = sweepLen * 3;
    const int    offset = sweepLen;                     // a full sweepLen of LEADING room noise before the sweep
    std::vector<float> ref, window;
    // Loud sweep (peak ~0.3 after the room IR) over VERY quiet leading noise (~-66 dBFS) -> a large sweep/noise ratio.
    makeOffsetMeasurement (sweepLen, fs, offset, winLen, /*leadNoise*/ 0.0005f, ref, window);

    eb::ReferenceGradePoller p;
    p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);            // poll 1: arm
    auto r = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);   // poll 2: grade
    REQUIRE (r.didGrade);
    CHECK (r.sweepSnrValid);                            // a usable leading-noise region was present
    CHECK (r.sweepSnrDb > eb::kMinSweepSnrDb);          // the loud sweep sits well above the quiet floor
    CHECK_FALSE (wouldRaiseLowSnr (r));                 // -> the GUI does NOT raise LowSnr
}

// ==================================================================================================
// SNR-NOISY: a sweep only a FEW dB above a LOUD leading-noise region grades, but its sweepSnrDb is LOW
// (< kMinSweepSnrDb) and the LowSnr predicate is TRUE — exactly the user-reported "noisy but read clean"
// case the dead level arm missed. The grade itself still fires (the match-gate is noise-robust).
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a noisy window (sweep barely above loud noise) has low sweep SNR -> LowSnr predicate true [SNR-NOISY]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    const int    winLen = sweepLen * 3;
    const int    offset = sweepLen;                     // a full sweepLen of LEADING room noise before the sweep
    std::vector<float> ref, window;
    // LOUD leading noise under a sweep whose room-IR response peaks ~0 dBFS: an ESS spreads its energy over time,
    // so its RMS is far below its peak — with noise this loud the sweep RMS sits only a few dB over the noise RMS,
    // a low sweep-to-noise SNR (a genuinely noisy capture that must be flagged). leadNoise tuned so sweepSnrDb sits
    // a few dB BELOW kMinSweepSnrDb while the match-gate still locks (it is noise-robust).
    makeOffsetMeasurement (sweepLen, fs, offset, winLen, /*leadNoise*/ 0.30f, ref, window);

    eb::ReferenceGradePoller p;
    p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);            // poll 1: arm
    auto r = p.poll (window.data(), (int) window.size(), ref.data(), (int) ref.size(), fs);   // poll 2: grade
    REQUIRE (r.didGrade);                               // the match still fires through the noise
    CHECK (r.state != RefMonState::ReferenceStale);
    CHECK (r.sweepSnrValid);
    CHECK (r.sweepSnrDb < eb::kMinSweepSnrDb);          // the sweep ran too close to the loud room floor
    CHECK (wouldRaiseLowSnr (r));                       // -> the GUI raises the LowSnr guidance flag
}

// ==================================================================================================
// SNR-DEGENERATE: the sweep is at offset 0 with NO usable leading noise AND no trailing silence. The SNR has
// no floor to measure against, so it is marked INVALID and the LowSnr predicate is FALSE — no false flag, no
// divide-by-~0. The grade itself still fires; only the SNR side-channel abstains.
// ==================================================================================================
TEST_CASE("ReferenceGradePoller: a degenerate window (sweep at offset 0, no noise region) yields invalid SNR -> no false flag [SNR-DEGENERATE]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);     // sweep at index 0; window length == refLen (no lead, no trail)
    scaleToPeak (resp, 0.3f);

    eb::ReferenceGradePoller p;
    p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);            // poll 1: arm
    auto r = p.poll (resp.data(), (int) resp.size(), ref.data(), (int) ref.size(), fs);   // poll 2: grade
    REQUIRE (r.didGrade);                               // the grade still fires
    CHECK_FALSE (r.sweepSnrValid);                      // but there is no noise region to measure against
    CHECK_FALSE (wouldRaiseLowSnr (r));                 // -> the GUI does NOT raise LowSnr (no false positive)
}

// Direct unit check of computeSweepSnr's edge-case fallback: when the LEADING region is too short but a longer
// TRAILING-silence region exists, the SNR falls back to it and stays valid (the documented edge case).
TEST_CASE("ReferenceGradePoller::computeSweepSnr falls back to trailing silence when the lead is too short [SNR-FALLBACK]") {
    const int    sweepLen = 1 << 15;
    const double fs = 48000.0;
    std::vector<float> ref, resp;
    makeCleanMeasurement (sweepLen, fs, ref, resp);     // resp == the room response to the sweep
    scaleToPeak (resp, 0.3f);
    const int respLen = (int) resp.size();
    // A window with the sweep at offset 0 (lead length 0) but a LONG trailing-silence tail with faint noise.
    const int winLen = respLen + sweepLen;              // a full sweepLen of trailing room after the sweep
    std::vector<float> window ((size_t) winLen, 0.0f);
    addNoise (window, 0.0005f, 77u);                    // faint room noise across the whole window
    for (int i = 0; i < respLen; ++i) window[(size_t) i] += resp[(size_t) i];   // sweep at the front

    eb::GradePollResult g;
    eb::ReferenceGradePoller::computeSweepSnr (window.data(), winLen, /*alignOffset*/ 0, respLen, g);
    CHECK (g.sweepSnrValid);                            // the trailing-silence fallback supplied a floor
    CHECK (g.sweepSnrDb > eb::kMinSweepSnrDb);          // loud sweep over faint trailing noise -> high SNR
}
