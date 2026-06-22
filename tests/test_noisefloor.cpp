#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/NoiseFloorMath.h"
#include <cmath>
using Catch::Matchers::WithinAbs;

TEST_CASE("isQuietBlock: below the ceiling is quiet, at/above is not") {
    CHECK (eb::isQuietBlock (0.003f, eb::kQuietCeilingLin));        // ~-50 dBFS quiet
    CHECK_FALSE (eb::isQuietBlock (0.5f, eb::kQuietCeilingLin));    // a loud sweep block
    CHECK_FALSE (eb::isQuietBlock (eb::kQuietCeilingLin, eb::kQuietCeilingLin)); // exactly at ceiling
    CHECK_FALSE (eb::isQuietBlock (std::nanf (""), eb::kQuietCeilingLin));       // non-finite is not quiet
}

TEST_CASE("robustLowFloor: median rejects a transient spike in an otherwise-quiet window") {
    float w[] = { 0.0031f, 0.0030f, 0.0032f, 0.5f, 0.0031f };   // one cough at 0.5
    CHECK_THAT (eb::robustLowFloor (w, 5), WithinAbs (0.0031f, 1e-4)); // median, not the spike
    CHECK (eb::robustLowFloor (nullptr, 0) == 0.0f);
}

TEST_CASE("blendFloor: slow EMA toward the candidate; uninitialized adopts it") {
    CHECK_THAT (eb::blendFloor (0.0f, 0.004f, eb::kFloorBlendAlpha), WithinAbs (0.004f, 1e-6)); // 0 -> adopt
    CHECK_THAT (eb::blendFloor (0.004f, 0.006f, 0.5f), WithinAbs (0.005f, 1e-6));               // halfway
}

TEST_CASE("averageFloorDb: power-mean of two channel floors, in dB") {
    CHECK_THAT (eb::averageFloorDb (0.01f, 0.01f), WithinAbs (-40.0f, 0.1f));   // equal -> -40 dBFS
    CHECK_THAT (eb::averageFloorDb (0.01f, 0.0f),  WithinAbs (-43.0f, 0.2f));   // one silent -> ~-3 dB
}

// ---- Task 2: NoiseFloorTracker ----
#include "audio/NoiseFloorTracker.h"

namespace {
// feed `blocks` blocks of constant per-ear level at 10 ms each
void feed (eb::NoiseFloorTracker& t, float lvlL, float lvlR, int blocks) {
    for (int i = 0; i < blocks; ++i) t.observeBlock (lvlL, lvlR, 0.010);
}
}

TEST_CASE("NoiseFloorTracker: invalid until a sustained quiet window, then baselines from silence") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    CHECK_FALSE (t.valid());
    feed (t, 0.0040f, 0.0030f, 10);   // 100 ms quiet -- not yet sustained (need >=500 ms)
    CHECK_FALSE (t.valid());
    feed (t, 0.0040f, 0.0030f, 50);   // now >500 ms of quiet -> baseline captured
    CHECK (t.valid());
    CHECK_THAT (t.floorLinear (0), WithinAbs (0.0040f, 5e-4));
    CHECK_THAT (t.floorLinear (1), WithinAbs (0.0030f, 5e-4));  // L/R independent
}

TEST_CASE("NoiseFloorTracker: a loud sweep does not corrupt the floor; gap refines it") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    feed (t, 0.0050f, 0.0050f, 60);   // pre-sweep silence -> baseline ~0.005
    REQUIRE (t.valid());
    feed (t, 0.4f, 0.4f, 60);         // a loud sweep -> NOT quiet -> floor unchanged
    CHECK_THAT (t.floorLinear (0), WithinAbs (0.0050f, 1e-3));
    feed (t, 0.0030f, 0.0030f, 60);   // inter-sweep gap, quieter -> floor refines downward (slowly)
    CHECK (t.floorLinear (0) < 0.0050f);
    CHECK (t.floorLinear (0) > 0.0030f);   // slow blend, not a jump to the new value
}

TEST_CASE("NoiseFloorTracker: averaged readout is the power-mean of the two channels") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    feed (t, 0.0100f, 0.0100f, 60);
    CHECK_THAT (t.floorDbAveraged(), WithinAbs (-40.0f, 0.5f));
}
