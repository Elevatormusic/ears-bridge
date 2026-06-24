#include <catch2/catch_test_macros.hpp>
#include "audio/ScheduledEarRouter.h"
#include <cmath>
#include <limits>
using eb::Ear; using eb::SweepSchedule; using eb::ScheduledEarRouter; using eb::RouterOut;

namespace {
constexpr float F  = 0.003f;     // noise floor (linear)  -> onset gate 0.012, gap gate 0.006
constexpr float SW = 0.2f;       // a driven sweep block (loud: > 0.012)
constexpr float SI = 0.0001f;    // a hard-panned silent / gap block (quiet: < 0.006)
constexpr float AB = 0.010f;     // ACTIVE-but-above-floor: between the gap (0.006) and onset (0.012) gates

// The on-device-confirmed L,R,L burst (built directly so the router test does not depend on extractSchedule).
SweepSchedule lrl (double segSec = 5.0, double gapSec = 0.5) {
    SweepSchedule s;
    s.segments = { { Ear::Left, segSec }, { Ear::Right, segSec }, { Ear::Left, segSec } };
    s.gapsSec  = { gapSec, gapSec };
    s.valid = true;
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

TEST_CASE("ScheduledEarRouter: the duration-guard HOLDS the ear through an early quiet patch (and flags it)", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, SI, SI, 0.5);
    drive (r, SW, SI, 1.5);                          // 1.5 s into L
    auto held = drive (r, SI, SI, 0.5);              // a 0.5 s QUIET patch at ~1.5-2.0 s (well before durOk 85% = 4.25 s)
    CHECK (held.ear == 0);                           // STILL on L -> no premature gap (the guard held)
    CHECK (held.ambiguous);                          // a sustained quiet this early (< 70%) is anomalous -> flagged
    drive (r, SW, SI, 3.0);                          // resume + finish L (~5 s)
    auto g = drive (r, SI, SI, 0.5);                 // the real gap now transitions
    CHECK (g.ear == 1);
}

TEST_CASE("ScheduledEarRouter: non-finite / degenerate inputs stay sane (no crash, ear in {0,1})", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    const float nan = std::nanf ("");
    const float inf = std::numeric_limits<float>::infinity();
    auto a = r.process (nan, nan, nan, 0.01);   CHECK ((a.ear == 0 || a.ear == 1));   // NaN peaks + NaN floor
    auto b = r.process (inf, SI, 0.003f, 0.01); CHECK ((b.ear == 0 || b.ear == 1));   // Inf peak
    auto c = r.process (SW, SI, 0.0f,   0.01);  CHECK ((c.ear == 0 || c.ear == 1));   // zero floor -> default-floor guard
    auto d = r.process (SW, SI, 0.003f, 0.0);   CHECK ((d.ear == 0 || d.ear == 1));   // zero blockSec
    auto e = r.process (SW, SI, 0.003f, std::nanf ("")); CHECK ((e.ear == 0 || e.ear == 1));  // NaN blockSec
    // A NaN blockSec must NOT poison tSeg_: drive a normal L sweep, inject a NaN-blockSec block mid-segment,
    // then finish + a gap; the schedule must still advance L->R (a poisoned NaN tSeg_ would freeze it forever).
    ScheduledEarRouter r2; r2.loadSchedule (lrl());
    drive (r2, SI, SI, 0.5);
    drive (r2, SW, SI, 2.0);
    r2.process (SW, SI, 0.003f, std::nanf (""));     // the poison attempt, mid-segment
    drive (r2, SW, SI, 3.0);
    auto g = drive (r2, SI, SI, 0.5);
    CHECK (g.ear == 1);                              // advanced to R -> tSeg_ stayed finite (not frozen by NaN)
}

TEST_CASE("ScheduledEarRouter: steady non-panned background does NOT arm a segment (negative gate)", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl());
    drive (r, 0.05f, 0.05f, 6.0);          // 6 s of LOUD but NON-PANNED background (both ears equal, above the onset gate)
    auto after = drive (r, SI, SI, 0.5);   // then a gap
    CHECK (after.ear == 0);                // never armed -> still Idle on the first ear; did NOT advance to R
}

TEST_CASE("ScheduledEarRouter: a short learned gap (< kDwellSec) is still followed via the adaptive dwell", "[autoperear][router]") {
    ScheduledEarRouter r; r.loadSchedule (lrl (5.0, 0.1));   // 0.1 s gaps - shorter than the 0.15 s MAX dwell
    drive (r, SI, SI, 0.5);
    drive (r, SW, SI, 5.0);                // L
    drive (r, SI, SI, 0.1);                // a SHORT 0.1 s gap
    auto inR = drive (r, SI, SW, 1.0);     // R sweep
    CHECK (inR.ear == 1);                  // followed the short gap -> routed R (a fixed 0.15 s dwell would have missed it)
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
