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

TEST_CASE("liveInputStatus: an active normal sweep shows both channels in dBFS") {
    const auto s = liveInputStatus (-2.0f, -8.0f, /*sweepActive*/ true);
    CHECK (s.severity == LiveInputSeverity::Normal);
    CHECK (s.text.toStdString() == "Sweep in progress - in  L -2.0  R -8.0 dBFS");
    // No reseat hint on a modest, both-loud sweep.
    CHECK_FALSE (s.text.contains ("reseat"));
}

TEST_CASE("liveInputStatus: a clip reads CLIPPING + the positive dB, naming the hot ear") {
    const auto s = liveInputStatus (1.6f, -8.0f, /*sweepActive*/ true);
    CHECK (s.severity == LiveInputSeverity::Clip);
    // The WORDS carry the meaning (HIG: never colour alone): "CLIPPING" + the explicit "+1.6".
    CHECK (s.text.contains ("CLIPPING"));
    CHECK (s.text.contains ("+1.6"));
    CHECK (s.text.contains ("L "));            // names the clipping (hotter) ear
    CHECK (s.text.contains ("lower the output"));
}

TEST_CASE("liveInputStatus: the clip branch names whichever ear is hotter") {
    const auto s = liveInputStatus (-3.0f, 0.4f, /*sweepActive*/ true);   // R over full scale
    CHECK (s.severity == LiveInputSeverity::Clip);
    CHECK (s.text.contains ("CLIPPING"));
    CHECK (s.text.contains ("R "));
    CHECK (s.text.contains ("+0.4"));
}

TEST_CASE("liveInputStatus: a 12 dB L/R gap with a quiet ear appends the reseat hint") {
    // L -3, R -20: 17 dB gap, R below -15 -> the reseat hint fires on the second line.
    const auto s = liveInputStatus (-3.0f, -20.0f, /*sweepActive*/ true);
    CHECK (s.severity == LiveInputSeverity::Warn);
    CHECK (s.text.contains ("reseat"));
    CHECK (s.text.contains ("R low"));         // names the quiet ear
    // The hint sits on a second line so the caller can split it onto statusLineR.
    CHECK (s.text.contains ("\n"));
}

TEST_CASE("liveInputStatus: a big gap but both ears loud does NOT hint a reseat") {
    // 12 dB gap, but the quiet ear (-10) is above the -15 dBFS imbalance floor -> no hint, stays Normal.
    const auto s = liveInputStatus (2.0f - 14.0f, -10.0f, /*sweepActive*/ true);   // L -12, R -10 -> small gap
    CHECK (s.severity == LiveInputSeverity::Normal);
    CHECK_FALSE (s.text.contains ("reseat"));

    const auto s2 = liveInputStatus (0.0f - 0.0f, 0.0f, true);   // both at 0 -> clip, not a reseat case
    CHECK (s2.severity == LiveInputSeverity::Clip);

    // Gap > 6 dB but the quiet ear above -15 -> no hint (a healthy-but-uneven seating reads Normal).
    const auto s3 = liveInputStatus (-2.0f, -12.0f, true);       // 10 dB gap, quiet ear -12 (> -15)
    CHECK (s3.severity == LiveInputSeverity::Normal);
    CHECK_FALSE (s3.text.contains ("reseat"));
}
