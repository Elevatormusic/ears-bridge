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

TEST_CASE("blendFloor: adopts when uninitialized; fast DOWN, slow UP (resists upward ratchet)") {
    CHECK_THAT (eb::blendFloor (0.0f, 0.004f, eb::kFloorBlendAlpha), WithinAbs (0.004f, 1e-6)); // 0 -> adopt
    CHECK_THAT (eb::blendFloor (0.006f, 0.004f, 0.5f), WithinAbs (0.005f, 1e-6));   // DOWN: halfway at alpha
    const float up = eb::blendFloor (0.004f, 0.006f, 0.5f);                        // UP: throttled by kFloorRiseFactor
    CHECK (up > 0.004f);
    CHECK (up < 0.0042f);   // 0.004 + 0.5*0.1*(0.002) = 0.0041, NOT 0.005
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

// ---- regression: the two MAJORs the fresh-context verifier caught ----

TEST_CASE("NoiseFloorTracker: a noisier (sub-ceiling) gap does NOT ratchet the floor up much") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    feed (t, 0.0030f, 0.0030f, 60);            // baseline ~0.003
    REQUIRE (t.valid());
    const float base = t.floorLinear (0);
    feed (t, 0.0100f, 0.0100f, 60);            // noisier but still below the 0.0631 ceiling
    CHECK (t.floorLinear (0) < base + 0.0010f);  // up-throttled: rose <1e-3, NOT toward 0.010
}

TEST_CASE("NoiseFloorTracker: folds correctly when the sustain window exceeds the ring cap (small buffers)") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 64);
    // 64 samples @ 48k ~ 1.33 ms -> ~376 quiet blocks for 0.5 s, exceeding the 256-entry ring
    for (int i = 0; i < 400; ++i) t.observeBlock (0.0040f, 0.0040f, 64.0 / 48000.0);
    CHECK (t.valid());
    CHECK_THAT (t.floorLinear (0), WithinAbs (0.0040f, 5e-4));   // constant level -> ring median exact
}

TEST_CASE("NoiseFloorTracker: an all-NaN run never produces a NaN floor and never folds") {
    eb::NoiseFloorTracker t; t.prepare (48000.0, 480);
    for (int i = 0; i < 60; ++i) t.observeBlock (std::nanf (""), std::nanf (""), 0.010);
    CHECK_FALSE (t.valid());                       // non-finite is never quiet -> never folds
    CHECK (std::isfinite (t.floorLinear (0)));     // stays the finite reset value
    CHECK (t.floorLinear (0) == 0.0f);
}
