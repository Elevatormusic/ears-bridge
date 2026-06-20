#include <catch2/catch_test_macros.hpp>
#include "audio/MeasurementSession.h"

using eb::MeasurementSession;
using eb::SessionPhase;

static const float kLoud  = 0.5f;    // -6 dBFS, well above the -24 dB arm threshold
static const float kQuiet = 0.0f;    // below the -50 dB silence floor

static void blocks (MeasurementSession& s, int n, float peak) {
    for (int i = 0; i < n; ++i) s.observeBlockPeak (peak);
}
// Arm the sweep: kArmSustainBlocks consecutive loud blocks.
static void arm (MeasurementSession& s) { blocks (s, MeasurementSession::kArmSustainBlocks, kLoud); }

TEST_CASE("MeasurementSession starts Idle, a quiet warm-up moves to Preflight, not SweepActive") {
    MeasurementSession s; s.reset();
    CHECK (s.phase() == SessionPhase::Idle);
    blocks (s, 4, kQuiet);
    CHECK (s.phase() == SessionPhase::Preflight);
    CHECK_FALSE (s.consumeSweepStarted());
}

TEST_CASE("MeasurementSession arms only after a SUSTAINED rise (a single transient does not arm)") {
    MeasurementSession s; s.reset();
    blocks (s, 2, kQuiet);
    s.observeBlockPeak (kLoud);                       // 1 loud block: a door slam, not the sweep
    CHECK (s.phase() == SessionPhase::Preflight);     // not armed by one block
    s.observeBlockPeak (kQuiet);                      // the run breaks
    CHECK (s.phase() == SessionPhase::Preflight);
    arm (s);                                          // kArmSustainBlocks loud blocks: the sweep
    CHECK (s.phase() == SessionPhase::SweepActive);
    CHECK (s.consumeSweepStarted());                  // genuine onset fires exactly once
    CHECK_FALSE (s.consumeSweepStarted());
}

TEST_CASE("MeasurementSession: a peak just under the arm threshold never arms") {
    MeasurementSession s; s.reset();
    // 0.05 (~ -26 dBFS) is below kSweepStartLinear (-24 dBFS): typical room noise must not arm.
    blocks (s, MeasurementSession::kArmSustainBlocks + 5, 0.05f);
    CHECK (s.phase() == SessionPhase::Preflight);
    CHECK_FALSE (s.consumeSweepStarted());
}

TEST_CASE("MeasurementSession reaches Complete after sustained silence following signal") {
    MeasurementSession s; s.reset();
    arm (s);
    CHECK (s.phase() == SessionPhase::SweepActive);
    blocks (s, MeasurementSession::kDefaultSilenceBlocks - 1, kQuiet);
    CHECK (s.phase() == SessionPhase::SweepActive);             // not yet
    s.observeBlockPeak (kQuiet);                                // crosses the sustain threshold
    CHECK (s.phase() == SessionPhase::Complete);
}

TEST_CASE("MeasurementSession: a brief gap mid-sweep does not end the sweep") {
    MeasurementSession s; s.reset();
    arm (s);
    blocks (s, MeasurementSession::kDefaultSilenceBlocks - 1, kQuiet);   // a gap, not sustained
    s.observeBlockPeak (kLoud);                                          // signal returns -> resets the run
    blocks (s, MeasurementSession::kDefaultSilenceBlocks - 1, kQuiet);
    CHECK (s.phase() == SessionPhase::SweepActive);
}

TEST_CASE("MeasurementSession: the SECOND Dirac sweep re-arms after a provisional Complete, no re-clear") {
    MeasurementSession s; s.reset();
    arm (s);                                                            // left earcup sweep
    CHECK (s.consumeSweepStarted());                                    // drained the one re-scope edge
    blocks (s, MeasurementSession::kDefaultSilenceBlocks + 2, kQuiet);  // long inter-sweep gap
    CHECK (s.phase() == SessionPhase::Complete);                        // provisional complete
    arm (s);                                                            // right earcup sweep
    CHECK (s.phase() == SessionPhase::SweepActive);                     // re-armed
    CHECK_FALSE (s.consumeSweepStarted());                              // re-arm does NOT re-scope latches
}

TEST_CASE("MeasurementSession: markInvalid latches Invalid and is absorbing") {
    MeasurementSession s; s.reset();
    arm (s);
    s.markInvalid();
    CHECK (s.phase() == SessionPhase::Invalid);
    arm (s);                                                            // further audio cannot un-invalidate
    blocks (s, MeasurementSession::kDefaultSilenceBlocks + 2, kQuiet);
    CHECK (s.phase() == SessionPhase::Invalid);
}

TEST_CASE("MeasurementSession: configure sizes the terminal silence in TIME, not raw blocks") {
    MeasurementSession s; s.reset();
    s.configure (256, 48000.0);   // 1.5 s @ 256/48k ~= 281 blocks
    arm (s);
    blocks (s, 280, kQuiet);
    CHECK (s.phase() == SessionPhase::SweepActive);   // 280 < ~281: not yet complete at the smaller block
    blocks (s, 3, kQuiet);
    CHECK (s.phase() == SessionPhase::Complete);
}

TEST_CASE("MeasurementSession: reset returns to Idle and clears the arm run") {
    MeasurementSession s; s.reset();
    s.observeBlockPeak (kLoud); s.observeBlockPeak (kLoud);   // partial arm run
    s.reset();
    CHECK (s.phase() == SessionPhase::Idle);
    arm (s);                                                  // a fresh full run arms cleanly
    CHECK (s.phase() == SessionPhase::SweepActive);
}
