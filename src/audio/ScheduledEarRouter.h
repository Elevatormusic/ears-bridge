#pragma once
#include "audio/SweepSchedule.h"
#include <algorithm>
#include <cmath>

// AutoPerEar hardening (P0-06) — the per-block, audio-thread routing state machine.
//
// Given a learned SweepSchedule (Dirac's [L,R,L] burst), drive WHICH earcup mic to feed Dirac, PREDICTIVELY
// from the schedule's elapsed-time clock — NOT from the mic envelope. This is the fix for the two envelope
// failures: the quiet LF onset is captured because the GAP pre-positions the next ear before the sweep
// ramps up, and the quiet HF tail is captured because the DURATION-GUARD holds the ear until the scheduled
// duration has ~elapsed (it never mistakes the quiet tail for the gap). The mic is consulted only for
// TIMING (when did the sweep start / when did the gap begin) and a loose sanity cross-check; identity is the
// schedule's job (the loopback's ~104 dB isolation is the identity oracle, not the leakage-contaminated mic).
//
// Pure + RT-safe: process() is plain arithmetic + atomics-free reads of the copied schedule. No alloc/lock/
// FFT on the hot path (loadSchedule copies the schedule on the message thread, before Start). The OWNER must
// not call loadSchedule concurrently with process() (single-writer; install while stopped).
namespace eb {

struct RouterOut {
    int  ear;          // the mic to feed: 0 = left, 1 = right
    bool ambiguous;    // this segment diverged from the schedule (drift / fused / inverted) -> fail the grade
};

class ScheduledEarRouter {
public:
    // Install a learned schedule (message thread, before Start). A schedule with < 2 segments / !valid is
    // rejected -> hasSchedule()==false -> the caller uses its mic-envelope fallback. Resets the machine.
    void loadSchedule (const SweepSchedule& s) {
        schedule_ = s;
        has_ = s.valid && s.segments.size() >= 2;
        reset();
    }
    // Fresh run: idle, pre-positioned on the first segment's ear.
    void reset() noexcept {
        state_ = State::Idle; segIdx_ = 0;
        tSeg_ = loudAccum_ = quietAccum_ = invertAccum_ = 0.0;
        ambiguous_ = false;
        ear_ = has_ ? (int) schedule_.segments[0].ear : 0;
    }
    bool hasSchedule() const noexcept { return has_; }
    int  currentEar()  const noexcept { return ear_; }

    // One audio block. peakL/peakR = this block's RAW per-ear mic peak (linear, >=0); floorLin = the measured
    // noise floor (NoiseFloorTracker::floorLinear, max-of-ears or per-ear); blockSec = block duration.
    RouterOut process (float peakL, float peakR, float floorLin, double blockSec) noexcept {
        if (! has_) return { ear_, false };
        if (! std::isfinite (blockSec) || blockSec < 0.0) blockSec = 0.0;   // a non-finite/negative host blockSec must not poison tSeg_
        const float pL      = std::abs (peakL), pR = std::abs (peakR);
        const float peak    = std::max (pL, pR);
        const float minPeak = std::min (pL, pR);
        const float eff     = (floorLin >= kMinFloorLin) ? floorLin : kDefaultFloorLin;   // guard an unlearned floor
        // ARM only on a HARD-PANNED loud block (one ear clearly louder) - mirrors extractSchedule's pan test so a
        // non-panned pre-sweep stereo LEVEL TONE above the onset gate cannot falsely start the schedule clock.
        const bool  loud  = (peak > eff * kOnsetMargin) && (peak > minPeak * kPanRatio);
        const bool  quiet = peak < eff * kGapMargin;
        const int   segN  = (int) schedule_.segments.size();

        switch (state_) {
            case State::Idle:
                ear_ = (int) schedule_.segments[0].ear;                 // pre-position the first ear
                ambiguous_ = false;
                armIfLoud (0, loud, blockSec);
                break;

            case State::InSegment: {
                ear_ = (int) schedule_.segments[segIdx_].ear;           // PREDICTIVE: route the schedule's ear regardless of level
                tSeg_ += blockSec;
                const double dur      = schedule_.segments[segIdx_].durationSec;
                const bool   durOk    = tSeg_ >= dur * (1.0 - kEndGuardTol);
                const double effDwell = gapConfirmSec (segIdx_);   // confirm the gap after a fraction of the LEARNED gap (short gaps followable)
                quietAccum_ = quiet ? quietAccum_ + blockSec : 0.0;

                if (tSeg_ > dur * (1.0 + kDriftTol)) ambiguous_ = true;  // ran long past schedule with no gap (drift/fused)
                // ...and a PREMATURE gap: a sustained quiet WELL BEFORE the scheduled end (a dropout / wrong
                // schedule). The quiet LF onset is NOT caught here - it occurs in Idle/InGap (pre-positioned),
                // before this segment ever armed on the loud body; within InSegment the body has already arrived.
                if (quietAccum_ >= kDwellSec && tSeg_ < dur * (1.0 - kDriftTol)) ambiguous_ = true;
                // mic-sanity: the WRONG ear sustainedly louder (only a CLEAR inversion trips it; identity stays the schedule's)
                if (! quiet) {
                    const float routed = (ear_ == 0) ? pL : pR;
                    const float other  = (ear_ == 0) ? pR : pL;
                    if (other > routed * kInvertMargin) { invertAccum_ += blockSec; if (invertAccum_ >= kInvertSec) ambiguous_ = true; }
                    else invertAccum_ = 0.0;
                } else invertAccum_ = 0.0;

                // Duration-guard transition: end ONLY when the scheduled duration has ~elapsed AND a real gap (sustained
                // quiet for effDwell, which follows the LEARNED gap so a config with short gaps still transitions in time).
                if (durOk && quietAccum_ >= effDwell) {
                    if (segIdx_ + 1 < segN) { state_ = State::InGap; loudAccum_ = 0.0; ear_ = (int) schedule_.segments[segIdx_ + 1].ear; }
                    else                    { state_ = State::Idle;  loudAccum_ = 0.0; }   // last segment done -> ready for the next position
                }
                break;
            }

            case State::InGap:
                ear_ = (int) schedule_.segments[segIdx_ + 1].ear;       // pre-positioned to the next ear (LF onset lands here)
                armIfLoud (segIdx_ + 1, loud, blockSec);
                break;
        }
        return { ear_, ambiguous_ };
    }

private:
    enum class State { Idle, InSegment, InGap };
    // NOTE: tSeg_ resets to 0 at the ARM point (after ~kArmSec of sustained loud), so it runs ~kArmSec short
    // of the true sweep-onset elapsed. This is self-consistent (durOk and the real sweep tail shift together)
    // and harmless at the current tolerances; an on-device tuner should NOT chase this ~50 ms offset.
    void startSegment (int i) noexcept {
        state_ = State::InSegment; segIdx_ = i;
        tSeg_ = loudAccum_ = quietAccum_ = invertAccum_ = 0.0;
        ambiguous_ = false;                                             // fresh segment -> fresh verdict
        ear_ = (int) schedule_.segments[i].ear;
    }
    // Arm `targetSeg` after kArmSec of sustained hard-panned loud (shared by the Idle and InGap states).
    void armIfLoud (int targetSeg, bool loud, double blockSec) noexcept {
        if (loud) { loudAccum_ += blockSec; if (loudAccum_ >= kArmSec) startSegment (targetSeg); }
        else        loudAccum_ = 0.0;
    }
    // Sustained-quiet time to confirm the gap AFTER segment i: a fraction of the LEARNED gap, clamped to
    // [kMinDwellSec, kDwellSec], so a config with SHORT gaps is still followable. A segment with no following
    // gap entry (the last one) -> the full kDwellSec.
    double gapConfirmSec (int i) const noexcept {
        if (i < 0 || i >= (int) schedule_.gapsSec.size()) return kDwellSec;
        return std::clamp (schedule_.gapsSec[(std::size_t) i] * kGapDwellFrac, kMinDwellSec, (double) kDwellSec);
    }

    SweepSchedule schedule_;
    bool   has_      = false;
    State  state_    = State::Idle;
    int    segIdx_   = 0;
    int    ear_      = 0;
    double tSeg_ = 0.0, loudAccum_ = 0.0, quietAccum_ = 0.0, invertAccum_ = 0.0;
    bool   ambiguous_ = false;

    // Tuning (DEFERRED on-device ratification; synthetic defaults). loud := hard-panned AND peak > floor*4
    // (+12 dB); quiet := peak < floor*2 (+6 dB); the band between is "active, not a gap".
    // The L/R-SEPARATION constants (kDefaultFloorLin, kPanRatio, kInvertMargin) describe the SAME physical
    // decision as ProcessingGraph's envelope AutoPerEar gate (0.0032f floor, 2.0f switch ratio) and LrVerify's
    // active/ratio gates. When the wiring task makes this router SUBSUME that envelope path, fold them into ONE
    // shared home so they cannot diverge (review findings #4/#9).
    static constexpr float  kMinFloorLin     = 1.0e-5f;   // below this, treat floorLin as unlearned
    static constexpr float  kDefaultFloorLin = 0.0032f;   // ~-50 dBFS fallback floor (matches ProcessingGraph's gate)
    static constexpr float  kOnsetMargin     = 4.0f;
    static constexpr float  kGapMargin       = 2.0f;
    static constexpr float  kPanRatio        = 3.0f;      // a SWEEP block is hard-panned: louder ear > 3x (~+10 dB) the other
    static constexpr double kArmSec          = 0.05;      // sustained loud to start the schedule clock
    static constexpr double kDwellSec        = 0.15;      // MAX sustained quiet to confirm a gap (+ the premature-gap floor)
    static constexpr double kMinDwellSec     = 0.03;      // MIN, so a short LEARNED gap is still confirmable
    static constexpr double kGapDwellFrac    = 0.4;       // confirm a gap after this fraction of the learned gap
    static constexpr double kEndGuardTol     = 0.15;      // hold the ear until >= 85% of the scheduled duration
    static constexpr double kDriftTol        = 0.30;      // tSeg > 130% of schedule -> ambiguous
    static constexpr float  kInvertMargin    = 2.0f;      // wrong ear louder by 2x ...
    static constexpr double kInvertSec       = 0.30;      // ... sustained 0.30 s -> ambiguous
};

} // namespace eb
