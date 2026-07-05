#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "cal/CalibrationPairValidator.h"
#include "cal/FirDesigner.h"
using eb::CalFile; using eb::CalSide; using eb::CalType; using eb::CalPoint;

static CalFile mk (CalSide side, juce::String serial, CalType type) {
    CalFile c; c.side = side; c.serial = serial; c.type = type;
    for (int f = 20; f <= 10000; f *= 2) c.points.push_back ({ (double) f, 0.0, 0.0 });
    c.points.push_back ({ 20000.0, 0.0, 0.0 });
    return c;
}

TEST_CASE("CalFile: real EARS header marks side in content, swap rejected") {
    // Mirrors the real factory file (sanitized serial): two quoted header lines, then *-comments, then data.
    auto rightTxt = juce::String (
        "\"Sens Factor =0.9dB, EARS Serial 000-0000, compensation HPN V1\"\n"
        "\"Use this file on the RIGHT channel. Your sensitive side is RIGHT.\"\n"
        "*\n* HPN: Default headphone compensation curve for miniDSP EARS\n*\n"
        "* For use with headphones. Do not use with IEMs.\n*\n"
        "* Freq(Hz) SPL(dB) Phase(degrees)\n*\n"
        "    10.0   -6.6    3.8\n    20.0  -3.0   2.0\n    50.0  -2.0   1.5\n"
        "    100.0  -2.0   1.0\n    500.0  -0.5  0.4\n    1000.0  0.0  0.0\n"
        "    5000.0 -0.5  0.3\n    10000.0 -1.0 0.5\n    20000.0 -3.0 0.2\n");
    auto leftTxt = rightTxt.replace ("RIGHT", "LEFT");

    auto R = eb::CalFile::parse (rightTxt);
    auto L = eb::CalFile::parse (leftTxt);
    INFO ("parsed R.side=" << (int) R.side << "  L.side=" << (int) L.side
          << "  R.type=" << (int) R.type << "  R.points=" << (int) R.points.size());
    CHECK (R.side == eb::CalSide::Right);   // does the parser read the side from the quoted content?
    CHECK (L.side == eb::CalSide::Left);
    CHECK (R.type == eb::CalType::Hpn);

    // The swap the user hit: the LEFT file dropped into the RIGHT slot. Must be rejected.
    auto verdict = eb::validateCalibrationPair (L /*left slot*/, L /*right slot = left file*/,
                                                eb::FirMode::MinPhaseMagnitude);
    INFO ("swap verdict valid=" << verdict.valid << "  reason=" << verdict.reason);
    CHECK_FALSE (verdict.valid);
}

TEST_CASE("CalPairValidator: a well-formed HPN left+right pair with matching serial is valid") {
    auto L = mk (CalSide::Left,  "000-0000", CalType::Hpn);
    auto R = mk (CalSide::Right, "000-0000", CalType::Hpn);
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK (r.valid);
}
TEST_CASE("CalPairValidator: swapped sides are rejected") {
    auto L = mk (CalSide::Right, "000-0000", CalType::Hpn);   // right file in the left slot
    auto R = mk (CalSide::Left,  "000-0000", CalType::Hpn);
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK_FALSE (r.valid);
    CHECK (r.reason.containsIgnoreCase ("side"));
}
TEST_CASE("CalPairValidator: serial mismatch is rejected") {
    auto L = mk (CalSide::Left,  "000-0000", CalType::Hpn);
    auto R = mk (CalSide::Right, "111-1111", CalType::Hpn);
    CHECK_FALSE (eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude).valid);
}
TEST_CASE("CalPairValidator: HEQ is accepted for Dirac (miniDSP's recommended headphone cal)") {
    // miniDSP's own "Using Dirac Live to tune headphones" note: "We suggest using the HEQ calibration
    // file." HEQ is the RECOMMENDED Dirac cal, not an invalid one -> a well-formed HEQ pair is valid.
    auto L = mk (CalSide::Left,  "000-0000", CalType::Heq);
    auto R = mk (CalSide::Right, "000-0000", CalType::Heq);
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK (r.valid);
}
TEST_CASE("CalPairValidator: Unknown type blocked unless override; Unknown SIDE allowed") {
    auto L = mk (CalSide::Unknown, "000-0000", CalType::Unknown);
    auto R = mk (CalSide::Unknown, "000-0000", CalType::Unknown);
    CHECK_FALSE (eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude).valid);
    CHECK (eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude, 20.0, 20000.0, /*allowUnknownType*/ true).valid);
}
TEST_CASE("CalPairValidator: insufficient band coverage is rejected") {
    CalFile L; L.side = CalSide::Left;  L.serial = "000-0000"; L.type = CalType::Hpn;
    CalFile R; R.side = CalSide::Right; R.serial = "000-0000"; R.type = CalType::Hpn;
    for (int f = 200; f <= 8000; f *= 2) { L.points.push_back({(double)f,0,0}); R.points.push_back({(double)f,0,0}); }
    CHECK_FALSE (eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude).valid);   // 200..8000 doesn't cover 20..20k
}

// --- Task-1-window regression: the parser is now PERMISSIVE, so a non-monotonic
// (out-of-order) file LOADS with a parse warning instead of throwing. This validator
// is the gate that must catch it — at Rule 5 (strict-increase), BEFORE Rule 6 (range),
// because minFreqHz()/maxFreqHz() (front()/back()) are only trustworthy once strictly
// increasing. The reason must name "frequenc...".
TEST_CASE("CalPairValidator: a non-monotonic (out-of-order) pair is rejected at Rule 5") {
    auto L = mk (CalSide::Left,  "000-0000", CalType::Hpn);
    auto R = mk (CalSide::Right, "000-0000", CalType::Hpn);
    // Corrupt L into a non-monotonic sequence while keeping >= 8 points and finite values.
    // Endpoints still span 20..20000 so a naive front()/back() range check would PASS —
    // proving Rule 5 must run before Rule 6.
    REQUIRE (L.points.size() >= 8);
    std::swap (L.points[2], L.points[4]);   // out-of-order interior row, not strictly increasing
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK_FALSE (r.valid);
    CHECK (r.reason.containsIgnoreCase ("frequenc"));
}

TEST_CASE("CalPairValidator: too few points is rejected") {
    CalFile L; L.side = CalSide::Left;  L.serial = "000-0000"; L.type = CalType::Hpn;
    CalFile R; R.side = CalSide::Right; R.serial = "000-0000"; R.type = CalType::Hpn;
    for (int f = 20; f <= 20000; f *= 4) { L.points.push_back({(double)f,0,0}); R.points.push_back({(double)f,0,0}); }
    REQUIRE (L.points.size() < 8);
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK_FALSE (r.valid);
    CHECK (r.reason.containsIgnoreCase ("points"));
}

TEST_CASE("CalPairValidator: a non-finite spl is rejected at Rule 5") {
    auto L = mk (CalSide::Left,  "000-0000", CalType::Hpn);
    auto R = mk (CalSide::Right, "000-0000", CalType::Hpn);
    R.points[3].splDb = std::numeric_limits<double>::quiet_NaN();
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK_FALSE (r.valid);
}

// #8: side must resolve from the CHANNEL phrase, not the per-unit "sensitive side" word. On a left-sensitive
// unit the RIGHT-channel file reads "...RIGHT channel. Your sensitive side is LEFT." - the old bare-word
// parser tested "left" first and mis-sided it, rejecting the factory-correct pair (Start locked shut).
TEST_CASE("CalFile #8: side reads the channel phrase, not the sensitive-side word") {
    auto txt = juce::String (
        "\"Sens Factor =0.9dB, EARS Serial 000-0000, compensation HEQ V1\"\n"
        "\"Use this file on the RIGHT channel. Your sensitive side is LEFT.\"\n"
        "* HEQ\n    20.0 -3.0 0\n    1000.0 0.0 0\n    20000.0 -6.0 0\n");
    CHECK (eb::CalFile::parse (txt).side == eb::CalSide::Right);
    // the matched (channel==sensitive) case still works
    CHECK (eb::CalFile::parse (txt.replace ("is LEFT", "is RIGHT")).side == eb::CalSide::Right);
}

// T10 (T9 observation 1): the RAW fixture used to carry only 6 points, so the RAW-caution UI
// state was never pure - the pair ALSO tripped "too few points" and Calibrate went Error. The
// fixture is now >= 8 monotonic points spanning 10..20000 Hz, so a RAW+HEQ pair passes the
// validator and the amber RAW caution renders on an otherwise-clean card. The too-few-points
// REJECT keeps its own case above ("CalPairValidator: too few points is rejected").
TEST_CASE("CalPairValidator: the RAW test fixture passes validation (pure caution state)") {
    const juce::File data (EB_TEST_DATA_DIR);
    auto rawL = eb::CalFile::parse (data.getChildFile ("L_RAW_0000000.txt").loadFileAsString());
    auto heqR = eb::CalFile::parse (data.getChildFile ("R_HEQ_0000000.txt").loadFileAsString());
    REQUIRE (rawL.type == eb::CalType::Raw);
    REQUIRE ((int) rawL.points.size() >= 8);
    auto r = eb::validateCalibrationPair (rawL, heqR, eb::FirMode::MinPhaseMagnitude);
    INFO ("reason: " << r.reason);
    CHECK (r.valid);
}

// #7: coverage is PER FILE. A band-limited file for one ear must fail (each ear's FIR is designed from its own
// file, so a truncated one gets a flat wrong correction); the reason names the failing side.
TEST_CASE("CalPairValidator #7: a band-limited single file fails per-file coverage") {
    auto full = mk (eb::CalSide::Left, "000-0000", eb::CalType::Heq);   // 20 Hz - 20 kHz
    eb::CalFile band; band.side = eb::CalSide::Right; band.serial = "000-0000"; band.type = eb::CalType::Heq;
    for (double f = 200; f <= 8000; f *= 1.3) band.points.push_back ({ f, 0.0, 0.0 });
    band.points.push_back ({ 8000.0, 0.0, 0.0 });                        // 200 Hz - 8 kHz only

    auto v = eb::validateCalibrationPair (full, band, eb::FirMode::MinPhaseMagnitude);
    CHECK_FALSE (v.valid);
    CHECK (v.reason.containsIgnoreCase ("right"));
    CHECK (v.reason.containsIgnoreCase ("cover"));
    // the symmetric full/full pair still passes coverage
    CHECK (eb::validateCalibrationPair (full, mk (eb::CalSide::Right, "000-0000", eb::CalType::Heq),
                                        eb::FirMode::MinPhaseMagnitude).valid);
}
