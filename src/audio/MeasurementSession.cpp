#include "audio/MeasurementSession.h"
#include <algorithm>

namespace eb {

void MeasurementSession::reset() noexcept {
    phase_.store ((int) SessionPhase::Idle);
    sweepStarted_.store (false);
    armRun_ = 0; silenceRun_ = 0; sawSignal_ = false;
    noiseFloor_ = kSilenceFloorLinear; preflightBlocks_ = 0;
    // SNR: clear the frozen arm-floor + the floor-confidence trackers (re-evaluated per onset).
    armFloor_ = 0.0f; armFloorMilli_.store (0);
    floorMin_ = 0.0f; floorMax_ = 0.0f; floorStable_.store (false);
}

void MeasurementSession::configure (int blockSize, double sampleRate) noexcept {
    if (blockSize > 0 && sampleRate > 0.0) {
        const long n = std::lround (kSilenceCompleteSeconds * sampleRate / (double) blockSize);
        silenceBlocksNeeded_ = (int) std::max (1L, n);
    }
}

bool MeasurementSession::advanceArmRun (float peak, bool requireWarmup) noexcept {
    // Track the room noise floor ONLY while a rise isn't already underway (armRun_ == 0): once the sweep
    // is rising, freezing the floor keeps the rise gate honest (the loud sweep can't drag the floor up to
    // meet itself). EWMA: a slow 5% pull so a brief transient barely moves it; clamped at the seed floor.
    if (armRun_ == 0)
        noiseFloor_ = std::max (kSilenceFloorLinear, 0.95f * noiseFloor_ + 0.05f * peak);

    // A qualifying arm block: above the absolute backstop AND a real rise above the tracked floor.
    if (peak >= kSweepStartLinear && peak >= noiseFloor_ * kArmRiseRatio) {
        // Don't trust the low seed from the very first block: a genuine sweep is always preceded by a
        // quiet pre-sweep window, so require the floor to have settled across kArmWarmupBlocks first.
        if (requireWarmup && preflightBlocks_ < kArmWarmupBlocks)
            return false;
        if (++armRun_ >= kArmSustainBlocks) { armRun_ = 0; return true; }
    } else {
        armRun_ = 0;
    }
    return false;
}

void MeasurementSession::observePreSweepFloor (float peak) noexcept {
    // Only fold the genuine pre-sweep window (no rise underway): the same gate noiseFloor_ uses, so the
    // floor-spread reflects the quiet room, not the ramp that arms.
    if (armRun_ != 0)
        return;
    if (floorMin_ <= 0.0f) {                 // first pre-sweep block of this onset: seed both ends
        floorMin_ = floorMax_ = std::max (peak, kSilenceFloorLinear);
    } else {
        const float p = std::max (peak, kSilenceFloorLinear);   // clamp at the silence seed: a true-silent
        floorMin_ = std::min (floorMin_, p);                    // block (0.0) shouldn't read as a 0:big spread
        floorMax_ = std::max (floorMax_, p);
    }
    // Stable when the spread stayed within kFloorStableRatio (a narrow, stationary band).
    floorStable_.store (floorMax_ <= floorMin_ * kFloorStableRatio);
}

void MeasurementSession::freezeArmFloor() noexcept {
    armFloor_ = noiseFloor_;                                     // snapshot the pre-sweep floor
    armFloorMilli_.store ((int) std::lround (armFloor_ * 1000.0f));   // publish (peak*1000 idiom)
}

void MeasurementSession::observeBlockPeak (float peak) noexcept {
    if (static_cast<SessionPhase> (phase_.load()) == SessionPhase::Invalid)
        return;   // absorbing

    if (static_cast<SessionPhase> (phase_.load()) == SessionPhase::Idle)
        phase_.store ((int) SessionPhase::Preflight);

    const auto cur = static_cast<SessionPhase> (phase_.load());

    if (cur == SessionPhase::Preflight) {
        // FIRST sweep onset: arm on a sustained RISE above the tracked floor and fire the one re-scope
        // edge. The warm-up counter advances every Preflight block (the floor settling window).
        if (preflightBlocks_ < kArmWarmupBlocks) ++preflightBlocks_;
        // SNR: track the pre-sweep block-peak spread for the floor-confidence guard. Only while a rise
        // isn't underway (armRun_ == 0) so the rising sweep's loud blocks don't widen the floor spread.
        observePreSweepFloor (peak);
        if (advanceArmRun (peak, /*requireWarmup*/ true)) {
            // SNR: freeze + publish the pre-sweep floor at the arm instant (Preflight -> SweepActive).
            freezeArmFloor();
            phase_.store ((int) SessionPhase::SweepActive);
            sweepStarted_.store (true);   // genuine onset -> the single latch clear
            silenceRun_ = 0; sawSignal_ = true;
        }
        return;
    }

    if (cur == SessionPhase::Complete) {
        // Re-arm for the next sweep of the SAME Dirac measurement (right earcup) or a re-take, WITHOUT
        // re-clearing latches: a clip on either earcup must accrue into the one validity window. The
        // floor was reseeded low on the SweepActive->Complete edge, so the right earcup re-arms cleanly;
        // no fresh warm-up is needed (the session is already past Preflight).
        // SNR: this is the SECOND-onset pre-sweep window — track its spread + snapshot its own floor.
        observePreSweepFloor (peak);
        if (advanceArmRun (peak, /*requireWarmup*/ false)) {
            freezeArmFloor();
            phase_.store ((int) SessionPhase::SweepActive);
            silenceRun_ = 0; sawSignal_ = true;
        }
        return;
    }

    // SweepActive: track the silence tail. A sustained below-floor run after signal -> provisional
    // Complete; any above-floor block resets the run so a brief inter-segment gap doesn't end it early.
    if (peak < kSilenceFloorLinear) {
        if (sawSignal_ && ++silenceRun_ >= silenceBlocksNeeded_) {
            phase_.store ((int) SessionPhase::Complete);
            // Reseed the floor low so the right-earcup re-arm (out of Complete) sees a fresh quiet floor
            // and the rise gate triggers on the next sweep instead of an inflated post-sweep floor.
            noiseFloor_ = kSilenceFloorLinear;
            // SNR: re-evaluate the floor-confidence over the NEXT onset's pre-sweep window. The frozen
            // arm-floor (armFloor_/armFloorMilli_) stays published until the second onset overwrites it.
            floorMin_ = 0.0f; floorMax_ = 0.0f; floorStable_.store (false);
        }
    } else {
        silenceRun_ = 0; sawSignal_ = true;
    }
}

bool MeasurementSession::consumeSweepStarted() noexcept {
    return sweepStarted_.exchange (false);
}

void MeasurementSession::markInvalid() noexcept {
    phase_.store ((int) SessionPhase::Invalid);
}

} // namespace eb
