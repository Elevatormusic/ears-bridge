#include <catch2/catch_test_macros.hpp>
#include "gui/LiveInputStatus.h"
#include <string>

using eb::liveInputStatus;
using eb::LiveInputSeverity;

TEST_CASE("liveInputStatus: idle defers to the per-ear verdicts (empty text)") {
    // Not sweep-active -> empty text + Normal severity: the caller keeps the existing idle verdict lines.
    const auto s = liveInputStatus (-2.0f, -8.0f, /*sweepActive*/ false);
    CHECK (s.text.isEmpty());
    CHECK (s.severity == LiveInputSeverity::Normal);
}

TEST_CASE("liveInputStatus: an active sweep reports the PEAK (loudest earcup), not per-channel") {
    const auto s = liveInputStatus (-2.0f, -8.0f, /*sweepActive*/ true);
    CHECK (s.severity == LiveInputSeverity::Normal);
    CHECK (s.text == "Sweep in progress" + eb::kDash + "peak -2.0 dBFS");
}

TEST_CASE("liveInputStatus: a clip reads CLIPPED + the positive peak dB") {
    const auto s = liveInputStatus (1.6f, -8.0f, /*sweepActive*/ true);
    CHECK (s.severity == LiveInputSeverity::Clip);
    // The WORDS carry the meaning (HIG: never colour alone): "CLIPPED" + the explicit "+1.6".
    CHECK (s.text.contains ("CLIPPED"));
    CHECK (s.text.contains ("+1.6"));
    CHECK (s.text.contains ("lower the output"));
}

TEST_CASE("liveInputStatus: the clip uses whichever ear is hotter for the peak") {
    const auto s = liveInputStatus (-3.0f, 0.4f, /*sweepActive*/ true);   // R over full scale
    CHECK (s.severity == LiveInputSeverity::Clip);
    CHECK (s.text.contains ("CLIPPED"));
    CHECK (s.text.contains ("+0.4"));
}

// Boundary: the clip gate is `>= 0.0f`, so EXACTLY full scale (0.0 dBFS) must read Clip, and just below it
// must read Normal. Pins the not-X side of the gate (a future flip to `> 0.0f` would silently pass without this).
TEST_CASE("liveInputStatus: peak exactly at 0.0 dBFS clips; just below does not") {
    const auto at = liveInputStatus (0.0f, -30.0f, /*sweepActive*/ true);
    CHECK (at.severity == LiveInputSeverity::Clip);
    CHECK (at.text.contains ("CLIPPED"));

    const auto below = liveInputStatus (-0.2f, -30.0f, /*sweepActive*/ true);
    CHECK (below.severity == LiveInputSeverity::Normal);
    CHECK_FALSE (below.text.contains ("CLIPPED"));
    CHECK (below.text.contains ("peak"));
}

// Regression (the OPPOSITE-ear bug): Dirac sweeps the earcups SEQUENTIALLY, so a large live L/R gap is
// simply the other ear being idle (not yet swept), NOT a seating fault. The live readout must therefore
// NEVER emit a "reseat" hint and must report only the loudest earcup's level.
TEST_CASE("liveInputStatus: a large L/R gap during a sweep does NOT emit a reseat hint") {
    const auto s = liveInputStatus (-3.0f, -20.0f, /*sweepActive*/ true);   // L sweeping, R idle
    CHECK (s.severity == LiveInputSeverity::Normal);
    CHECK_FALSE (s.text.contains ("reseat"));
    CHECK_FALSE (s.text.contains ("\n"));                                   // single line, no second-line hint
    CHECK (s.text == "Sweep in progress" + eb::kDash + "peak -3.0 dBFS");   // the active (loud) ear's level

    // Same with R as the active ear and L idle -> still just the peak, no reseat.
    const auto s2 = liveInputStatus (-25.0f, -1.0f, /*sweepActive*/ true);
    CHECK (s2.severity == LiveInputSeverity::Normal);
    CHECK_FALSE (s2.text.contains ("reseat"));
    CHECK (s2.text == "Sweep in progress" + eb::kDash + "peak -1.0 dBFS");
}
