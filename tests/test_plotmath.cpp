#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "gui/PlotMath.h"
#include <cmath>

using Catch::Matchers::WithinAbs;

// Plot rect: x in [0,W], freq log-mapped 20 Hz -> 20 kHz; y in [0,H], dB top->bottom.
TEST_CASE("freqToX maps the band endpoints to the plot edges") {
    const float W = 300.0f;
    CHECK_THAT (eb::freqToX (20.0f,    W), WithinAbs (0.0f,   1e-3));
    CHECK_THAT (eb::freqToX (20000.0f, W), WithinAbs (300.0f, 1e-3));
}

TEST_CASE("freqToX is logarithmic: a decade is a fixed fraction") {
    const float W = 300.0f;
    // 20 -> 20000 spans 3 decades. 200 Hz is exactly 1 decade up -> 1/3 of width.
    CHECK_THAT (eb::freqToX (200.0f, W),  WithinAbs (100.0f, 1e-2));
    // 2000 Hz is 2 decades up -> 2/3 of width.
    CHECK_THAT (eb::freqToX (2000.0f, W), WithinAbs (200.0f, 1e-2));
}

TEST_CASE("freqToX clamps out-of-band input to the edges") {
    const float W = 300.0f;
    CHECK_THAT (eb::freqToX (5.0f,     W), WithinAbs (0.0f,   1e-3)); // below 20 Hz
    CHECK_THAT (eb::freqToX (40000.0f, W), WithinAbs (300.0f, 1e-3)); // above 20 kHz
}

TEST_CASE("dbToY maps top dB to y=0 and bottom dB to y=H, inverted") {
    const float H = 200.0f, topDb = 24.0f, botDb = -24.0f;
    CHECK_THAT (eb::dbToY ( 24.0f, H, topDb, botDb), WithinAbs (0.0f,   1e-3)); // max dB at top
    CHECK_THAT (eb::dbToY (-24.0f, H, topDb, botDb), WithinAbs (200.0f, 1e-3)); // min dB at bottom
    CHECK_THAT (eb::dbToY (  0.0f, H, topDb, botDb), WithinAbs (100.0f, 1e-3)); // 0 dB centre
}

TEST_CASE("dbToY clamps out-of-range dB to the plot edges") {
    const float H = 200.0f, topDb = 24.0f, botDb = -24.0f;
    CHECK_THAT (eb::dbToY ( 99.0f, H, topDb, botDb), WithinAbs (0.0f,   1e-3));
    CHECK_THAT (eb::dbToY (-99.0f, H, topDb, botDb), WithinAbs (200.0f, 1e-3));
}
