#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/HealthMonitor.h"
#include <cmath>   // std::abs in the Task-2 level/ratio round-trip cases appended below
// (HealthMonitor.h / EngineTypes.h already included by the Plan-2 test prologue above)
using Catch::Matchers::WithinAbs;

TEST_CASE("HealthMonitor: counters accumulate and snapshot reflects them") {
    eb::HealthMonitor h; h.reset();
    h.reportXrun(); h.reportXrun();
    h.reportDroppedFrames (128);
    h.reportDroppedFrames (64);
    h.setFifoFill (0.42);
    h.setCaptureToRenderRatio (2.0);
    auto s = h.snapshot();
    CHECK (s.xruns == 2);
    CHECK (s.droppedFrames == 192);
    CHECK_THAT (s.fifoFill, WithinAbs (0.42, 1e-3));
    CHECK_THAT (s.captureToRenderRatio, WithinAbs (2.0, 1e-3));
}

TEST_CASE("HealthMonitor: cleanCapture latches false on first fault and stays false") {
    eb::HealthMonitor h; h.reset();
    CHECK (h.snapshot().cleanCapture);
    h.reportXrun();
    CHECK_FALSE (h.snapshot().cleanCapture);
    h.reset();                          // a fresh run clears the latch
    CHECK (h.snapshot().cleanCapture);
    h.reportDroppedFrames (1);
    CHECK_FALSE (h.snapshot().cleanCapture);
}

TEST_CASE("HealthMonitor: levels round-trip and clip flags") {
    eb::HealthMonitor h; h.reset();
    h.reportInLevels (0.5f, 0.25f, false, true);
    h.reportOutLevel (0.8f, false);
    auto lv = h.levels();
    CHECK_THAT (lv.inL, WithinAbs (0.5f, 2e-3));
    CHECK_THAT (lv.inR, WithinAbs (0.25f, 2e-3));
    CHECK_THAT (lv.outMono, WithinAbs (0.8f, 2e-3));
    CHECK_FALSE (lv.clipL);
    CHECK (lv.clipR);
    CHECK_FALSE (lv.clipOut);
}

TEST_CASE("DipGainProfile reflects each EARS model range") {
    auto ears = eb::DipGainProfile::forModel (eb::EarsModel::Ears);
    CHECK(ears.minDb == 0.0);
    CHECK(ears.maxDb == 36.0);

    auto pro = eb::DipGainProfile::forModel (eb::EarsModel::EarsPro);
    CHECK(pro.minDb == 0.0);
    CHECK(pro.maxDb == 45.0);

    // Unknown falls back to the conservative original-EARS range.
    auto unk = eb::DipGainProfile::forModel (eb::EarsModel::Unknown);
    CHECK(unk.maxDb == 36.0);
}

TEST_CASE("HealthFlag bitwise algebra ORs and ANDs as expected") {
    using eb::HealthFlag;
    auto combined = HealthFlag::Xrun | HealthFlag::ClipInput;
    CHECK(eb::any (combined & HealthFlag::Xrun));
    CHECK(eb::any (combined & HealthFlag::ClipInput));
    CHECK_FALSE(eb::any (combined & HealthFlag::Dropout));
    CHECK_FALSE(eb::any (HealthFlag::None & HealthFlag::Xrun));
}
