#include <catch2/catch_test_macros.hpp>
#include "sim/SimSignals.h"
#include "audio/SweepSchedule.h"
#include <cmath>

// The synthetic-measurement-rig signal layer (spec: 2026-07-02-synthetic-measurement-rig-design).
// These pin the VIRTUAL DIRAC SESSION's shape against the on-device-confirmed model (L,R,trailing-L
// hard-panned sweeps, leading stereo level tone) and — crucially — that the rig's session and the
// PRODUCTION schedule learner agree about what was generated (rig <-> pipeline agreement).

TEST_CASE("SimSignals: the virtual session has the confirmed L,R,L hard-panned shape") {
    ebsim::SessionSpec spec; ebsim::SessionTruth truth;
    auto tl = ebsim::makeDiracSession (spec, truth);
    REQUIRE (tl.L.size() == tl.R.size());
    REQUIRE (truth.segments.size() == 3);
    CHECK (truth.segments[0].ear == 0);
    CHECK (truth.segments[1].ear == 1);
    CHECK (truth.segments[2].ear == 0);

    // Hard-panned: during the L sweep the R channel is DIGITAL SILENCE (and vice versa).
    const auto& s0 = truth.segments[0];
    float maxR = 0.0f;
    for (int i = s0.start; i < s0.start + s0.len; ++i) maxR = std::max (maxR, std::abs (tl.R[(size_t) i]));
    CHECK (maxR == 0.0f);
    const auto& s1 = truth.segments[1];
    float maxL = 0.0f;
    for (int i = s1.start; i < s1.start + s1.len; ++i) maxL = std::max (maxL, std::abs (tl.L[(size_t) i]));
    CHECK (maxL == 0.0f);

    // The level tone is on BOTH channels at ~-40 dBFS (must be IGNORED by the schedule learner).
    float maxToneL = 0.0f, maxToneR = 0.0f;
    for (int i = truth.toneStart; i < truth.toneStart + truth.toneLen; ++i) {
        maxToneL = std::max (maxToneL, std::abs (tl.L[(size_t) i]));
        maxToneR = std::max (maxToneR, std::abs (tl.R[(size_t) i]));
    }
    CHECK (maxToneL > 0.005f); CHECK (maxToneL < 0.02f);
    CHECK (maxToneR > 0.005f); CHECK (maxToneR < 0.02f);

    // The sweep drive sits at the spec peak (~-6 dBFS default).
    float maxSweep = 0.0f;
    for (int i = s0.start; i < s0.start + s0.len; ++i) maxSweep = std::max (maxSweep, std::abs (tl.L[(size_t) i]));
    CHECK (maxSweep > 0.4f); CHECK (maxSweep <= 0.51f);
}

TEST_CASE("SimSignals: extractSchedule recovers the session's own schedule (rig-production agreement)") {
    ebsim::SessionSpec spec; ebsim::SessionTruth truth;
    auto tl = ebsim::makeDiracSession (spec, truth);
    auto sched = eb::extractSchedule (tl.L.data(), tl.R.data(), (int) tl.L.size(), spec.fs);
    REQUIRE (sched.valid);
    REQUIRE (sched.segments.size() == 3);
    CHECK (sched.segments[0].ear == eb::Ear::Left);
    CHECK (sched.segments[1].ear == eb::Ear::Right);
    CHECK (sched.segments[2].ear == eb::Ear::Left);
    CHECK (std::abs (sched.segments[0].durationSec - spec.sweepSeconds) < 0.3);
    REQUIRE (sched.gapsSec.size() == 2);
    CHECK (std::abs (sched.gapsSec[0] - spec.gapSeconds) < 0.3);
}

TEST_CASE("SimSignals: contentPpm scales the sweep duration by exact phase math") {
    ebsim::SessionSpec spec; ebsim::SessionTruth a, b;
    ebsim::makeDiracSession (spec, a, 0.0);
    ebsim::makeDiracSession (spec, b, +400.0);   // exaggerated so the length delta is clearly integral
    REQUIRE (a.segments.size() == 3); REQUIRE (b.segments.size() == 3);
    const double ratio = (double) b.segments[0].len / (double) a.segments[0].len;
    CHECK (std::abs (ratio - 1.0004) < 5e-5);
}
