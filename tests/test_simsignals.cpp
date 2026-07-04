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

// ==================================================================================================
// Impairment transforms (rig Task 2). Each is pinned in ISOLATION so a scenario failure later
// attributes to the pipeline, never to a mis-built impairment.
// ==================================================================================================
static ebsim::StereoTimeline probeTone (double freq, float amp, int n = 48000) {
    ebsim::StereoTimeline tl; tl.fs = 48000.0;
    tl.L.assign ((size_t) n, 0.0f); tl.R.assign ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
        tl.L[(size_t) i] = amp * (float) std::sin (2.0 * 3.14159265358979323846 * freq * i / 48000.0);
    return tl;
}
static double rmsOf (const std::vector<float>& x, int first, int last) {
    double acc = 0.0; for (int i = first; i < last; ++i) acc += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (acc / std::max (1, last - first));
}
// Goertzel single-bin magnitude (cheap spectral probe; avoids pulling an FFT into the sim tests).
static double goertzelMag (const std::vector<float>& x, double freq, double fs) {
    const int n = (int) x.size();
    const double w = 2.0 * 3.14159265358979323846 * freq / fs;
    const double c = 2.0 * std::cos (w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { s0 = (double) x[(size_t) i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / n;
}

TEST_CASE("SimSignals: flat crosstalk leaks the opposite ear at the stated level") {
    auto tl = probeTone (997.0, 0.5f);              // tone on L only
    ebsim::mixCrosstalk (tl, -15.0f);
    const double leakDb = 20.0 * std::log10 (rmsOf (tl.R, 0, (int) tl.R.size())
                                           / rmsOf (tl.L, 0, (int) tl.L.size()));
    CHECK (std::abs (leakDb - (-15.0)) < 1.0);
}

TEST_CASE("SimSignals: shaped crosstalk passes 6 kHz near the stated level but attenuates 500 Hz") {
    auto mid = probeTone (6000.0, 0.5f);            // in the 4-8 kHz emphasis band
    ebsim::mixCrosstalkShaped (mid, -15.0f);
    const double leakMid = 20.0 * std::log10 (rmsOf (mid.R, 4800, (int) mid.R.size())
                                            / rmsOf (mid.L, 4800, (int) mid.L.size()));
    CHECK (leakMid > -19.0);                        // near the stated peak level in-band
    auto low = probeTone (500.0, 0.5f);
    ebsim::mixCrosstalkShaped (low, -15.0f);
    const double leakLow = 20.0 * std::log10 (rmsOf (low.R, 4800, (int) low.R.size())
                                            / rmsOf (low.L, 4800, (int) low.L.size()));
    CHECK (leakLow < leakMid - 8.0);                // clearly shaped: out-of-band leak is much lower
}

TEST_CASE("SimSignals: applyDistortion adds a 2nd harmonic at the analytic a2 level") {
    auto tl = probeTone (997.0, 1.0f);              // FULL-SCALE drive (the pinned amplitude, RV)
    const double h2Before = goertzelMag (tl.L, 2.0 * 997.0, 48000.0);
    ebsim::applyDistortion (tl, 0.10f, 0.0f);       // ~5% THD at A=1 (H2 = a2*A^2/2)
    const double h1 = goertzelMag (tl.L, 997.0,       48000.0);
    const double h2 = goertzelMag (tl.L, 2.0 * 997.0, 48000.0);
    CHECK (h2Before < 1e-4);
    CHECK (std::abs (h2 / h1 - 0.05) < 0.01);       // the FFT-verified mapping from the research pass
}

TEST_CASE("SimSignals: HF droop attenuates 15 kHz strongly while 500 Hz stays put") {
    auto hi = probeTone (15000.0, 0.5f);
    const double hiBefore = goertzelMag (hi.L, 15000.0, 48000.0);
    ebsim::applyHfDroop (hi, 6000.0f);
    const double hiAfter = goertzelMag (hi.L, 15000.0, 48000.0);
    CHECK (20.0 * std::log10 (hiAfter / hiBefore) < -6.0);
    auto lo = probeTone (500.0, 0.5f);
    const double loBefore = goertzelMag (lo.L, 500.0, 48000.0);
    ebsim::applyHfDroop (lo, 6000.0f);
    const double loAfter = goertzelMag (lo.L, 500.0, 48000.0);
    CHECK (std::abs (20.0 * std::log10 (loAfter / loBefore)) < 1.5);
}

TEST_CASE("SimSignals: gain, clip, seeded noise and the coupler IR behave as specified") {
    auto tl = probeTone (997.0, 0.5f);
    ebsim::applyGainDb (tl, +6.02f);
    float pk = 0; for (float v : tl.L) pk = std::max (pk, std::abs (v));
    CHECK (std::abs (pk - 1.0f) < 0.02f);           // +6 dB doubles the 0.5 peak

    ebsim::applyGainDb (tl, +6.0f);                  // push past full scale...
    ebsim::clipHard (tl);
    pk = 0; for (float v : tl.L) pk = std::max (pk, std::abs (v));
    CHECK (pk == 1.0f);                              // ...and the rail clamps exactly

    ebsim::StereoTimeline n1, n2; n1.fs = n2.fs = 48000.0;
    n1.L.assign (4800, 0.0f); n1.R.assign (4800, 0.0f);
    n2.L.assign (4800, 0.0f); n2.R.assign (4800, 0.0f);
    ebsim::addSeededNoise (n1, 0.01f, 42u);
    ebsim::addSeededNoise (n2, 0.01f, 42u);
    CHECK (n1.L == n2.L);                            // deterministic
    CHECK (rmsOf (n1.L, 0, 4800) > 0.001);

    auto ir = probeTone (997.0, 0.5f);
    ebsim::convolveIr (ir);
    // The IR is a real frequency response: its tap comb spans ~2.5 periods of 997 Hz, so this probe
    // sits in a partial-cancellation notch (~-7 dB) - that is physics, not a defect. This sanity
    // bound only guards blow-up/vanish; the LOAD-BEARING pin is the match-gate mainLobe landing in
    // 0.2-0.4 through the real pipeline (Task 4's clean-session test).
    const double r = rmsOf (ir.L, 480, (int) ir.L.size());
    CHECK (r > 0.08); CHECK (r < 1.0);
}

// ==================================================================================================
// Task-6 impairments — pinned in ISOLATION (same rationale as above: a scenario failure attributes
// to the pipeline, never to a mis-built impairment).
// ==================================================================================================
TEST_CASE("SimSignals: addEcho sums a delayed copy at the stated gain and delay") {
    ebsim::StereoTimeline tl; tl.fs = 48000.0;
    tl.L.assign (48000, 0.0f); tl.R.assign (48000, 0.0f);
    tl.L[100] = 1.0f;                                // a lone impulse
    ebsim::addEcho (tl, 5.0, -10.0f);               // +240 samples, 0.316 linear
    const int d = (int) std::llround (5.0e-3 * 48000.0);
    CHECK (d == 240);
    CHECK (std::abs (tl.L[100] - 1.0f) < 1e-6);      // the original tap is untouched
    CHECK (std::abs (tl.L[(size_t) (100 + d)] - std::pow (10.0f, -10.0f / 20.0f)) < 1e-4);  // the echo tap
}

TEST_CASE("SimSignals: applyBrickwallLowpass zeroes above the cutoff and preserves below") {
    auto hi = probeTone (12000.0, 0.5f);
    const double hiBefore = goertzelMag (hi.L, 12000.0, 48000.0);
    ebsim::applyBrickwallLowpass (hi, 8000.0);      // 12 kHz is above the cut -> gutted
    const double hiAfter = goertzelMag (hi.L, 12000.0, 48000.0);
    CHECK (20.0 * std::log10 ((hiAfter + 1e-12) / hiBefore) < -40.0);   // a true brickwall cliff
    auto lo = probeTone (2000.0, 0.5f);
    const double loBefore = goertzelMag (lo.L, 2000.0, 48000.0);
    ebsim::applyBrickwallLowpass (lo, 8000.0);      // 2 kHz is below the cut -> passes
    const double loAfter = goertzelMag (lo.L, 2000.0, 48000.0);
    CHECK (std::abs (20.0 * std::log10 ((loAfter + 1e-12) / loBefore)) < 0.5);
}

TEST_CASE("SimSignals: invertPolarity negates exactly one channel") {
    auto tl = probeTone (997.0, 0.5f);
    tl.R = tl.L;                                     // both channels identical
    const auto before = tl.L;
    ebsim::invertPolarity (tl, 1);                   // negate R only
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK (tl.L[i] == before[i]);                // L untouched
        CHECK (tl.R[i] == -before[i]);               // R inverted
    }
}

TEST_CASE("SimSignals: addMainsHum plants base + 2f + 3f lines deterministically") {
    ebsim::StereoTimeline a, b; a.fs = b.fs = 48000.0;
    a.L.assign (48000, 0.0f); a.R.assign (48000, 0.0f);
    b.L.assign (48000, 0.0f); b.R.assign (48000, 0.0f);
    ebsim::addMainsHum (a, 60.0, 0.003f);
    ebsim::addMainsHum (b, 60.0, 0.003f);
    CHECK (a.L == b.L);                              // deterministic phases
    CHECK (goertzelMag (a.L, 60.0,  48000.0) > 0.002);    // fundamental present near amp
    CHECK (goertzelMag (a.L, 120.0, 48000.0) > 0.001);    // 2f
    CHECK (goertzelMag (a.L, 180.0, 48000.0) > 0.0005);   // 3f
    CHECK (goertzelMag (a.L, 300.0, 48000.0) < 1e-4);     // no 5th line (only 3 harmonics)
}

TEST_CASE("SimSignals: applyGainStepAt jumps the level from the step index onward") {
    auto tl = probeTone (997.0, 0.5f);
    ebsim::applyGainStepAt (tl, 0.5, +3.0f);         // step at 24000 samples
    const int at = (int) std::llround (0.5 * 48000.0);
    const double before = rmsOf (tl.L, 0, at);
    const double after  = rmsOf (tl.L, at, (int) tl.L.size());
    CHECK (std::abs (20.0 * std::log10 (after / before) - 3.0) < 0.2);   // +3 dB step
}
