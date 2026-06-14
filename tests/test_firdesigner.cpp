#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"

using Catch::Matchers::WithinAbs;

// cal curve: flat 0 dB at 1k, +20 dB at 4k -> inverted target = 0 dB, -20 dB
static eb::CalFile twoPointCal() {
    eb::CalFile c;
    c.points = { {20.0, 0.0, 0.0}, {1000.0, 0.0, 0.0},
                 {4000.0, 20.0, 0.0}, {20000.0, 0.0, 0.0} };
    return c;
}

static double binToHz (int bin, int fftSize, double sr) {
    return (double) bin * sr / (double) fftSize;
}
static int hzToBin (double hz, int fftSize, double sr) {
    return (int) std::lround (hz * (double) fftSize / sr);
}

TEST_CASE("FirDesigner target magnitude is the inverted, interpolated cal curve") {
    eb::FirDesignParams p; p.sampleRate = 48000.0; p.invert = true; p.maxBoostDb = 12.0;
    const int fftSize = 65536;
    auto mag = eb::FirDesigner::targetMagnitudeLinear (twoPointCal(), p, fftSize);
    REQUIRE((int) mag.size() == fftSize/2 + 1);

    auto dbAt = [&](double hz) {
        int bin = hzToBin (hz, fftSize, p.sampleRate);
        return 20.0 * std::log10 (std::max (1e-9f, mag[(size_t) bin]));
    };
    CHECK_THAT(dbAt(1000.0), WithinAbs(0.0, 0.5));     // inverse of 0 dB
    CHECK_THAT(dbAt(4000.0), WithinAbs(-20.0, 0.5));   // inverse of +20 dB
}

TEST_CASE("FirDesigner clamps excessive boost") {
    // cal -30 dB at 100 Hz -> inverted +30 dB, clamped to +12 dB
    eb::CalFile c; c.points = { {20.0,-30.0,0.0}, {100.0,-30.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.invert = true; p.maxBoostDb = 12.0;
    const int fftSize = 65536;
    auto mag = eb::FirDesigner::targetMagnitudeLinear (c, p, fftSize);
    int bin = hzToBin (100.0, fftSize, p.sampleRate);
    double db = 20.0 * std::log10 (std::max (1e-9f, mag[(size_t) bin]));
    CHECK(db <= 12.0 + 0.5);
    CHECK(db >= 12.0 - 0.5);
}
