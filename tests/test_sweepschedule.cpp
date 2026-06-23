#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/SweepSchedule.h"
#include <vector>
#include <cmath>
using Catch::Matchers::WithinAbs;
using eb::Ear;

namespace {
constexpr double kRate    = 48000.0;
constexpr float  kSweepA  = 0.178f;    // ~-15 dBFS tone (RMS ~-18 dBFS) -> the driven sweep
constexpr float  kSilent  = 1.0e-6f;   // ~-120 dBFS -> the hard-panned silent channel
constexpr float  kToneA   = 0.010f;    // ~-40 dBFS -> a STEREO level tone (active, not hard-panned)

// Append `sec` of a 1 kHz tone at the given per-channel amplitudes (hard-panned when one side is kSilent).
void region (std::vector<float>& L, std::vector<float>& R, double sec, float aL, float aR) {
    const int n = (int) std::lround (sec * kRate);
    for (int i = 0; i < n; ++i) {
        const float ph = (float) (2.0 * 3.14159265358979323846 * 1000.0 * (double) i / kRate);
        L.push_back (aL * std::sin (ph));
        R.push_back (aR * std::sin (ph));
    }
}
void sweep (std::vector<float>& L, std::vector<float>& R, Ear ear, double sec) {
    if (ear == Ear::Left) region (L, R, sec, kSweepA, kSilent);
    else                  region (L, R, sec, kSilent, kSweepA);
}
void gap       (std::vector<float>& L, std::vector<float>& R, double sec) { region (L, R, sec, kSilent, kSilent); }
void levelTone (std::vector<float>& L, std::vector<float>& R, double sec) { region (L, R, sec, kToneA,  kToneA);  }

eb::SweepSchedule run (const std::vector<float>& L, const std::vector<float>& R) {
    return eb::extractSchedule (L.data(), R.data(), (int) L.size(), kRate);
}
}

TEST_CASE("extractSchedule: confirmed L,R,L model (on-device burst) is recovered", "[autoperear][schedule]") {
    std::vector<float> L, R;
    sweep (L, R, Ear::Left, 5.0); gap (L, R, 0.5);
    sweep (L, R, Ear::Right, 5.0); gap (L, R, 0.5);
    sweep (L, R, Ear::Left, 5.0);
    auto s = run (L, R);
    REQUIRE (s.valid);
    REQUIRE (s.segments.size() == 3);
    CHECK (s.segments[0].ear == Ear::Left);
    CHECK (s.segments[1].ear == Ear::Right);
    CHECK (s.segments[2].ear == Ear::Left);            // the trailing L reference is captured, not dropped
    CHECK_THAT (s.segments[0].durationSec, WithinAbs (5.0, 0.2));
    CHECK_THAT (s.segments[1].durationSec, WithinAbs (5.0, 0.2));
    CHECK_THAT (s.segments[2].durationSec, WithinAbs (5.0, 0.2));
    REQUIRE (s.gapsSec.size() == 2);
    CHECK_THAT (s.gapsSec[0], WithinAbs (0.5, 0.15));
    CHECK_THAT (s.gapsSec[1], WithinAbs (0.5, 0.15));
}

TEST_CASE("extractSchedule: R-first burst is recovered as R,L,R", "[autoperear][schedule]") {
    std::vector<float> L, R;
    sweep (L, R, Ear::Right, 4.0); gap (L, R, 0.5);
    sweep (L, R, Ear::Left, 4.0); gap (L, R, 0.5);
    sweep (L, R, Ear::Right, 4.0);
    auto s = run (L, R);
    REQUIRE (s.segments.size() == 3);
    CHECK (s.segments[0].ear == Ear::Right);
    CHECK (s.segments[1].ear == Ear::Left);
    CHECK (s.segments[2].ear == Ear::Right);
}

TEST_CASE("extractSchedule: a pre-sweep STEREO level tone is ignored, not counted as a segment", "[autoperear][schedule]") {
    std::vector<float> L, R;
    levelTone (L, R, 1.0); gap (L, R, 0.5);            // Dirac's level-set tone: active but |L-R|<6 dB
    sweep (L, R, Ear::Left, 5.0); gap (L, R, 0.5);
    sweep (L, R, Ear::Right, 5.0);
    auto s = run (L, R);
    REQUIRE (s.segments.size() == 2);                  // ONLY the two hard-panned sweeps
    CHECK (s.segments[0].ear == Ear::Left);
    CHECK (s.segments[1].ear == Ear::Right);
}

TEST_CASE("extractSchedule: a sub-minSegment hard-panned blip is skipped", "[autoperear][schedule]") {
    std::vector<float> L, R;
    sweep (L, R, Ear::Left, 5.0); gap (L, R, 0.5);
    sweep (L, R, Ear::Right, 1.0); gap (L, R, 0.5);    // a 1 s blip (< 2 s minSegment) -> dropped
    sweep (L, R, Ear::Left, 5.0);
    auto s = run (L, R);
    REQUIRE (s.segments.size() == 2);
    CHECK (s.segments[0].ear == Ear::Left);
    CHECK (s.segments[1].ear == Ear::Left);            // the R blip is gone; both survivors are L
}

TEST_CASE("extractSchedule: durations are LEARNED, not hardcoded (3s and 6s configs both recover)", "[autoperear][schedule]") {
    std::vector<float> L3, R3, L6, R6;
    sweep (L3, R3, Ear::Left, 3.0); gap (L3, R3, 0.5); sweep (L3, R3, Ear::Right, 3.0); gap (L3, R3, 0.5); sweep (L3, R3, Ear::Left, 3.0);
    sweep (L6, R6, Ear::Left, 6.0); gap (L6, R6, 0.5); sweep (L6, R6, Ear::Right, 6.0); gap (L6, R6, 0.5); sweep (L6, R6, Ear::Left, 6.0);
    auto s3 = run (L3, R3); auto s6 = run (L6, R6);
    REQUIRE (s3.segments.size() == 3);
    REQUIRE (s6.segments.size() == 3);
    CHECK_THAT (s3.segments[0].durationSec, WithinAbs (3.0, 0.2));   // learned the short config
    CHECK_THAT (s6.segments[0].durationSec, WithinAbs (6.0, 0.2));   // learned the long config
}

TEST_CASE("extractSchedule: a single segment / all-silent buffer is invalid (-> fallback)", "[autoperear][schedule]") {
    std::vector<float> L, R;
    sweep (L, R, Ear::Left, 5.0);                      // one segment only
    auto s = run (L, R);
    CHECK_FALSE (s.valid);
    CHECK (s.segments.size() == 1);

    std::vector<float> Lz, Rz; gap (Lz, Rz, 6.0);      // all silent
    auto z = run (Lz, Rz);
    CHECK_FALSE (z.valid);
    CHECK (z.segments.empty());

    auto nul = eb::extractSchedule (nullptr, nullptr, 0, kRate);   // bad args
    CHECK_FALSE (nul.valid);
}
