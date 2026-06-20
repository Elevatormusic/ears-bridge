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
TEST_CASE("CalPairValidator: HEQ is blocked for Dirac") {
    auto L = mk (CalSide::Left,  "000-0000", CalType::Heq);
    auto R = mk (CalSide::Right, "000-0000", CalType::Heq);
    auto r = eb::validateCalibrationPair (L, R, eb::FirMode::MinPhaseMagnitude);
    CHECK_FALSE (r.valid);
    CHECK (r.reason.containsIgnoreCase ("HEQ"));
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
