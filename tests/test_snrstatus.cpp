#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "gui/SnrStatus.h"

// Sweep-to-Noise SNR data-quality check (Task 3). The verdict is PER-EAR (min of L/R so the good ear
// never vouches for the bad one), gated by a floor-confidence guard (honest silence off an untrusted
// floor), against a PROVISIONAL broadband peak-over-peak threshold. CLAUDE.md negative-test rule: the
// asymmetric pair (a high-SNR sweep must NOT warn, a low-SNR sweep must) is mandatory.

using eb::evaluateSnr;
using eb::snrNote;
using eb::kMinSweepSnrDb;

TEST_CASE("evaluateSnr: a high-SNR sweep does NOT warn (negative)") {
    // floor 0.003, peak 0.18 -> 20*log10(60) ~= 35.6 dB, well above the 20 dB threshold.
    auto v = evaluateSnr (0.003f, 0.18f, 0.18f, true);
    CHECK (v.floorTrusted);
    CHECK (v.snrDbMin == Catch::Approx (35.56f).margin (0.5f));
    CHECK_FALSE (v.lowSnr);
    CHECK (snrNote (v).isEmpty());          // a clean sweep leaves the label blank
}

TEST_CASE("evaluateSnr: a low-SNR sweep DOES warn (positive)") {
    // floor 0.03, peak 0.06 -> 20*log10(2) ~= 6 dB, below threshold.
    auto v = evaluateSnr (0.03f, 0.06f, 0.06f, true);
    CHECK (v.floorTrusted);
    CHECK (v.snrDbMin == Catch::Approx (6.02f).margin (0.5f));
    CHECK (v.lowSnr);
    // The note speaks and names the dB number (rounded), so the user can see how close to the floor it was.
    const auto note = snrNote (v);
    CHECK_FALSE (note.isEmpty());
    CHECK (note.contains ("6"));            // the rounded snrDbMin appears in the wording
    CHECK (note.containsIgnoreCase ("re-measure"));
}

TEST_CASE("evaluateSnr is PER-EAR: a good ear must NOT vouch for a bad one") {
    // L is a clean 0.18 (~15.6 dB over the 0.03 floor) but R is only 0.05 (~4.4 dB). min() must take R.
    auto v = evaluateSnr (0.03f, 0.18f, 0.05f, true);
    CHECK (v.snrDbL == Catch::Approx (15.56f).margin (0.5f));
    CHECK (v.snrDbR == Catch::Approx (4.44f).margin (0.5f));
    CHECK (v.snrDbMin == v.snrDbR);         // the min is the bad R ear, not the good L
    CHECK (v.lowSnr);                       // the bad ear drags the verdict low
}

TEST_CASE("evaluateSnr: an untrusted floor suppresses the warning (honest silence)") {
    // Identical numbers to the low-SNR case (would warn), but the pre-sweep window was NOT stable.
    auto v = evaluateSnr (0.03f, 0.06f, 0.06f, /*floorStable*/ false);
    CHECK_FALSE (v.floorTrusted);
    CHECK_FALSE (v.lowSnr);                 // never warn off a contaminated floor
    CHECK (snrNote (v).isEmpty());
}

TEST_CASE("evaluateSnr boundary: exactly at the threshold is NOT low (>= is fine)") {
    // Construct peak/floor so snrDbMin == kMinSweepSnrDb exactly: ratio = 10^(20/20) = 10.
    const float floor = 0.01f;
    const float peak  = floor * 10.0f;      // exactly +20 dB
    auto v = evaluateSnr (floor, peak, peak, true);
    CHECK (v.snrDbMin == Catch::Approx (kMinSweepSnrDb).margin (0.01f));
    CHECK_FALSE (v.lowSnr);                 // strictly-less-than threshold => the boundary is acceptable
    CHECK (snrNote (v).isEmpty());
    // One micro-step below the threshold DOES warn (confirms the comparison is the live edge).
    auto below = evaluateSnr (floor, floor * 9.0f, floor * 9.0f, true);   // ~19.08 dB
    CHECK (below.lowSnr);
}

TEST_CASE("evaluateSnr: never armed (armFloor==0) cannot warn") {
    // armNoiseFloor() returns 0 until the session has armed; floorTrusted requires armFloor > 0.
    auto v = evaluateSnr (0.0f, 0.06f, 0.06f, true);
    CHECK_FALSE (v.floorTrusted);
    CHECK_FALSE (v.lowSnr);                 // honest silence: no floor was ever captured
    CHECK (snrNote (v).isEmpty());
}

TEST_CASE("kMinSweepSnrDb is the documented provisional v1 threshold") {
    CHECK (kMinSweepSnrDb == Catch::Approx (20.0f));   // provisional, on-device ratification pending
}
