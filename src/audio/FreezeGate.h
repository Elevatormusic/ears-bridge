#pragma once

namespace eb {

// FreezeGate — the level gate that arms the D6 ClockBridge resample-ratio freeze during a Dirac sweep.
//
// WHY THIS EXISTS: the freeze was driven by MeasurementSession::sweepActive(), a "+12 dB rise above the noise
// floor" level arm that a GRADUAL Dirac log-sweep never trips (the floor estimate chases the ramp up). An
// on-device probe proved the consequence: across a whole measurement the freeze read `frozen=no` on all 590
// samples — it NEVER engaged — so the async-resampler ratio free-ran and the PI fill-controller re-trimmed it
// every block, drifting ~1100 ppm. That continuous fractional-phase creep non-uniformly retimes the sweep and
// fails Dirac's CROSS-sweep phase-consistency check ("imprecise — excluded from phase correction"), while
// leaving within-sweep coherence / THD / SNR untouched (they are phase-blind).
//
// THE GATE: a real measurement sweep block-peak sits ~-1..-4 dBFS, far above the room/chain floor (~-30 dBFS
// peak), so a fixed -24 dBFS gate fires RELIABLY on the sweep and never on the room. Hold ONE ratio across
// BOTH earcups: Dirac sweeps L then R with a quiet gap; a long release HOLD keeps the gate engaged across that
// gap so the ClockBridge snapshots ONE frozen ratio for both sweeps (consistent interaural group delay), then
// releases after the measurement so the PI loop re-centers before the next one. The probe showed this is SAFE:
// fifoFill held 0.493–0.506 (dead-center, far from the 0.10/0.90 emergency bounds) with zero under/overruns,
// so a held ratio cannot drift the FIFO to a forced retime within a measurement.
//
// Pure + allocation-free; the capture thread owns the state. Unit-tested in tests/test_freezegate.cpp.
struct FreezeGateState {
    int  loudRun     = 0;       // consecutive blocks at/above the gate (attack debounce)
    long holdSamples = 0;       // samples remaining in the release hold (bridges the inter-sweep gap)
    bool on          = false;   // the published gate (drives bridge.setSweepActive)
};

// Tunables (shared with the tests so the two never silently diverge).
inline constexpr float  kFreezeGateLin      = 0.0631f;   // ~-24 dBFS block peak: a sweep, safely above the floor
inline constexpr int    kFreezeAttackBlocks = 3;          // consecutive loud blocks before the gate arms
inline constexpr double kFreezeReleaseSecs  = 6.0;        // release hold: bridges the L<->R gap; drops after a run

// Advance the gate from ONE capture block: this block's PEAK (linear sample magnitude, 0..1+), the block length,
// and the capture rate (for the time-based release). Returns true while the ratio should be frozen this block.
[[nodiscard]] inline bool freezeGateStep (FreezeGateState& s, float blockPeakLin, int numSamples, double captureRate) noexcept {
    if (blockPeakLin >= kFreezeGateLin) {
        if (s.loudRun < kFreezeAttackBlocks) ++s.loudRun;
        if (s.loudRun >= kFreezeAttackBlocks) {                       // armed: (re)charge the release hold
            s.on = true;
            s.holdSamples = (long) (kFreezeReleaseSecs * captureRate);
        }
    } else {
        s.loudRun = 0;
        if (s.holdSamples > 0) {                                      // quiet: bleed the release hold down
            s.holdSamples -= numSamples;
            if (s.holdSamples <= 0) { s.holdSamples = 0; s.on = false; }
        }
    }
    return s.on;
}

} // namespace eb
