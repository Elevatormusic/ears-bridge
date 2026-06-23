#include <catch2/catch_test_macros.hpp>
#include "audio/ScheduledEarRouter.h"
#include <cmath>
using eb::Ear; using eb::SweepSchedule; using eb::ScheduledEarRouter; using eb::RouterOut;

namespace {
constexpr float F  = 0.003f;     // noise floor (linear)  -> onset gate 0.012, gap gate 0.006
constexpr float SW = 0.2f;       // a driven sweep block (loud: > 0.012)
constexpr float SI = 0.0001f;    // a hard-panned silent / gap block (quiet: < 0.006)
constexpr float AB = 0.010f;     // ACTIVE-but-above-floor: between the gap (0.006) and onset (0.012) gates

// The on-device-confirmed L,R,L burst (built directly so the router test does not depend on extractSchedule).
SweepSchedule lrl (double segSec = 5.0) {
    SweepSchedule s;
    s.segments = { { Ear::Left, segSec }, { Ear::Right, segSec }, { Ear::Left, segSec } };
    s.gapsSec  = { 0.5, 0.5 };
    s.valid = true; s.totalActiveSec = 3.0 * segSec;
    return s;
}
// Feed `sec` of a constant per-ear peak; return the LAST RouterOut.
RouterOut drive (ScheduledEarRouter& r, float pL, float pR, double sec, double blk = 0.01) {
    RouterOut o { r.currentEar(), false };
    const int n = (int) std::lround (sec / blk);
    for (int i = 0; i < n; ++i) o = r.process (pL, pR, F, blk);
    return o;
}
}

TEST_CASE("ScheduledEarRouter: routes L,R,L predictively incl. the trailing L reference", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    REQUIRE (r.hasSchedule());
    drive (r, SI, SI, 0.5);                          // pre-measurement idle
    auto a  = drive (r, SW, SI, 5.0);                // L segment
    CHECK (a.ear == 0);   CHECK_FALSE (a.ambiguous);
    auto g1 = drive (r, SI, SI, 0.5);                // gap -> pre-position R
    CHECK (g1.ear == 1);
    auto b  = drive (r, SI, SW, 5.0);                // R segment
    CHECK (b.ear == 1);   CHECK_FALSE (b.ambiguous);
    auto g2 = drive (r, SI, SI, 0.5);                // gap -> pre-position the trailing L
    CHECK (g2.ear == 0);                             // <-- the bug-fix: route BACK to Left after R
    auto c  = drive (r, SW, SI, 5.0);                // trailing L reference segment
    CHECK (c.ear == 0);   CHECK_FALSE (c.ambiguous);
}

TEST_CASE("ScheduledEarRouter: a quiet LF onset rides the gap pre-position (captured on the right ear)", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, SI, SI, 0.5);
    drive (r, SW, SI, 5.0);                          // finish L
    drive (r, SI, SI, 0.3);                          // into the gap -> pre-positioned to R
    auto onset = drive (r, SI, AB, 0.2);             // R's quiet LF onset (above floor but not yet armed-loud)
    CHECK (onset.ear == 1);                          // already on R BEFORE the sweep ramps up -> onset captured
}

TEST_CASE("ScheduledEarRouter: the duration-guard HOLDS the ear through an early quiet patch", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, SI, SI, 0.5);
    drive (r, SW, SI, 1.5);                          // 1.5 s into L
    auto held = drive (r, SI, SI, 0.5);              // a 0.5 s QUIET patch at ~1.5-2.0 s (well before durOk 85% = 4.25 s)
    CHECK (held.ear == 0);                           // STILL on L -> no premature gap (the guard held)
    drive (r, SW, SI, 3.0);                          // resume + finish L (~5 s)
    auto g = drive (r, SI, SI, 0.5);                 // the real gap now transitions
    CHECK (g.ear == 1);
}

TEST_CASE("ScheduledEarRouter: a quiet-but-above-floor tail is NOT a gap (floor-tight)", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, SI, SI, 0.5);
    drive (r, SW, SI, 4.5);                          // L loud to 4.5 s (past durOk)
    auto tail = drive (r, AB, SI, 0.5);              // a quiet-but-above-floor HF tail (0.010, > the 0.006 gap gate)
    CHECK (tail.ear == 0);                           // reads ACTIVE, not a gap -> still on L, no transition to R
}

TEST_CASE("ScheduledEarRouter: a segment running far over schedule is flagged ambiguous (routing unchanged)", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, SI, SI, 0.5);
    auto over = drive (r, SW, SI, 7.0);              // L runs 7 s with no gap (> 5 s * 1.30 = 6.5 s)
    CHECK (over.ambiguous);
    CHECK (over.ear == 0);                           // ambiguity flags, it does NOT change the routed ear
}

TEST_CASE("ScheduledEarRouter: the WRONG ear sustainedly louder flags ambiguous (schedule keeps identity)", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, SI, SI, 0.5);
    drive (r, SW, SI, 1.0);                          // start L correctly
    auto inv = drive (r, SI, SW, 0.5);               // now R is loud while routing L (inverted) for 0.5 s (> 0.30 s)
    CHECK (inv.ambiguous);
    CHECK (inv.ear == 0);                            // still routes L: the loopback schedule is the identity authority
}

TEST_CASE("ScheduledEarRouter: no/invalid schedule -> fallback (hasSchedule false, never ambiguous)", "[autoperear][router]") {
    ScheduledEarRouter none;                          // nothing loaded
    CHECK_FALSE (none.hasSchedule());
    auto o = none.process (SW, SI, F, 0.01);
    CHECK_FALSE (o.ambiguous);

    SweepSchedule bad; bad.segments = { { Ear::Left, 5.0 } }; bad.valid = false;   // 1 segment / invalid
    ScheduledEarRouter r; r.loadSchedule (bad);
    CHECK_FALSE (r.hasSchedule());
    CHECK_FALSE (r.process (SW, SI, F, 0.01).ambiguous);
}
