#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/HealthMonitor.h"
#include <cmath>   // std::abs in the Task-2 level/ratio round-trip cases appended below
#include <limits>
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

TEST_CASE("HealthMonitor flags input clip above -1 dBFS but clip does NOT invalidate cleanCapture") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    // L peak clips (>= kClipLinear), R is fine; the caller passes the matching clip bools.
    h.reportInLevels (0.95f, 0.10f, true, false);
    auto lv = h.levels();
    CHECK(lv.clipL);
    CHECK_FALSE(lv.clipR);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ClipInput));
    // Clip is a guidance warning, not a measurement invalidation.
    CHECK(h.cleanCapture());
}

TEST_CASE("HealthMonitor flags ClipOutput from reportOutLevel") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.reportOutLevel (0.95f, true);   // mono output clips
    CHECK(h.levels().clipOut);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ClipOutput));
    CHECK(h.cleanCapture());          // output clip is guidance, not invalidation
}

TEST_CASE("HealthMonitor flags low input level only after the grace window") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 4096);
    // During grace: silent input must NOT raise LowLevel.
    for (int i = 0; i < eb::HealthMonitor::kLowLevelGraceBlocks; ++i) {
        h.observeRenderBlock (256, 256, 1.0, 0.5);  // advances the block counter
        h.reportInLevels (0.0f, 0.0f, false, false);
    }
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::LowLevel));
    // After grace, a sustained near-silent capture raises LowLevel.
    h.observeRenderBlock (256, 256, 1.0, 0.5);
    h.reportInLevels (0.001f, 0.001f, false, false);
    CHECK(eb::any (h.flags() & eb::HealthFlag::LowLevel));
}

TEST_CASE("HealthMonitor latches Dropout and invalidates cleanCapture on silence-filled render") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    CHECK(h.cleanCapture());
    h.observeRenderBlock (256, 200, 1.0, 0.1);     // got < wanted -> FIFO starved + dropout
    CHECK(eb::any (h.flags() & eb::HealthFlag::Dropout));
    CHECK(eb::any (h.flags() & eb::HealthFlag::FifoStarved));
    CHECK_FALSE(h.cleanCapture());                 // latched
    // Latch is sticky: a subsequent clean block does not clear it.
    h.observeRenderBlock (256, 256, 1.0, 0.5);
    CHECK_FALSE(h.cleanCapture());
    CHECK(h.snapshot().droppedFrames == 56);       // 256 - 200 accounted via observeRenderBlock
}

TEST_CASE("HealthMonitor latches ExcessDrift only after sustained out-of-tolerance ratio") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 4096);
    const double bad = 1.0 + 2.0 * eb::HealthMonitor::kDriftRatioTol; // +1.0% > 0.5% tol
    // One bad block must NOT latch.
    h.observeRenderBlock (256, 256, bad, 0.5);
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
    // Sustain to the threshold -> latch.
    for (int i = 1; i < eb::HealthMonitor::kDriftSustainBlocks; ++i)
        h.observeRenderBlock (256, 256, bad, 0.5);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
    CHECK_FALSE(h.cleanCapture());
    // observeRenderBlock forwarded the ratio through the Plan-2 micro-fixed-point setter, so
    // compare within the 1e-6 storage resolution rather than bit-exact.
    CHECK(std::abs (h.snapshot().captureToRenderRatio - bad) < 1e-6);
}

TEST_CASE("HealthMonitor: an in-tolerance block resets the drift run so noise does not accumulate") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 4096);
    const double bad = 1.0 + 2.0 * eb::HealthMonitor::kDriftRatioTol;
    for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks - 1; ++i)
        h.observeRenderBlock (256, 256, bad, 0.5);
    h.observeRenderBlock (256, 256, 1.0, 0.5);     // good block resets the run
    h.observeRenderBlock (256, 256, bad, 0.5);     // start over
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
}

TEST_CASE("HealthMonitor measures drift against the NOMINAL ratio, not 1.0 (mismatched-rate run)") {
    // 96k capture -> 48k render: nominal capture:render ratio is 2.0. A ratio AT nominal must NOT
    // latch ExcessDrift (the pre-fix |ratio-1.0| check would have falsely tripped on every block).
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 16384, 2.0);
    for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks + 4; ++i)
        h.observeRenderBlock (256, 256, 2.0, 0.5);          // on-nominal: no drift
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));

    // A ratio deviating from nominal by > tol, sustained, DOES latch.
    const double off = 2.0 * (1.0 + 2.0 * eb::HealthMonitor::kDriftRatioTol);
    for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks; ++i)
        h.observeRenderBlock (256, 256, off, 0.5);
    CHECK(eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
}

TEST_CASE("HealthMonitor xrun counter, flag, and reset (Plan-2 reportXrun preserved)") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.reportXrun(); h.reportXrun();
    CHECK(h.snapshot().xruns == 2);
    CHECK(eb::any (h.flags() & eb::HealthFlag::Xrun));
    CHECK_FALSE(h.cleanCapture());                 // xrun invalidates a measurement (spec §3.5)
    h.reset();
    CHECK(h.snapshot().xruns == 0);
    CHECK(h.cleanCapture());
    CHECK_FALSE(eb::any (h.flags() & eb::HealthFlag::Xrun));
}

TEST_CASE("HealthMonitor: Plan-2 reportDroppedFrames still accumulates + invalidates") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.reportDroppedFrames (128);
    h.reportDroppedFrames (64);
    CHECK(h.snapshot().droppedFrames == 192);
    CHECK_FALSE(h.cleanCapture());                 // dropped frames invalidate the run
}

TEST_CASE("HealthMonitor: Plan-2 setFifoFill / setCaptureToRenderRatio / levels round-trip") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.setFifoFill (0.42);
    h.setCaptureToRenderRatio (2.0);
    h.reportInLevels (0.5f, 0.25f, false, false);
    h.reportOutLevel (0.8f, false);
    auto s  = h.snapshot();
    auto lv = h.levels();
    CHECK(std::abs (s.fifoFill - 0.42) < 1e-3);
    CHECK(std::abs (s.captureToRenderRatio - 2.0) < 1e-3);
    CHECK(std::abs (lv.inL - 0.5f) < 2e-3f);
    CHECK(std::abs (lv.inR - 0.25f) < 2e-3f);
    CHECK(std::abs (lv.outMono - 0.8f) < 2e-3f);
}

TEST_CASE("HealthMonitor latches reachedGoodLevel once a healthy capture peak is seen") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    CHECK_FALSE(h.reachedGoodLevel());
    // A present-but-too-quiet capture sits ABOVE the -50 dBFS no-signal floor but BELOW the -24 dBFS
    // healthy floor -- the "tin-can" case. It must NOT count as reaching a usable level.
    h.reportInLevels (0.02f, 0.02f, false, false);   // ~ -34 dBFS, both ears
    CHECK_FALSE(h.reachedGoodLevel());
    // A healthy peak on EITHER ear (>= kGoodLevelLinear) latches it true.
    h.reportInLevels (0.10f, 0.02f, false, false);   // L ~ -20 dBFS
    CHECK(h.reachedGoodLevel());
    // It stays latched even if the level later falls back (so an inter-sweep gap can't un-set it).
    h.reportInLevels (0.0f, 0.0f, false, false);
    CHECK(h.reachedGoodLevel());
    // A fresh run clears the latch.
    h.reset();
    CHECK_FALSE(h.reachedGoodLevel());
}

TEST_CASE("HealthMonitor: scanAndFlagNonFinite invalidates on NaN/Inf, ignores finite") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    const float clean[4] = { 0.0f, 0.5f, -0.5f, 0.25f };
    CHECK_FALSE (h.scanAndFlagNonFinite (clean, 4));
    CHECK (h.cleanCapture());
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::NonFinite));

    const float bad[4] = { 0.1f, std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.3f };
    CHECK (h.scanAndFlagNonFinite (bad, 4));
    CHECK (eb::any (h.flags() & eb::HealthFlag::NonFinite));
    CHECK_FALSE (h.cleanCapture());                 // NonFinite is invalidating

    const float inf[2] = { std::numeric_limits<float>::infinity(), 0.0f };
    eb::HealthMonitor h2; h2.prepare (eb::EarsModel::Ears, 4096);
    CHECK (h2.scanAndFlagNonFinite (inf, 2));
    CHECK_FALSE (h2.cleanCapture());
    CHECK (eb::any (h2.flags() & eb::HealthFlag::NonFinite));
}

TEST_CASE("HealthMonitor: a consecutive near-rail run is a confirmed clip; an isolated peak is not") {
    using eb::HealthFlag; using eb::any;
    // helper: build an L buffer, R silent
    auto runL = [] (eb::HealthMonitor& h, std::vector<float> l) {
        std::vector<float> r (l.size(), 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
    };

    SECTION ("positive rail run -> confirmed + invalid") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        runL (h, { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f });   // 3 consecutive at the rail
        CHECK (h.clipConfirmed());
        CHECK (any (h.flags() & HealthFlag::ClipConfirmed));
        CHECK_FALSE (h.cleanCapture());               // confirmed clip invalidates
        CHECK (h.clipLongestRun() >= 3);
        CHECK (h.clipRailSamples() >= 3);
    }
    SECTION ("negative rail run -> confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        runL (h, { -1.0f, -1.0f, -1.0f });
        CHECK (h.clipConfirmed());
        CHECK_FALSE (h.cleanCapture());
    }
    SECTION ("isolated full-scale peak (clean 0 dBFS sine sample) -> NOT confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        runL (h, { 0.7f, 1.0f, 0.7f, -1.0f, 0.7f });  // no run of 3
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());                     // stays valid
    }
    SECTION ("clean -1 dBFS constant -> near-rail guidance, NOT confirmed, still valid") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l (16, eb::HealthMonitor::kClipLinear);   // exactly -1.0 dBFS
        std::vector<float> r (16, 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), 16);
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());
        CHECK (any (h.flags() & HealthFlag::ClipInput));   // guidance fires
    }
    SECTION ("clean -0.1 dBFS -> neither confirmed nor near-rail-invalid") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l (16, 0.98855f);   // -0.1 dBFS, below kRailCeiling
        std::vector<float> r (16, 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), 16);
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());
        CHECK (eb::any (h.flags() & eb::HealthFlag::ClipInput));   // guidance fires; still valid
    }
    SECTION ("a NaN on one channel does not suppress a confirmed run on the other") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l { 1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f };
        std::vector<float> r { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   // clean 5-sample rail run on R
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
        CHECK (h.clipConfirmed());                                  // R's run still confirms
        CHECK (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
        CHECK (eb::any (h.flags() & eb::HealthFlag::NonFinite));    // the L NaN is also flagged
        CHECK_FALSE (h.cleanCapture());
    }
    SECTION ("right-channel-only rail run -> confirmed (per-channel)") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l (8, 0.1f);
        std::vector<float> r { 0.1f, 1.0f, 1.0f, 1.0f, 0.1f };
        h.analyzeInputBlock (l.data(), r.data(), (int) r.size());
        CHECK (h.clipConfirmed());
    }
}

TEST_CASE("Input meter CLIP latches at the rail, not at the -1 dBFS guidance threshold") {
    using eb::HealthFlag; using eb::any;
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    // -0.4 dBFS (0.95): above the guidance threshold (-1 dBFS) but NOT at the rail.
    std::vector<float> l (16, 0.95f), r (16, 0.0f);
    h.analyzeInputBlock (l.data(), r.data(), 16);
    CHECK_FALSE (h.levels().clipL);                              // meter LED off below the rail
    CHECK (any (h.flags() & HealthFlag::ClipInput));             // guidance still fires at -1 dBFS
    // At the rail (0.9999): meter LED on.
    eb::HealthMonitor h2; h2.prepare (eb::EarsModel::Ears, 4096);
    std::vector<float> l2 (16, 0.9999f), r2 (16, 0.0f);
    h2.analyzeInputBlock (l2.data(), r2.data(), 16);
    CHECK (h2.levels().clipL);
}

TEST_CASE("Confirmed clip requires a FLAT rail run, not a smooth full-scale peak") {
    using eb::HealthFlag; using eb::any;
    SECTION ("flat-topped clip (equal rail samples) -> confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        std::vector<float> l { 0.5f, 1.0f, 1.0f, 1.0f, 0.5f };   // flat top, delta == 0
        std::vector<float> r (l.size(), 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
        CHECK (h.clipConfirmed());
        CHECK_FALSE (h.cleanCapture());
    }
    SECTION ("smooth full-scale sine peak (varying rail samples) -> NOT confirmed") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        // Three consecutive samples above kRailCeiling but each differs by ~1.5e-4 (a sine peak).
        std::vector<float> l { 0.99980f, 0.99993f, 1.00000f, 0.99993f, 0.99980f };
        std::vector<float> r (l.size(), 0.0f);
        h.analyzeInputBlock (l.data(), r.data(), (int) l.size());
        CHECK_FALSE (h.clipConfirmed());
        CHECK (h.cleanCapture());
    }
}
