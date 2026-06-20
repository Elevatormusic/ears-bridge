#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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

TEST_CASE("CalFile extracts serial and HPN type from header") {
    auto cal = eb::CalFile::parse (sampleFrd());
    CHECK(cal.serial == juce::String("000-0000"));
    CHECK(cal.type == eb::CalType::Hpn);
}

TEST_CASE("CalFile detects HEQ type") {
    juce::String t =
        "\"Sens Factor =0.9dB, EARS Serial 000-0000, compensation HEQ V2\"\n"
        "* Freq(Hz) SPL(dB) Phase(degrees)\n"
        "   100.0   0.4   4.4\n   200.0   0.5   8.1\n";
    auto cal = eb::CalFile::parse (t);
    CHECK(cal.type == eb::CalType::Heq);
}

TEST_CASE("CalFile records a non-monotonic frequency as a parse warning") {
    juce::String t =
        "* Freq SPL Phase\n   100.0 0.0 0.0\n   50.0 0.0 0.0\n";
    auto cal = eb::CalFile::parse (t);
    CHECK (cal.points.size() == 2);          // permissive: both rows kept
    CHECK (cal.parseWarnings.size() >= 1);   // out-of-order recorded
}

TEST_CASE("CalFile reads the real R_HPN fixture") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_0000000.txt");
    REQUIRE(f.existsAsFile());
    auto cal = eb::CalFile::parse (f.loadFileAsString());
    CHECK(cal.type == eb::CalType::Hpn);
    CHECK(cal.serial == juce::String("000-0000"));
    REQUIRE(cal.points.size() >= 130);
    CHECK_THAT(cal.points.front().freqHz, Catch::Matchers::WithinAbs(10.0, 1e-9));
    CHECK_THAT(cal.points.back().freqHz, Catch::Matchers::WithinAbs(20000.0, 1e-9));
}

TEST_CASE("CalFile: parses side + serial from header, hashes content, exposes freq range") {
    juce::String txt =
        "* Serial 000-0000 Left\n"
        "20 -1.0 0.0\n"
        "1000 0.5 0.0\n"
        "20000 -2.0 0.0\n";
    auto c = eb::CalFile::parse (txt);
    CHECK (c.side == eb::CalSide::Left);
    CHECK (c.serial == "000-0000");
    CHECK (c.points.size() == 3);
    CHECK (c.minFreqHz() == Catch::Approx (20.0));
    CHECK (c.maxFreqHz() == Catch::Approx (20000.0));
    CHECK (c.contentHash.isNotEmpty());
    // Same content -> same hash; one char change -> different hash.
    CHECK (eb::CalFile::parse (txt).contentHash == c.contentHash);
    CHECK (eb::CalFile::parse (txt + " ").contentHash != c.contentHash);
}

TEST_CASE("CalFile: a non-finite data row is skipped and recorded as a parse warning") {
    juce::String txt = "20 -1.0 0.0\n1000 nan 0.0\n20000 -2.0 0.0\n";
    auto c = eb::CalFile::parse (txt);
    CHECK (c.points.size() == 2);                 // the nan row dropped
    CHECK (c.parseWarnings.size() >= 1);
}
