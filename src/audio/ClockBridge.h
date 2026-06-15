#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
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

    double fifoFill()  const;   // 0..1 fraction of capacity currently buffered (smoothed)
    int    underruns() const;
    int    overruns()  const;   // count of overrun EVENTS (one per FIFO-full pushCapture); diagnostic
    long long droppedCaptureFrames() const;  // cumulative producer FRAMES dropped on FIFO-full
    double currentRatio() const;   // current trimmed capture:render resample ratio

private:
    // FIFO of mono float samples.
    juce::AbstractFifo fifo { 1 };
    std::vector<float> ring;            // capacity samples
    int capacity = 0;

    // SRC consumer-side scratch + interpolator.
    juce::LagrangeInterpolator src;
    std::vector<float> srcInput;        // drained from FIFO, fed to interpolator
    double captureRate = 48000.0, renderRate = 48000.0;

    // PI fill-control state (consumer thread only).
    double smoothedFill = 0.5;          // fraction
    double ratioTrim    = 1.0;          // multiplies nominal ratio
    double integ        = 0.0;

    std::atomic<int>    underrunCount { 0 };
    std::atomic<int>    overrunCount  { 0 };
    std::atomic<long long> droppedFrameCount { 0 };   // producer frames lost to FIFO-full (cumulative)
    std::atomic<double> publishedFill  { 0.5 };
    std::atomic<double> publishedRatio { 1.0 };   // last trimmed capture:render ratio (for currentRatio())
};

} // namespace eb
