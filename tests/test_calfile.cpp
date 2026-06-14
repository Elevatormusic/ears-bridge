#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "cal/CalFile.h"

using Catch::Matchers::WithinAbs;

static juce::String sampleFrd() {
    return
        "\"Sens Factor =0.9dB, EARS Serial 000-0000, compensation HPN V1\"\n"
        "\"Use this file on the LEFT channel. Your sensitive side is RIGHT.\"\n"
        "* HPN: Default headphone compensation curve for miniDSP EARS\n"
        "* Freq(Hz) SPL(dB) Phase(degrees)\n"
        "    10.0     -6.4      3.8\n"
        "    20.0     -2.8      2.2\n"
        "  1000.0     -2.1     35.6\n"
        " 20000.0     -2.3      0.6\n";
}

TEST_CASE("CalFile parses numeric data rows") {
    auto cal = eb::CalFile::parse (sampleFrd());
    REQUIRE(cal.points.size() == 4);
    CHECK_THAT(cal.points.front().freqHz, WithinAbs(10.0, 1e-9));
    CHECK_THAT(cal.points.front().splDb,  WithinAbs(-6.4, 1e-9));
    CHECK_THAT(cal.points.front().phaseDeg, WithinAbs(3.8, 1e-9));
    CHECK_THAT(cal.points.back().freqHz,  WithinAbs(20000.0, 1e-9));
    CHECK_THAT(cal.points[2].splDb,       WithinAbs(-2.1, 1e-9));
}
