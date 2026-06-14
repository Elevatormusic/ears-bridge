#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/DeviceManager.h"
#include "audio/ClockBridge.h"
#include "audio/HealthMonitor.h"
#include "audio/EngineTypes.h"
#include "audio/ProcessingGraph.h"
#include "audio/CombineMode.h"
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"
#include <atomic>
#include <memory>
namespace eb {

// Compute the FIR tap count for a given active sample rate: 8192 @ 48k, scaled by
// rate/48000, rounded UP to the next power of two (8192@48k, 16384@96k, 32768@192k).
int firTapsForRate (double sampleRate);

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // ---- Device / format selection (call while Stopped) ----
    std::vector<DeviceId> inputDevices()  const;
    std::vector<DeviceId> outputDevices() const;
    std::vector<double>   supportedSampleRates (const DeviceId&) const;
    std::vector<int>      supportedBitDepths   (const DeviceId&) const;
    void setInput  (const DeviceId&);
    void setOutput (const DeviceId&);
    void setSampleRate (double sr);
    void setOutputBitDepth (int bits);   // 16/24/32 (24 default); best-effort request, not enforced

    // ---- DSP config ----
    void setLeftCalFir  (juce::AudioBuffer<float> fir);   // hot-swappable while Running
    void setRightCalFir (juce::AudioBuffer<float> fir);
    void setCombineMode (eb::CombineMode);

    // Convenience: design + load the FIR from a parsed cal file at the active rate.
    void loadLeftCal  (const CalFile&);
    void loadRightCal (const CalFile&);

    // ---- Transport ----
    bool start (juce::String& errorOut);
    void stop();
    EngineStatus status() const;

    // ---- Telemetry (lock-free snapshots) ----
    Levels levels() const;
    Health health() const;

    // ---- Headless test seam ----
    // Re-prepare the graph at the given rate and process one injected 2-ch block to mono.
    // No device I/O. Spins until the async Convolution IR load + gain ramp settles.
    void prepareForTest (double sampleRate, int blockSize);
    void processCaptureBlockForTest (const float* inL, const float* inR,
                                     float* outMono, int numSamples);

private:
    void rescanDevices();
    static int nextPow2 (int v);

    DeviceManager      devices;
    ProcessingGraph    graph;
    ClockBridge        bridge;
    HealthMonitor      hm;

    DeviceId inputId, outputId;
    double   activeRate = 48000.0;
    int      outputBits = 24;
    int      blockSize  = 0;

    std::atomic<int> engineStatus { (int) EngineStatus::Stopped };

    // Audio callback adapters (own no state beyond pointers back to this).
    struct CaptureCallback;  struct RenderCallback;
    std::unique_ptr<CaptureCallback> captureCb;
    std::unique_ptr<RenderCallback>  renderCb;
    friend struct CaptureCallback;   friend struct RenderCallback;
};

} // namespace eb
