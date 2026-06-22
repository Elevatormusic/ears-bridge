#include <catch2/catch_test_macros.hpp>
#include "audio/FreezeGate.h"

using eb::FreezeGateState;
using eb::freezeGateStep;

namespace {
constexpr int    kBlk  = 512;
constexpr double kRate = 48000.0;
constexpr float  kLoud = 0.5f;    // ~-6 dBFS — a real measurement sweep
constexpr float  kRoom = 0.03f;   // ~-30 dBFS — room/chain floor, below the -24 dBFS gate
}

TEST_CASE("freezeGate: a sustained loud sweep arms only after the attack debounce") {
    FreezeGateState s;
    // The first kFreezeAttackBlocks-1 loud blocks do NOT arm (a transient can't flip the freeze on).
    CHECK_FALSE (freezeGateStep (s, kLoud, kBlk, kRate));   // loudRun 1
    CHECK_FALSE (freezeGateStep (s, kLoud, kBlk, kRate));   // loudRun 2
    CHECK       (freezeGateStep (s, kLoud, kBlk, kRate));   // loudRun 3 -> armed
    CHECK       (freezeGateStep (s, kLoud, kBlk, kRate));   // stays on while loud
}

TEST_CASE("freezeGate: the room/chain floor never arms it") {
    FreezeGateState s;
    for (int i = 0; i < 200; ++i) CHECK_FALSE (freezeGateStep (s, kRoom, kBlk, kRate));
    CHECK_FALSE (s.on);
}

TEST_CASE("freezeGate: the release HOLDS across a short quiet gap (one ratio for both earcup sweeps)") {
    FreezeGateState s;
    for (int i = 0; i < 5; ++i) freezeGateStep (s, kLoud, kBlk, kRate);   // arm on the L sweep
    REQUIRE (s.on);
    // A ~2 s quiet inter-sweep gap (well under the 6 s release) must NOT drop the gate -> the R sweep that
    // follows reuses the SAME frozen ratio the L sweep snapshotted (no interaural group-delay mismatch).
    const int gapBlocks = (int) (2.0 * kRate / kBlk);
    for (int i = 0; i < gapBlocks; ++i) CHECK (freezeGateStep (s, 0.0f, kBlk, kRate));
    CHECK (s.on);
    // The R sweep re-charges the hold; still on.
    for (int i = 0; i < 5; ++i) CHECK (freezeGateStep (s, kLoud, kBlk, kRate));
}

TEST_CASE("freezeGate: sustained quiet past the release drops the gate (PI re-centers before the next run)") {
    FreezeGateState s;
    for (int i = 0; i < 5; ++i) freezeGateStep (s, kLoud, kBlk, kRate);
    REQUIRE (s.on);
    // Quiet longer than the 6 s release -> the gate releases so the bridge un-freezes and the PI loop recenters.
    const int quietBlocks = (int) (eb::kFreezeReleaseSecs * kRate / kBlk) + 5;
    bool on = true;
    for (int i = 0; i < quietBlocks; ++i) on = freezeGateStep (s, 0.0f, kBlk, kRate);
    CHECK_FALSE (on);
    CHECK_FALSE (s.on);
    CHECK (s.holdSamples == 0);
}

TEST_CASE("freezeGate: a momentary sub-attack blip does not arm, and resets the run") {
    FreezeGateState s;
    CHECK_FALSE (freezeGateStep (s, kLoud, kBlk, kRate));   // loudRun 1
    CHECK_FALSE (freezeGateStep (s, kLoud, kBlk, kRate));   // loudRun 2
    CHECK_FALSE (freezeGateStep (s, kRoom, kBlk, kRate));   // a dip below the gate before arming -> loudRun resets
    CHECK_FALSE (s.on);
    CHECK (s.loudRun == 0);
}
