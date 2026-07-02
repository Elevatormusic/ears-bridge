#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>   // D5: Catch::Approx for the blockPeak peak comparison
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/HealthMonitor.h"
#include "gui/ClipStatus.h"   // D5: invalidMeasurementMessage() for the resetMeasurementLatches caveat check
#include <cmath>   // std::abs in the Task-2 level/ratio round-trip cases appended below
#include <limits>
#include <vector>
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

// ---- SNR Task 2: per-ear in-sweep peak latch + LowSnr guidance flag ----
TEST_CASE("HealthMonitor: per-ear in-sweep peak latches the max per ear") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    CHECK (h.maxSweepPeakL() == Catch::Approx (0.0f));   // 0 until a sweep runs
    CHECK (h.maxSweepPeakR() == Catch::Approx (0.0f));
    // The engine calls observeSweepPeak only while session_.sweepActive() is true. Feed several
    // blocks with varying per-ear peaks; each ear latches its own max independently.
    h.observeSweepPeak (0.10f, 0.04f);
    h.observeSweepPeak (0.20f, 0.05f);
    h.observeSweepPeak (0.15f, 0.03f);
    CHECK (h.maxSweepPeakL() == Catch::Approx (0.20f).margin (2e-3));   // max L peak seen
    CHECK (h.maxSweepPeakR() == Catch::Approx (0.05f).margin (2e-3));   // max R peak seen (asymmetric)
}

TEST_CASE("HealthMonitor: in-sweep peak latches reset on resetMeasurementLatches() and reset()") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.observeSweepPeak (0.30f, 0.22f);
    REQUIRE (h.maxSweepPeakL() == Catch::Approx (0.30f).margin (2e-3));
    REQUIRE (h.maxSweepPeakR() == Catch::Approx (0.22f).margin (2e-3));

    h.resetMeasurementLatches();   // the sweep-onset re-scope clears the numerator latches
    CHECK (h.maxSweepPeakL() == Catch::Approx (0.0f));
    CHECK (h.maxSweepPeakR() == Catch::Approx (0.0f));

    h.observeSweepPeak (0.18f, 0.07f);
    REQUIRE (h.maxSweepPeakL() == Catch::Approx (0.18f).margin (2e-3));
    h.reset();                     // a fresh run also clears them
    CHECK (h.maxSweepPeakL() == Catch::Approx (0.0f));
    CHECK (h.maxSweepPeakR() == Catch::Approx (0.0f));
}

TEST_CASE("HealthMonitor: resetSweepPeaks zeros ONLY the SNR peaks; clip + validity latches untouched") {
    // SNR review fix (Finding 1/2): the engine calls resetSweepPeaks() at the SweepActive->Complete edge
    // so the NEXT earcup sweep starts a fresh numerator. It must zero ONLY the two per-ear peak atomics --
    // a confirmed clip latched during the sweep (ClipConfirmed + cleanCapture=false) must SURVIVE the reset.
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    h.observeSweepPeak (0.30f, 0.22f);
    // Drive a confirmed flat-topped clip run (>= kRailRunMin consecutive flat rail samples) on L.
    std::vector<float> clip (8, 1.0f), quiet (8, 0.0f);
    h.analyzeInputBlock (clip.data(), quiet.data(), 8);
    REQUIRE (h.clipConfirmed());
    REQUIRE_FALSE (h.cleanCapture());
    REQUIRE (h.maxSweepPeakL() == Catch::Approx (0.30f).margin (2e-3));

    h.resetSweepPeaks();   // the per-Complete scope: zero the peaks only
    CHECK (h.maxSweepPeakL() == Catch::Approx (0.0f));
    CHECK (h.maxSweepPeakR() == Catch::Approx (0.0f));
    // The clip/validity latches are NOT disturbed (they belong to the whole run / next re-scope, not here).
    CHECK (h.clipConfirmed());
    CHECK_FALSE (h.cleanCapture());
    CHECK (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
}

TEST_CASE("HealthMonitor: publishCompletedSnrDb snapshots the verdict dB; completedSnrDb reads it back") {
    // SNR review fix (Finding 3): the GUI reads this frozen snapshot, not a live recompute, so the
    // displayed dB can't drift away from the dB that raised LowSnr once the next sweep mutates the peaks.
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    CHECK (h.completedSnrDb() == Catch::Approx (0.0f));    // 0 until a sweep completes
    h.publishCompletedSnrDb (24.0f);
    CHECK (h.completedSnrDb() == Catch::Approx (24.0f).margin (1e-3));   // milli round-trip
    h.publishCompletedSnrDb (-12.5f);                                     // a later sweep overwrites it
    CHECK (h.completedSnrDb() == Catch::Approx (-12.5f).margin (1e-3));
    // A non-finite verdict (e.g. a degenerate floor) must not poison the atomic.
    h.publishCompletedSnrDb (std::numeric_limits<float>::infinity());
    CHECK (h.completedSnrDb() == Catch::Approx (0.0f));
    h.reset();                                            // a fresh run clears the snapshot
    CHECK (h.completedSnrDb() == Catch::Approx (0.0f));
}

TEST_CASE("HealthMonitor: in-sweep peaks do NOT accumulate when the sweep is not active") {
    // The engine gates observeSweepPeak on session_.sweepActive(); when the sweep is NOT active it is
    // simply never called, so the latches stay 0 even though analyzeInputBlock sees loud blocks.
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    std::vector<float> l (16, 0.5f), r (16, 0.3f);
    h.analyzeInputBlock (l.data(), r.data(), 16);   // loud block, but sweep not active -> no observeSweepPeak
    CHECK (h.maxSweepPeakL() == Catch::Approx (0.0f));
    CHECK (h.maxSweepPeakR() == Catch::Approx (0.0f));
}

TEST_CASE("HealthMonitor: LowSnr is guidance, not invalidating") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    REQUIRE (h.cleanCapture());
    REQUIRE_FALSE (eb::any (h.snapshot().flags & eb::HealthFlag::LowSnr));
    h.raiseLowSnr();   // the engine (Task 3) calls this on a noisy-sweep verdict
    CHECK (eb::any (h.snapshot().flags & eb::HealthFlag::LowSnr));   // the flag bit is set
    CHECK (h.cleanCapture());                                       // ...but it does NOT invalidate (guidance)
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

TEST_CASE("HealthMonitor::blockPeak is the max |sample| over both channels, ignoring non-finite") {
    std::vector<float> l { 0.1f, -0.7f, 0.2f }, r { 0.3f, 0.05f, -0.4f };
    CHECK (eb::HealthMonitor::blockPeak (l.data(), r.data(), 3) == Catch::Approx (0.7f));
    std::vector<float> ln { 0.2f, std::numeric_limits<float>::quiet_NaN(), 0.1f }, rn (3, 0.0f);
    CHECK (eb::HealthMonitor::blockPeak (ln.data(), rn.data(), 3) == Catch::Approx (0.2f));  // NaN skipped
}

TEST_CASE("HealthMonitor: resetMeasurementLatches clears validity but keeps config + telemetry + OsResampled") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::EarsPro, 16384, 2.0);   // model + nominal ratio 2.0

    h.observeRenderBlock (256, 256, 2.0, 0.5);                 // advances blockCount; sets ratio/fill
    h.reportInLevels (0.10f, 0.02f, false, false);             // latches reachedGoodLevel
    h.reportRawRail (false);                                   // D2: per-run OsResampled guidance
    std::vector<float> rail (4, 1.0f), sil (4, 0.0f);
    h.analyzeInputBlock (rail.data(), sil.data(), 4);          // confirmed clip -> invalidates
    REQUIRE_FALSE (h.cleanCapture());
    REQUIRE (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
    REQUIRE (eb::any (h.flags() & eb::HealthFlag::OsResampled));

    h.resetMeasurementLatches();   // the sweep-start edge: re-scope validity to the sweep

    // Validity is cleared...
    CHECK (h.cleanCapture());
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (h.clipConfirmed());
    CHECK (h.clipLongestRun() == 0);
    // ...but the per-run OS-SRC guidance + run config + telemetry survive.
    CHECK (eb::any (h.flags() & eb::HealthFlag::OsResampled));            // D2 guidance kept
    CHECK (h.reachedGoodLevel());                                        // kept
    CHECK (std::abs (h.snapshot().captureToRenderRatio - 2.0) < 1e-6);   // ratio telemetry kept

    // And a fresh in-sweep confirmed clip still invalidates after the re-scope.
    h.analyzeInputBlock (rail.data(), sil.data(), 4);
    CHECK_FALSE (h.cleanCapture());
    CHECK (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
    // The in-sweep clip message still carries the OS-resampled caveat (OsResampled survived).
    CHECK (eb::invalidMeasurementMessage (h.flags()).contains ("OS-resampled"));
}

TEST_CASE("HealthMonitor::reportRawRail raises OsResampled only when NOT verified, never invalidates") {
    using eb::HealthFlag; using eb::any;
    SECTION ("verified => no flag, clean") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (true);
        CHECK_FALSE (any (h.flags() & HealthFlag::OsResampled));
        CHECK (h.cleanCapture());
    }
    SECTION ("not verified => OsResampled, but still VALID (guidance)") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (false);
        CHECK (any (h.flags() & HealthFlag::OsResampled));
        CHECK (h.cleanCapture());
    }
    SECTION ("reset clears it") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        h.reportRawRail (false);
        h.reset();
        CHECK_FALSE (any (h.flags() & HealthFlag::OsResampled));
    }
}

TEST_CASE("HealthMonitor: reportSweepRetimed invalidates and flags, distinct from Dropout") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    REQUIRE (h.cleanCapture());
    REQUIRE_FALSE (eb::any (h.flags() & eb::HealthFlag::SweepRetimed));
    h.reportSweepRetimed();
    CHECK_FALSE (h.cleanCapture());                                   // invalidating
    CHECK (eb::any (h.flags() & eb::HealthFlag::SweepRetimed));       // its own flag
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::Dropout));      // NOT folded into the dropout class
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::OsResampled));  // and NOT aliased onto OsResampled (1u<<9)
}

TEST_CASE("HealthMonitor: a FROZEN held off-nominal ratio does not trip ExcessDrift; an unfrozen one does") {
    // D6: while the SRC ratio is frozen for a sweep, the held ratio is intentional - the frozen-mode
    // drift detector is the emergency/SweepRetimed path, not ExcessDrift. An off-nominal held ratio
    // (e.g. a converged trim 1% off) must NOT self-invalidate the sweep via ExcessDrift.
    SECTION ("frozen: a held 1%-off ratio is intentional -> no ExcessDrift, stays clean") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);   // nominal_ = 1.0
        for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks + 4; ++i)
            h.observeRenderBlock (256, 256, 1.01, 0.5, /*frozen*/ true);
        CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
        CHECK (h.cleanCapture());
    }
    SECTION ("free (default): the same sustained 1%-off ratio still trips ExcessDrift") {
        eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
        for (int i = 0; i < eb::HealthMonitor::kDriftSustainBlocks + 4; ++i)
            h.observeRenderBlock (256, 256, 1.01, 0.5);              // frozen defaults false
        CHECK (eb::any (h.flags() & eb::HealthFlag::ExcessDrift));
    }
}

// ---- D8: runtime format revalidation ----
TEST_CASE("HealthMonitor: format stable across blocks -> FormatChanged not raised") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    for (int i = 0; i < 16; ++i)
        h.checkFormatChange(48000.0, 32, 2);
    CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK(h.cleanCapture());
}

TEST_CASE("HealthMonitor: sample rate change mid-run raises FormatChanged and invalidates") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 32, 2);   // first block: clean
    CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK(h.cleanCapture());
    h.checkFormatChange(44100.0, 32, 2);   // rate changed
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
    // Flag is sticky after the event
    h.checkFormatChange(48000.0, 32, 2);   // even if it "recovers", latch stays
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: bit-depth change mid-run raises FormatChanged") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 16, 2);   // depth downgraded
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: channel count change mid-run raises FormatChanged") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 32, 1);   // channel count dropped
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: reset clears prepared format; notifyPreparedFormat after reset re-arms") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(96000.0, 32, 2);   // trigger invalidation
    CHECK_FALSE(h.cleanCapture());
    h.reset();
    // After reset the prepared-format snapshot is zeroed; a zero-sentinel means
    // checkFormatChange is a no-op until notifyPreparedFormat is called again.
    h.checkFormatChange(96000.0, 32, 2);
    CHECK(h.cleanCapture());   // no notification yet -> no comparison -> no flag
    h.notifyPreparedFormat(96000.0, 32, 2);
    h.checkFormatChange(96000.0, 32, 2);   // matches
    CHECK(h.cleanCapture());
}

// ---- D8 Task 4: edge cases ----
TEST_CASE("HealthMonitor: standard rates round-trip through Hz-integer storage without false positive") {
    // All standard WASAPI shared-mode rates must store and compare without rounding error.
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (double r : rates) {
        eb::HealthMonitor h;
        h.prepare(eb::EarsModel::Ears, 4096);
        h.notifyPreparedFormat(r, 32, 2);
        for (int i = 0; i < 8; ++i)
            h.checkFormatChange(r, 32, 2);
        INFO("rate = " << r);
        CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
        CHECK(h.cleanCapture());
    }
}

TEST_CASE("HealthMonitor: checkFormatChange is a no-op before notifyPreparedFormat is called") {
    // On the first startup, or immediately after reset(), the sentinel is 0.
    // An audio block that calls checkFormatChange before notifyPreparedFormat (a race that
    // cannot happen in the real engine, but must be safe in tests) must not raise a flag.
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    // Do NOT call notifyPreparedFormat.
    h.checkFormatChange(48000.0, 32, 2);
    CHECK(h.cleanCapture());
    CHECK_FALSE(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
}

TEST_CASE("HealthMonitor: same rate but depth change 32->16 raises FormatChanged") {
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.checkFormatChange(48000.0, 32, 2);   // clean
    CHECK(h.cleanCapture());
    h.checkFormatChange(48000.0, 16, 2);   // 32->16 bit depth downgrade
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK_FALSE(h.cleanCapture());
}

TEST_CASE("HealthMonitor: FormatChanged co-occurs with other flags correctly") {
    // A dropout AND a format change in the same run: both flags set, cleanCapture false.
    eb::HealthMonitor h;
    h.prepare(eb::EarsModel::Ears, 4096);
    h.notifyPreparedFormat(48000.0, 32, 2);
    h.observeRenderBlock(256, 200, 1.0, 0.1);    // induces Dropout + FifoStarved
    h.checkFormatChange(96000.0, 32, 2);           // also a rate change
    CHECK(eb::any(h.flags() & eb::HealthFlag::FormatChanged));
    CHECK(eb::any(h.flags() & eb::HealthFlag::Dropout));
    CHECK_FALSE(h.cleanCapture());
}

// ---- Noise-floor primitive (Task 3): HealthMonitor owns + exposes the measured floor ----
// #40: cleanCapture is DERIVED from the invalidating-flag mask (single atomic word), never a separate
// bool. The old two-step resetMeasurementLatches (clean.store(true) THEN flagBits.fetch_and) could
// interleave with a render-thread raise() so cleanCapture read false with its explaining flag wiped —
// "measurement invalid" with no reason. Derivation makes the inconsistent state UNREPRESENTABLE: these
// tests pin the contract that !cleanCapture always co-occurs with a surviving invalidating flag.
TEST_CASE("HealthMonitor: !cleanCapture always has an explaining invalidating flag [#40]") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    constexpr auto invalidating = eb::HealthFlag::Xrun | eb::HealthFlag::Dropout
                                | eb::HealthFlag::ExcessDrift | eb::HealthFlag::FifoStarved
                                | eb::HealthFlag::ClipConfirmed | eb::HealthFlag::NonFinite
                                | eb::HealthFlag::SweepRetimed | eb::HealthFlag::FormatChanged;

    // An invalidating raise sets BOTH the flag and !clean, atomically the same word.
    h.reportXrun();
    REQUIRE_FALSE (h.cleanCapture());
    CHECK (eb::any (h.snapshot().flags & invalidating));

    // The sweep-onset re-scope clears the fault AND the verdict TOGETHER: no state where the
    // verdict says invalid but every explaining flag is gone.
    h.resetMeasurementLatches();
    CHECK (h.cleanCapture());
    CHECK_FALSE (eb::any (h.snapshot().flags & invalidating));

    // A fault raised AFTER the re-scope belongs to the new sweep: flag AND verdict both survive.
    h.reportDroppedFrames (8);
    CHECK_FALSE (h.cleanCapture());
    CHECK (eb::any (h.snapshot().flags & eb::HealthFlag::Dropout));

    // Guidance flags never invalidate through the derived read either.
    h.resetMeasurementLatches();
    h.raiseLowSnr();
    CHECK (h.cleanCapture());
}

TEST_CASE("HealthMonitor: exposes the measured noise floor once a quiet window is captured") {
    eb::HealthMonitor hm; hm.prepareNoiseFloor (48000.0, 480);
    CHECK_FALSE (hm.floorValid());
    for (int i = 0; i < 60; ++i) hm.observeFloorBlock (0.004f, 0.004f, 0.010);  // >500 ms quiet
    CHECK (hm.floorValid());
    CHECK_THAT (hm.measuredFloorLinear (0), Catch::Matchers::WithinAbs (0.004f, 5e-4));
    CHECK_THAT (hm.measuredFloorDbAveraged(), Catch::Matchers::WithinAbs (-48.0f, 1.0));
}
