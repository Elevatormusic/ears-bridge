#include "audio/MeasurementSession.h"
#include <algorithm>

namespace eb {

void MeasurementSession::reset() noexcept {
    phase_.store ((int) SessionPhase::Idle);
    sweepStarted_.store (false);
    armRun_ = 0; silenceRun_ = 0; sawSignal_ = false;
}

void MeasurementSession::configure (int blockSize, double sampleRate) noexcept {
    if (blockSize > 0 && sampleRate > 0.0) {
        const long n = std::lround (kSilenceCompleteSeconds * sampleRate / (double) blockSize);
        silenceBlocksNeeded_ = (int) std::max (1L, n);
    }
}

bool MeasurementSession::advanceArmRun (float peak) noexcept {
    if (peak >= kSweepStartLinear) {
        if (++armRun_ >= kArmSustainBlocks) { armRun_ = 0; return true; }
    } else {
        armRun_ = 0;
    }
    return false;
}

void MeasurementSession::observeBlockPeak (float peak) noexcept {
    if (static_cast<SessionPhase> (phase_.load()) == SessionPhase::Invalid)
        return;   // absorbing

    if (static_cast<SessionPhase> (phase_.load()) == SessionPhase::Idle)
        phase_.store ((int) SessionPhase::Preflight);

    const auto cur = static_cast<SessionPhase> (phase_.load());

    if (cur == SessionPhase::Preflight) {
        // FIRST sweep onset: arm on a sustained rise and fire the one re-scope edge.
        if (advanceArmRun (peak)) {
            phase_.store ((int) SessionPhase::SweepActive);
            sweepStarted_.store (true);   // genuine onset -> the single latch clear
            silenceRun_ = 0; sawSignal_ = true;
        }
        return;
    }

    if (cur == SessionPhase::Complete) {
        // Re-arm for the next sweep of the SAME Dirac measurement (right earcup) or a re-take, WITHOUT
        // re-clearing latches: a clip on either earcup must accrue into the one validity window.
        if (advanceArmRun (peak)) {
            phase_.store ((int) SessionPhase::SweepActive);
            silenceRun_ = 0; sawSignal_ = true;
        }
        return;
    }

    // SweepActive: track the silence tail. A sustained below-floor run after signal -> provisional
    // Complete; any above-floor block resets the run so a brief inter-segment gap doesn't end it early.
    if (peak < kSilenceFloorLinear) {
        if (sawSignal_ && ++silenceRun_ >= silenceBlocksNeeded_)
            phase_.store ((int) SessionPhase::Complete);
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
