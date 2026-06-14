#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/DeviceManager.h"
#include "audio/ClockBridge.h"
#include "audio/HealthMonitor.h"
#include "audio/EngineTypes.h"
#include "audio/ProcessingGraph.h"
#include "audio/CombineMode.h"
#include "audio/LrVerify.h"
#include "audio/AsioFallback.h"
#include "audio/CalBinder.h"
#include "platform/AggregateDevice_mac.h"   // portable header; macOS-gated .mm (Plan 4 Task 7)
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

    // --- Plan 4 additions (additive) ---
    // Detailed sticky health flags for the GUI status light / tooltip (reads the same `hm`).
    HealthFlag     healthFlags() const noexcept;
    bool           cleanCapture() const noexcept;
    DipGainProfile gainProfile() const noexcept;   // for the "lower/raise DIP gain" hint

    // Drive a short tone into ONE earcup (user routes playback to that earcup) and report which
    // mic channel responded. Runs only while Stopped; uses a dedicated short capture-only open.
    // Returns the verdict; the GUI presents it. Non-blocking variant: begin + poll.
    void     beginLrVerify (Ear earUnderTest);
    LrResult lrVerifyResult() const noexcept;
    bool     lrVerifyComplete() const noexcept;

    // Last ASIO->WASAPI/CoreAudio fallback message (empty when no fallback occurred).
    juce::String lastFallbackMessage() const { return lastFallbackMessage_; }

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

    LrVerify      lrVerify_;        // Plan 4 (pure state machine)
    CalBinder     calBinder_;       // Plan 4 (re-bind cal across re-enumeration)
    AggregateDevice aggregate_;     // Plan 4 Task 7: macOS CoreAudio aggregate (no-op on Windows)
    juce::String  lastFallbackMessage_;   // surfaced by the GUI after an ASIO fallback
    int           lastUnderruns_ = 0;     // render-thread-only: ClockBridge underrun count seen last block
    long long     lastDroppedCapture_ = 0;// capture-thread-only: ClockBridge dropped-frame count seen last block
    bool          usingAggregate_ = false; // macOS: true when the CoreAudio aggregate path is active
                                           // (Task 7 sets it; the render callback reads it). Always
                                           // false on Windows.

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
