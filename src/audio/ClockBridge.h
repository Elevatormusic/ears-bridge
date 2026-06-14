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
    void prepare (double captureRate, double renderRate, int channels, int capacityFrames);
    void pushCapture (const float* mono, int numFrames);  // producer
    int  pullRender  (float* out, int numFrames);          // consumer; returns frames written
    void setRenderRate (double);
    void reset();

    double fifoFill()  const;   // 0..1 fraction of capacity currently buffered (smoothed)
    int    underruns() const;
    int    overruns()  const;   // count of overrun EVENTS (one per FIFO-full pushCapture)
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
