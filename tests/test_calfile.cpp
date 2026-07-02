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

// sideFromFilename is the SECOND ear-side signal (the first is the file content). It must read
// the side from a real EARS export name (L_HPN_..., R_HPN_..., 8604350-R), the words left/right,
// and stay Unknown when the name carries no unambiguous side marker.
TEST_CASE("CalFile::sideFromFilename derives the ear from common names") {
    CHECK (eb::sideFromFilename ("L_HPN_000-0000.txt") == eb::CalSide::Left);
    CHECK (eb::sideFromFilename ("R_HPN_000-0000.txt") == eb::CalSide::Right);
    CHECK (eb::sideFromFilename ("8604350-R.txt")      == eb::CalSide::Right);
    CHECK (eb::sideFromFilename ("headphone.txt")      == eb::CalSide::Unknown);
    CHECK (eb::sideFromFilename ("left_ear.frd")       == eb::CalSide::Left);
}

TEST_CASE("CalFile::sideFromFilename: words win, bare letters need a delimiter, ambiguity is Unknown") {
    // The whole word "left"/"right" anywhere in the name is decisive.
    CHECK (eb::sideFromFilename ("RIGHT_ear_cal.txt") == eb::CalSide::Right);
    CHECK (eb::sideFromFilename ("ears-LEFT.frd")     == eb::CalSide::Left);
    // A delimited single-letter token: leading, trailing, or _L_-style.
    CHECK (eb::sideFromFilename ("cal_R_000-0000.txt") == eb::CalSide::Right);
    CHECK (eb::sideFromFilename ("000-0000_L.txt")     == eb::CalSide::Left);
    // No marker, or a letter buried inside a word (not a side) -> Unknown.
    CHECK (eb::sideFromFilename ("calibration.txt")  == eb::CalSide::Unknown);
    CHECK (eb::sideFromFilename ("realtek.txt")      == eb::CalSide::Unknown);  // the 'l'/'r' inside a word
    // Both sides present (a renamed/ambiguous name) -> Unknown, never a guess.
    CHECK (eb::sideFromFilename ("L_and_R.txt")      == eb::CalSide::Unknown);
    CHECK (eb::sideFromFilename ("left_right.txt")   == eb::CalSide::Unknown);
}

// calSideMismatched is the PER-SLOT single-file check: it must flag a wrong-side file the moment ONE
// slot is loaded, independent of the other slot (the regression that shipped twice -- the swap banner
// used to need BOTH files loaded). Unknown side is never accused; an empty slot is never flagged.
TEST_CASE("CalFile::calSideMismatched flags a wrong-side file in a single slot") {
    using eb::CalSide;
    // The headline case: a RIGHT file alone in the LEFT slot -> flagged (no second file needed).
    CHECK (eb::calSideMismatched (true, CalSide::Right, CalSide::Left)  == true);
    CHECK (eb::calSideMismatched (true, CalSide::Left,  CalSide::Right) == true);
    // Correct side -> not flagged.
    CHECK (eb::calSideMismatched (true, CalSide::Left,  CalSide::Left)  == false);
    CHECK (eb::calSideMismatched (true, CalSide::Right, CalSide::Right) == false);
    // Unknown side (a generically named file with no side marker) -> never falsely accused.
    CHECK (eb::calSideMismatched (true, CalSide::Unknown, CalSide::Left)  == false);
    CHECK (eb::calSideMismatched (true, CalSide::Unknown, CalSide::Right) == false);
    // Empty slot -> never flagged.
    CHECK (eb::calSideMismatched (false, CalSide::Right, CalSide::Left)   == false);
    // End-to-end with the filename detector: "R_HPN_..." in the LEFT slot is the user's exact case.
    CHECK (eb::calSideMismatched (true, eb::sideFromFilename ("R_HPN_000-0000.txt"),
                                  CalSide::Left) == true);
}

// #26: the SPL / phase tokens are validated, not just the frequency. getDoubleValue() returns 0.0 for
// non-numeric text and stops at a comma, so garbage previously became a silent 0 dB point and a comma-decimal
// export silently truncated. Such rows must be SKIPPED + recorded, not folded into the curve as bad data.
TEST_CASE("CalFile #26: non-numeric / comma-decimal SPL rows are skipped, not accepted as 0") {
    auto txt = juce::String (
        "* freq spl phase\n"
        "  100.0   -3.0  0\n"     // good
        "  200.0   junk  0\n"     // non-numeric SPL -> SKIP (was silently 0.0 dB)
        "  400.0   -1,5  0\n"     // comma-decimal -> SKIP (was silently truncated to -1)
        "  1000.0  -2.0  0\n");   // good
    auto cal = eb::CalFile::parse (txt);
    REQUIRE (cal.points.size() == 2);
    CHECK_THAT (cal.points[0].freqHz, WithinAbs (100.0,  1e-9));
    CHECK_THAT (cal.points[1].freqHz, WithinAbs (1000.0, 1e-9));
    CHECK (cal.parseWarnings.size() >= 2);
}
