#include <catch2/catch_test_macros.hpp>
#include "gui/GradeBandSmoother.h"

// Per-ear anti-flicker smoothing of the 3 quality bands across consecutive grades (uses the generic
// BandHysteresis + the metric edge arrays / margins). The per-metric dots render the SMOOTHED bands.

TEST_CASE ("GradeBandSmoother: first grade returns the raw hard bands", "[gradesmoother]") {
    eb::GradeBandSmoother s;
    auto b = s.update (28.0f, true, 60.0f, 0.5f);   // sweepSNR 28 (green), IR-SNR 60 (green), THD 0.5% (green)
    CHECK (b.sweepSnr == eb::QualityBand::Green);
    CHECK (b.irSnr    == eb::QualityBand::Green);
    CHECK (b.thd      == eb::QualityBand::Green);
}

TEST_CASE ("GradeBandSmoother: hysteresis holds a band through near-edge flicker", "[gradesmoother]") {
    eb::GradeBandSmoother s;
    CHECK (s.update (28.0f, true, 60.0f, 0.5f).sweepSnr == eb::QualityBand::Green);
    // 24.5 dB: the hard band is Orange (< 25), but the 1 dB dead-band [24,26] HOLDS Green across runs.
    CHECK (s.update (24.5f, true, 60.0f, 0.5f).sweepSnr == eb::QualityBand::Green);
    CHECK (s.update (25.3f, true, 60.0f, 0.5f).sweepSnr == eb::QualityBand::Green);   // still
    // 23 dB clears the dead-band (< 24) -> drops to Orange.
    CHECK (s.update (23.0f, true, 60.0f, 0.5f).sweepSnr == eb::QualityBand::Orange);
}

TEST_CASE ("GradeBandSmoother: invalid sweepSNR -> Unknown, does not poison the hysteresis", "[gradesmoother]") {
    eb::GradeBandSmoother s;
    CHECK (s.update (30.0f, true,  60.0f, 0.5f).sweepSnr == eb::QualityBand::Green);
    CHECK (s.update (5.0f,  false, 60.0f, 0.5f).sweepSnr == eb::QualityBand::Unknown);  // not valid -> Unknown
    CHECK (s.update (30.0f, true,  60.0f, 0.5f).sweepSnr == eb::QualityBand::Green);     // resumes from the held Green
}

TEST_CASE ("GradeBandSmoother: THD direction (lower=better) + reset", "[gradesmoother]") {
    eb::GradeBandSmoother s;
    CHECK (s.update (30.0f, true, 60.0f, 0.5f).thd  == eb::QualityBand::Green);   // 0.5% green
    CHECK (s.update (30.0f, true, 60.0f, 12.0f).thd == eb::QualityBand::Red);     // 12% red
    s.reset();
    CHECK (s.update (30.0f, true, 60.0f, 6.0f).thd  == eb::QualityBand::Orange);  // fresh after reset -> raw band
}
