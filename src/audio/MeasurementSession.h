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
    // clears it, while typical room noise (~ -30 dBFS) stays below. This is the ABSOLUTE backstop; the
    // arm ALSO requires a RISE above the tracked noise floor (see kArmRiseRatio) so steady noise that
    // happens to sit above -24 dBFS cannot arm. Arming also requires kArmSustainBlocks consecutive
    // blocks so a single loud transient (door slam, cable bump) cannot arm.
    static constexpr float kSweepStartLinear      = 0.06310f;   // -24 dBFS (absolute backstop)
    static constexpr int   kArmSustainBlocks      = 8;          // consecutive qualifying blocks to arm
    // #7: a qualifying arm block must also rise at least this factor above the tracked noise floor
    // (4.0 == +12 dB). A real Dirac log sweep starts near-silent and ramps up, so it clears the floor by
    // far; steady room noise (even loud-steady) hugs its own floor, so floor*kArmRiseRatio overtakes it.
    static constexpr float kArmRiseRatio          = 4.0f;       // +12 dB rise above the noise floor
    // Minimum Preflight blocks observed (the floor settling) before an arm run may COMPLETE. Without it,
    // loud-steady energy from the very first block would arm off the low seed before the floor catches
    // up. A real session always has a quiet pre-sweep window, so this never delays a genuine sweep.
    static constexpr int   kArmWarmupBlocks       = 8;          // Preflight floor-settling blocks before arming
    // Below this counts as silence for the sweep-end detector. Same -50 dBFS no-signal floor the level
    // guidance uses, kept independent so the two can't desync by accident. Also the noiseFloor_ seed.
    static constexpr float kSilenceFloorLinear    = 0.00316f;   // -50 dBFS
    // SNR: the pre-sweep window is "stable" (trustworthy as a noise floor) when the spread of its block
    // peaks is within this ratio (3.0 ~ +9.5 dB). A genuinely quiet, stationary room hugs a narrow band;
    // a non-stationary pre-sweep window (HVAC cycling, a passing transient) swings wider and reads false,
    // so the SNR check falls back to honest silence instead of warning off a contaminated floor.
    // PROVISIONAL: on-device ratification needed (does a normal quiet room read stable?).
    static constexpr float kFloorStableRatio      = 3.0f;
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

    // SNR edge: true exactly once per SweepActive->Complete transition (terminal post-sweep silence) —
    // drives the engine's ONE per-sweep SNR verdict (mirrors consumeSweepStarted). Each completed sweep
    // segment (e.g. the right-earcup re-arm that later goes silent again) fires it once.
    bool consumeSweepComplete() noexcept;

    // Latch Invalid (an invalidating HealthMonitor flag fired in-measurement). Absorbing.
    void markInvalid() noexcept;

    SessionPhase phase() const noexcept { return static_cast<SessionPhase> (phase_.load()); }
    bool sweepActive()   const noexcept { return phase() == SessionPhase::SweepActive; }   // D6 consumer

    // SNR: the pre-sweep room noise floor (linear peak) SNAPSHOTTED at the arm instant (Preflight ->
    // SweepActive), then FROZEN for that onset; 0.0 until the session has armed at least once. Per-onset:
    // a Complete -> re-arm reseeds the tracker low, so a second sweep publishes its own pre-sweep floor.
    // Read off the GUI thread via the fixed-point atomic (the project's peak*1000 publish idiom).
    float armNoiseFloor() const noexcept { return armFloorMilli_.load() / 1000.0f; }
    // SNR: true when the pre-sweep warm-up window was quiet/stationary enough to trust the floor (its
    // block-peak spread stayed within kFloorStableRatio). The SNR check only warns off a trusted floor.
    // NOTE: this is the LIVE floor confidence — it is reseeded to false at the SweepActive->Complete
    // edge for the NEXT onset. The engine's per-sweep verdict (drained via consumeSweepComplete) must
    // read completedFloorStable() instead, which is snapshotted at that edge for the sweep that finished.
    bool preSweepFloorStable() const noexcept { return floorStable_.load(); }

    // SNR: the floor confidence of the JUST-COMPLETED sweep, snapshotted at the SweepActive->Complete
    // edge BEFORE floorStable_ is reseeded for the next onset. This is the value the SNR verdict must use
    // (consumeSweepComplete pairs with it); preSweepFloorStable() would already read the next onset's
    // (false) state by the time the engine drains the edge on the same capture block.
    bool completedFloorStable() const noexcept { return completedFloorStable_.load(); }
    // True once the measurement has started (armed or later) — the engine marks Invalid only here, so a
    // pre-sweep room event in Idle/Preflight is never scored, but a same-block silence->Complete plus a
    // late-latched dropout still invalidates.
    bool inMeasurement() const noexcept {
        const auto p = phase();
        return p == SessionPhase::SweepActive || p == SessionPhase::Complete || p == SessionPhase::Invalid;
    }

private:
    // Shared, sustained-rise re-arm helper for Preflight (first onset) and Complete (re-take/2nd sweep).
    // Returns true when the kArmSustainBlocks run of rise-above-floor blocks completes this block.
    // requireWarmup gates the FIRST onset on kArmWarmupBlocks (Complete re-arm doesn't need it).
    bool advanceArmRun (float peak, bool requireWarmup) noexcept;

    // SNR (capture-thread, single writer, no alloc/lock): fold one pre-sweep block peak into the
    // floor-confidence spread (floorMin_/floorMax_) and republish floorStable_. Skipped once a rise is
    // underway (armRun_ > 0) so the sweep's loud onset blocks can't poison the floor-spread estimate.
    void observePreSweepFloor (float peak) noexcept;
    // SNR: snapshot the tracked noiseFloor_ into the frozen, published arm-floor at the arm instant.
    void freezeArmFloor() noexcept;

    std::atomic<int>  phase_        { (int) SessionPhase::Idle };
    std::atomic<bool> sweepStarted_ { false };   // edge, drained by consumeSweepStarted()
    std::atomic<bool> sweepCompleted_ { false }; // SNR edge, drained by consumeSweepComplete()
    int  armRun_            = 0;                  // CAPTURE-THREAD scratch: consecutive rise-above-floor blocks
    int  silenceRun_        = 0;                  // CAPTURE-THREAD scratch: consecutive below-floor blocks in-sweep
    bool sawSignal_         = false;              // CAPTURE-THREAD scratch: signal seen since SweepActive entry
    int  silenceBlocksNeeded_ = kDefaultSilenceBlocks;   // configured terminal-silence count
    // #7: capture-thread noise-floor tracker (single writer, no alloc/lock). Updated only while armRun_==0
    // so a rising sweep can't poison it; an arm block requires peak >= noiseFloor_ * kArmRiseRatio.
    float noiseFloor_      = kSilenceFloorLinear; // CAPTURE-THREAD scratch: EWMA of the pre-sweep room floor
    int   preflightBlocks_ = 0;                   // CAPTURE-THREAD scratch: Preflight blocks seen (warm-up gate)

    // SNR: the pre-sweep floor frozen at the arm instant, published to the GUI via armFloorMilli_
    // (peak*1000 fixed-point, same idiom as HealthMonitor's level atomics). armFloor_ is the capture-thread
    // scalar; armFloorMilli_ is the lock-free read.
    float            armFloor_      = 0.0f;        // CAPTURE-THREAD scratch: noiseFloor_ snapshot at arm
    std::atomic<int> armFloorMilli_ { 0 };        // published: int(armFloor_ * 1000); 0 until armed
    // SNR: floor-confidence over the pre-sweep window. floorMin_/floorMax_ track the per-block-peak spread
    // (capture-thread scalars, single writer); floorStable_ publishes whether it stayed within
    // kFloorStableRatio. All three re-evaluate per onset (reset on reset() + the SweepActive->Complete edge).
    float            floorMin_      = 0.0f;        // CAPTURE-THREAD scratch: min pre-sweep block peak (0 = unset)
    float            floorMax_      = 0.0f;        // CAPTURE-THREAD scratch: max pre-sweep block peak
    std::atomic<bool> floorStable_  { false };     // published: pre-sweep window quiet/stationary enough
    // SNR: floorStable_ snapshotted at the SweepActive->Complete edge (the confidence of the sweep that
    // just finished), so the engine's per-sweep verdict isn't poisoned by the next onset's reseed.
    std::atomic<bool> completedFloorStable_ { false };
};

} // namespace eb
