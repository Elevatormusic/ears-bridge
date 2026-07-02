#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/SweepScheduleStore.h"
using Catch::Matchers::WithinAbs;
using eb::Ear; using eb::SweepSchedule;

namespace {
SweepSchedule lrl() {
    SweepSchedule s;
    s.segments = { { Ear::Left, 5.0 }, { Ear::Right, 5.0 }, { Ear::Left, 5.0 } };
    s.gapsSec  = { 0.5, 0.5 };
    s.valid = true;
    return s;
}
}

TEST_CASE("SweepScheduleStore: round-trips a schedule when the reference hash matches", "[autoperear][store]") {
    auto txt = eb::serializeSchedule (lrl(), "ABC123");
    auto s = eb::deserializeSchedule (txt, "ABC123");
    REQUIRE (s.valid);
    REQUIRE (s.segments.size() == 3);
    CHECK (s.segments[0].ear == Ear::Left);
    CHECK (s.segments[1].ear == Ear::Right);
    CHECK (s.segments[2].ear == Ear::Left);            // order incl. the trailing L preserved
    CHECK_THAT (s.segments[1].durationSec, WithinAbs (5.0, 1e-4));
    REQUIRE (s.gapsSec.size() == 2);
    CHECK_THAT (s.gapsSec[0], WithinAbs (0.5, 1e-4));
}

TEST_CASE("SweepScheduleStore: a MISMATCHED reference hash is rejected (stale guard)", "[autoperear][store]") {
    auto txt = eb::serializeSchedule (lrl(), "ABC123");
    auto s = eb::deserializeSchedule (txt, "DIFFERENT");   // the reference was re-learned -> different hash
    CHECK_FALSE (s.valid);                                  // not installed -> router falls back to the envelope
    CHECK (s.segments.empty());
}

TEST_CASE("SweepScheduleStore: empty current hash / bad text / single segment never validate", "[autoperear][store]") {
    auto txt = eb::serializeSchedule (lrl(), "ABC123");
    CHECK_FALSE (eb::deserializeSchedule (txt, "").valid);               // no reference present -> drop
    CHECK_FALSE (eb::deserializeSchedule ("garbage", "ABC123").valid);   // not v1
    CHECK_FALSE (eb::deserializeSchedule ("", "ABC123").valid);          // empty
    auto one = juce::String ("v1\nABC123\nseg 0 5.0\n");                 // only ONE segment
    CHECK_FALSE (eb::deserializeSchedule (one, "ABC123").valid);         // needs >= 2
}

// #70: a corrupt / hand-edited sidecar (garbage duration -> getDoubleValue 0.0, negative gap, or a gap count
// that doesn't match segments-1) must be REJECTED so the router falls back to the mic envelope, not wedge
// AutoPerEar into permanent "routing ambiguous".
TEST_CASE("SweepScheduleStore #70: rejects invalid durations, gaps, and gap-count", "[autoperear][store]") {
    CHECK_FALSE (eb::deserializeSchedule ("v1\nH\nseg 0 5.0\nseg 1 0\ngap 0.5\n",    "H").valid);  // zero duration
    CHECK_FALSE (eb::deserializeSchedule ("v1\nH\nseg 0 5.0\nseg 1 xyz\ngap 0.5\n",  "H").valid);  // non-numeric duration
    CHECK_FALSE (eb::deserializeSchedule ("v1\nH\nseg 0 5.0\nseg 1 5.0\ngap -0.5\n", "H").valid);  // negative gap
    CHECK_FALSE (eb::deserializeSchedule ("v1\nH\nseg 0 5.0\nseg 1 5.0\n",           "H").valid);  // 2 segs, 0 gaps
    CHECK       (eb::deserializeSchedule ("v1\nH\nseg 0 5.0\nseg 1 5.0\ngap 0.5\n",  "H").valid);  // clean pair
}
