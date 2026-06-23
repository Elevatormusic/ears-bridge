#include <catch2/catch_test_macros.hpp>
#include "audio/Hysteresis.h"

// Generic, reusable anti-flicker hysteresis. Bands indexed by N ascending edges (>= lower-inclusive):
// index 0 = value < edges[0]; index i = [edges[i-1], edges[i]); index N = >= edges[N-1].

TEST_CASE ("hysteresisBand: raw hard bands when there is no history", "[hysteresis]") {
    const float edges[] = { 18.0f, 25.0f };   // 0=[<18] 1=[18,25) 2=[>=25]
    CHECK (eb::hysteresisBand (5.0f,  edges, 2, -1, 1.0f) == 0);
    CHECK (eb::hysteresisBand (18.0f, edges, 2, -1, 1.0f) == 1);   // >= lower-inclusive
    CHECK (eb::hysteresisBand (22.0f, edges, 2, -1, 1.0f) == 1);
    CHECK (eb::hysteresisBand (25.0f, edges, 2, -1, 1.0f) == 2);
    CHECK (eb::hysteresisBand (40.0f, edges, 2, -1, 1.0f) == 2);
}

TEST_CASE ("hysteresisBand: dead-band straddling each edge holds the previous band", "[hysteresis]") {
    const float edges[] = { 18.0f, 25.0f };
    // 25 edge, margin 1 -> dead-band [24, 26].
    CHECK (eb::hysteresisBand (24.9f, edges, 2, 2, 1.0f) == 2);   // from Green: held
    CHECK (eb::hysteresisBand (24.0f, edges, 2, 2, 1.0f) == 2);   // 24 not < 24 -> held
    CHECK (eb::hysteresisBand (23.9f, edges, 2, 2, 1.0f) == 1);   // < 24 -> drops to Orange
    CHECK (eb::hysteresisBand (25.1f, edges, 2, 1, 1.0f) == 1);   // from Orange: held (need >= 26)
    CHECK (eb::hysteresisBand (26.0f, edges, 2, 1, 1.0f) == 2);   // >= 26 -> Green
    // 18 edge -> dead-band [17, 19].
    CHECK (eb::hysteresisBand (18.5f, edges, 2, 0, 1.0f) == 0);   // from Red: held (need >= 19)
    CHECK (eb::hysteresisBand (19.0f, edges, 2, 0, 1.0f) == 1);   // -> Orange
    CHECK (eb::hysteresisBand (17.5f, edges, 2, 1, 1.0f) == 1);   // from Orange: held (need < 17)
    CHECK (eb::hysteresisBand (16.9f, edges, 2, 1, 1.0f) == 0);   // -> Red
}

TEST_CASE ("hysteresisBand: same band is a no-op; large jumps still pass", "[hysteresis]") {
    const float edges[] = { 18.0f, 25.0f };
    CHECK (eb::hysteresisBand (30.0f, edges, 2, 2, 1.0f) == 2);   // already Green
    CHECK (eb::hysteresisBand (40.0f, edges, 2, 0, 1.0f) == 2);   // Red->Green, clears edges[0]+margin
    CHECK (eb::hysteresisBand (2.0f,  edges, 2, 2, 1.0f) == 0);   // Green->Red, clears edges[1]-margin
}

TEST_CASE ("hysteresisBand: degenerate inputs are safe", "[hysteresis]") {
    CHECK (eb::hysteresisBand (10.0f, nullptr, 0, -1, 1.0f) == 0);
    const float one[] = { 5.0f };
    CHECK (eb::hysteresisBand (10.0f, one, 1, -1, 1.0f) == 1);
    CHECK (eb::hysteresisBand (1.0f,  one, 1, -1, 1.0f) == 0);
}

TEST_CASE ("BandHysteresis stateful wrapper smooths a flickering sequence", "[hysteresis]") {
    const float edges[] = { 18.0f, 25.0f };
    eb::BandHysteresis h (edges, 2, 1.0f);
    CHECK (h.band()        == -1);   // no history yet
    CHECK (h.update (28.0f) == 2);   // first -> raw Green
    CHECK (h.update (24.9f) == 2);   // flicker held Green
    CHECK (h.update (25.2f) == 2);   // still Green
    CHECK (h.update (23.5f) == 1);   // < 24 -> Orange
    CHECK (h.update (24.5f) == 1);   // held Orange (need >= 26 to climb)
    CHECK (h.band()        == 1);
    h.reset();
    CHECK (h.band()        == -1);   // history cleared
    CHECK (h.update (24.5f) == 1);   // fresh -> raw band for 24.5
}
