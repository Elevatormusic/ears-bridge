#pragma once
#include "audio/EngineTypes.h"   // eb::SessionPhase
#include <atomic>
#include <cmath>

namespace eb {

// Lock-free measurement-session state machine (D5 / R18). Scopes clip/integrity validity to Dirac's
// actual sweep window (left earcup -> gap -> right earcup) instead of the whole Start->Stop run.
// Driven on the CAPTURE thread by the per-block input peak; the GUI reads phase() via an atomic
// (never the internal scratch). Pure: no JUCE audio deps, no alloc, no lock — same contract as LrVerify.
//
// Phases:
//   Idle        - fresh, before the first observed block
//   Preflight   - armed; input is below the sweep-start threshold (room noise / silence)
//   SweepActive - input sustained above the start threshold; THIS is the validity window. D6 will read
//                 sweepActive() to freeze the ClockBridge SRC ratio for the sweep.
//   Complete    - sustained silence after signal. PROVISIONAL: a fresh sustained rise re-arms
//                 SweepActive (the right-earcup sweep / a re-take) WITHOUT re-clearing latches.
//   Invalid     - an invalidating condition fired in-measurement (latched; absorbing)
class MeasurementSession {
public:
    // Sustained input peak (linear) that arms the sweep. -24 dBFS == the app's healthy-level floor
    // (HealthMonitor::kGoodLevelLinear): a correctly-leveled Dirac sweep (the app targets -18..-12 dBFS)
    // clears it, while typical room noise (~ -30 dBFS) stays below. Arming also requires kArmSustainBlocks
    // consecutive blocks so a single loud transient (door slam, cable bump) cannot arm.
    static constexpr float kSweepStartLinear      = 0.06310f;   // -24 dBFS
    static constexpr int   kArmSustainBlocks      = 3;          // consecutive above-threshold blocks to arm
    // Below this counts as silence for the sweep-end detector. Same -50 dBFS no-signal floor the level
    // guidance uses, kept independent so the two can't desync by accident.
    static constexpr float kSilenceFloorLinear    = 0.00316f;   // -50 dBFS
    // Terminal post-signal silence that ends a sweep segment, expressed in TIME. configure() converts
    // it to a block count for the real block size; kDefaultSilenceBlocks is the pre-configure default
    // (~1.5 s @ 512/48k) used by unit tests. 1.5 s is long enough that the quiet tail of a sweep (or a
    // short inter-sweep gap) does not end it early; Complete is re-armable so a longer gap is also safe.
    static constexpr double kSilenceCompleteSeconds = 1.5;
    static constexpr int    kDefaultSilenceBlocks   = 140;

    void reset() noexcept;
    void configure (int blockSize, double sampleRate) noexcept;   // size the terminal-silence window

    // Advance the machine with one block's input peak (max of the two channel peaks). RT-safe.
    void observeBlockPeak (float peak) noexcept;

    // Edge: true exactly once, on the FIRST (genuine) sweep onset (Preflight -> SweepActive) — drives
    // the one validity re-scope in the engine. Re-arms out of a provisional Complete do NOT set it.
    bool consumeSweepStarted() noexcept;

    // Latch Invalid (an invalidating HealthMonitor flag fired in-measurement). Absorbing.
    void markInvalid() noexcept;

    SessionPhase phase() const noexcept { return static_cast<SessionPhase> (phase_.load()); }
    bool sweepActive()   const noexcept { return phase() == SessionPhase::SweepActive; }   // D6 consumer
    // True once the measurement has started (armed or later) — the engine marks Invalid only here, so a
    // pre-sweep room event in Idle/Preflight is never scored, but a same-block silence->Complete plus a
    // late-latched dropout still invalidates.
    bool inMeasurement() const noexcept {
        const auto p = phase();
        return p == SessionPhase::SweepActive || p == SessionPhase::Complete || p == SessionPhase::Invalid;
    }

private:
    // Shared, sustained-rise re-arm helper for Preflight (first onset) and Complete (re-take/2nd sweep).
    // Returns true when the kArmSustainBlocks run completes this block.
    bool advanceArmRun (float peak) noexcept;

    std::atomic<int>  phase_        { (int) SessionPhase::Idle };
    std::atomic<bool> sweepStarted_ { false };   // edge, drained by consumeSweepStarted()
    int  armRun_            = 0;                  // CAPTURE-THREAD scratch: consecutive above-threshold blocks
    int  silenceRun_        = 0;                  // CAPTURE-THREAD scratch: consecutive below-floor blocks in-sweep
    bool sawSignal_         = false;              // CAPTURE-THREAD scratch: signal seen since SweepActive entry
    int  silenceBlocksNeeded_ = kDefaultSilenceBlocks;   // configured terminal-silence count
};

} // namespace eb
