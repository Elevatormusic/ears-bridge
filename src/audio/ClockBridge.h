#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "audio/PolyphaseResampler.h"
#include <atomic>
#include <vector>
namespace eb {

// Single-producer (capture cb) / single-consumer (render cb) mono bridge across two
// free-running clocks. pushCapture() writes capture-rate samples into a lock-free FIFO;
// pullRender() reads render-rate samples out through an async SRC whose ratio is the
// nominal captureRate/renderRate trimmed by a PI loop on smoothed fill. No alloc/lock
// on either path after prepare().
class ClockBridge {
public:
    // The PI fill-control setpoint AND the startup prime depth, as a fraction of capacity. These
    // MUST be the same value: pullRender() steers fill toward this target, so priming to anything
    // else makes the loop chase from a non-equilibrium fill and drags the SRC ratio off-nominal
    // for seconds (ExcessDrift). Single-sourced here so the two sites can never silently diverge.
    static constexpr double kTargetFill = 0.5;

    // Hard FIFO-fill band tolerated WHILE FROZEN (raw fill, not the steering smoother). Tied to
    // kTargetFill so a future setpoint change keeps the band balanced. Outside it the held ratio can no
    // longer hold the FIFO and a real drop/insert is imminent -> emergency correction.
    static constexpr double kFreezeBand  = 0.40;                      // ± headroom around the setpoint
    static constexpr double kFreezeFloor = kTargetFill - kFreezeBand; // 0.10: near-empty (interpolator starve)
    static constexpr double kFreezeCeil  = kTargetFill + kFreezeBand; // 0.90: near-full (producer overrun)

    void prepare (double captureRate, double renderRate, int channels, int capacityFrames);
    void pushCapture (const float* mono, int numFrames);  // producer
    int  pullRender  (float* out, int numFrames);          // consumer; returns frames written
    void setRenderRate (double);
    void reset();
    // Pre-fill the FIFO with `silentFrames` of silence (clamped to free space) before the streams
    // start, so the consumer begins from the PI controller's half-full target instead of empty.
    // Prevents the one-time startup underrun (render pulling before capture has primed) that would
    // otherwise latch FifoStarved + ExcessDrift for the whole run. Call once, after reset(), on the
    // setup thread before start(); the write is lock-free and allocation-free (ring already sized).
    void prime (int silentFrames);
    void primeToTarget();   // prime to kTargetFill * capacity (the PI setpoint); use this in the engine

    // ---- D6: freeze the SRC ratio during a Dirac sweep --------------------------------------
    // The PI fill-controller normally re-trims the resample ratio every render block; during a sweep
    // that continuous sub-0.5% creep nonuniformly retimes Dirac's log sweep (smears the recovered IR)
    // while staying under the drift latch -> reads "clean". While the sweep is active we HOLD the
    // converged trim (publishedRatio stops moving), absorb short-term clock drift with the FIFO
    // headroom, and recenter only between sweeps. If drift outruns the headroom (raw FIFO fill crosses a
    // hard near-empty/near-full bound) a real retiming is forced; we latch that as an emergency edge so
    // the engine can invalidate, rather than silently steer through it. Driven live by D5's session via
    // AudioEngine (setSweepActive(session_.sweepActive()) each capture block).
    void setSweepActive (bool active) noexcept;   // any thread; consumer reads it
    bool sweepActive() const noexcept;
    bool consumeEmergencyCorrection() noexcept;   // read-and-clear: true once if a hard bound crossed while frozen

    double fifoFill()  const;   // 0..1 fraction of capacity currently buffered (smoothed)
    int    underruns() const;
    int    overruns()  const;   // count of overrun EVENTS (one per FIFO-full pushCapture); diagnostic
    long long droppedCaptureFrames() const;  // cumulative producer FRAMES dropped on FIFO-full
    double currentRatio() const;   // current trimmed capture:render resample ratio
    double currentGroupDelay() const noexcept { return resampler_.groupDelay(); }   // constant resampler latency (samples)
    double avgRatioTrim() const noexcept { return avgRatioTrim_; }   // TEST: the running mean the freeze snapshots

private:
    // FIFO of mono float samples.
    juce::AbstractFifo fifo { 1 };
    std::vector<float> ring;            // capacity samples
    int capacity = 0;

    // SRC consumer-side scratch + measurement-grade windowed-sinc polyphase resampler. The resampler is
    // STATELESS (vs the old stateful Lagrange) so ClockBridge owns the fractional read pointer readPhase_
    // and retains L/2-1 history samples in the FIFO between blocks.
    eb::PolyphaseResampler resampler_;
    double readPhase_ = 0.0;            // fractional input position of the next output (consumer thread only)
    std::vector<float> srcInput;        // peeked from FIFO, fed to the resampler
    double captureRate = 48000.0, renderRate = 48000.0;
    static constexpr double kMaxRatio       = 4.0;      // 192k -> 48k, the worst-case input:output ratio
    static constexpr int    kMaxRenderBlock = 8192;     // upper bound on pullRender numFrames (scratch sizing)

    // PI fill-control state (consumer thread only).
    double smoothedFill = 0.5;          // fraction
    double ratioTrim    = 1.0;          // multiplies nominal ratio
    double avgRatioTrim_ = 1.0;         // running mean of ratioTrim ~= the TRUE clock ratio; the freeze snapshots
                                        // THIS (not the instantaneous, possibly-off PI value) so the held ratio
                                        // carries no sustained offset that would accumulate a HF timing error across
                                        // a sweep. alpha = max(0.001, 1/n): a RUNNING MEAN early (converges in a
                                        // second or two, so a sweep right after start() doesn't freeze a stale 1.0),
                                        // settling to a slow ~10 s EMA once warm (to track slow crystal drift).
    long   avgCount_     = 0;           // free-running blocks seen since reset -> drives the 1/n warmup alpha
    double integ        = 0.0;

    std::atomic<int>    underrunCount { 0 };
    std::atomic<int>    overrunCount  { 0 };
    std::atomic<long long> droppedFrameCount { 0 };   // producer frames lost to FIFO-full (cumulative)
    std::atomic<double> publishedFill  { 0.5 };
    std::atomic<double> publishedRatio { 1.0 };   // last trimmed capture:render ratio (for currentRatio())

    // D6 freeze state. sweepActive_/emergencyCorrection_ are cross-thread atomics; frozenRatio_/
    // freezeArmed_ are CONSUMER-THREAD-ONLY scratch (written/read only inside pullRender).
    std::atomic<bool>   sweepActive_ { false };
    std::atomic<bool>   emergencyCorrection_ { false };   // edge, drained by consumeEmergencyCorrection()
    bool   freezeArmed_  = false;   // false until the first frozen block snapshots the trim
    double frozenRatio_  = 1.0;     // the held capture:render ratio while frozen
};

} // namespace eb
