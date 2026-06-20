#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/DeviceManager.h"
#include "audio/ClockBridge.h"
#include "audio/HealthMonitor.h"
#include "audio/MeasurementSession.h"
#include "audio/EngineTypes.h"
#include "audio/ProcessingGraph.h"
#include "audio/CombineMode.h"
#include "audio/LrVerify.h"
#include "audio/AsioFallback.h"
#include "audio/CalBinder.h"
#include "audio/CalibrationGeneration.h"
#include "platform/AggregateDevice_mac.h"   // portable header; macOS-gated .mm (Plan 4 Task 7)
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"
#include "audio/FirTaps.h"   // firTapsForRate() — single source of truth (also used by the GUI)
#include <atomic>
#include <memory>
namespace eb {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Fired (message thread) when the OS device list changes (hot-plug). Set by the GUI to
    // refresh its pickers without a restart.
    std::function<void()> onDevicesChanged;

    // ---- Device / format selection (call while Stopped) ----
    std::vector<DeviceId> inputDevices()  const;
    std::vector<DeviceId> outputDevices() const;
    std::vector<double>   supportedSampleRates (const DeviceId&);   // non-const: may create a transient device to query rates
    std::vector<int>      supportedBitDepths   (const DeviceId&) const;
    void setInput  (const DeviceId&);
    void setOutput (const DeviceId&);
    void setSampleRate (double sr);
    void setOutputBitDepth (int bits);   // 16/24/32 (24 default); best-effort request, not enforced

    // ---- DSP config ----
    void setLeftCalFir  (juce::AudioBuffer<float> fir);   // hot-swappable while Running
    void setRightCalFir (juce::AudioBuffer<float> fir);
    void clearLeftCalFir();    // restore a neutral (unity) FIR + reset that ear's auto-headroom
    void clearRightCalFir();
    void setCombineMode (eb::CombineMode);
    void setOutputTrimDb (double db);   // output level trim (<= 0 dB), applied live to the mono output

    // Convenience: design + load the FIR from a parsed cal file at the active rate.
    void loadLeftCal  (const CalFile&);
    void loadRightCal (const CalFile&);

    // ---- Calibration generation lifecycle (message thread) ----
    // A monotonic generation token guards the Start gate so it can never be satisfied by a stale,
    // half-installed, or invalid calibration. The GUI bumps requestedGeneration() when it posts an
    // off-thread build job, hands the finished CalibrationGeneration to applyCalibrationGeneration(),
    // and the gate (calibrationApplied()) is satisfied only when requested == built == applied and the
    // applied generation is valid. The ids are std::atomic<int> so the GUI gate reads them without a
    // lock; the audio thread never touches them. appliedGen_ is written on the message thread only.
    void setRequestedGeneration (int id) noexcept;       // GUI: bump when posting a build job
    // Always records builtGenId_/appliedGen_ (so the diagnostic surfaces). ONLY when gen.valid does it
    // install the FIR pair (graph.setFirPair) and advance appliedGenId_ — an invalid generation leaves
    // the graph at its prior state and keeps the gate closed. Message thread only.
    void applyCalibrationGeneration (CalibrationGeneration gen);
    int  requestedGeneration() const noexcept;
    int  builtGeneration()     const noexcept;
    int  appliedGeneration()   const noexcept;
    bool calibrationApplied()  const noexcept;           // the Start-gate readiness predicate
    juce::String calibrationDiagnostic() const;          // appliedGen_.diagnostic (empty when valid)

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

    // D5: the measurement-session phase (Idle/Preflight/SweepActive/Complete/Invalid). The GUI gates
    // its clean/invalid wording on this so pre-/post-sweep room events aren't scored as the sweep.
    SessionPhase sessionPhase() const noexcept { return session_.phase(); }
    bool sweepActive()         const noexcept { return session_.sweepActive(); }   // D6 consumer

    // Edge-triggered raw-input clip since the last poll (self-clearing; drives the GUI gain warning).
    bool consumeRecentInputClip() noexcept;

    // True once the input reached a healthy capture level this run (drives the GUI "level low"
    // guidance so a too-quiet capture can't masquerade as "clean").
    bool reachedGoodLevel() const noexcept;

    // AutoPerEar: which earcup is currently being fed to Dirac (0 = left, 1 = right). Drives the GUI
    // "capturing Left/Right" indicator; only meaningful while running in AutoPerEar with signal.
    int autoActiveEar() const noexcept;

    // Device-loss handling. A capture/render device that the OS removes mid-run (unplug, sleep,
    // gain-DIP re-enumerate) calls audioDeviceStopped(); we latch deviceDied_ there (never tear down
    // from inside that callback -- re-entrant with JUCE's close()). The GUI drains consumeDeviceDied()
    // on its timer and, if still Running, calls onDeviceLost() to tear down cleanly and flip to Error
    // (so the status light stops falsely showing a clean run on a dead stream).
    bool consumeDeviceDied() noexcept;
    void onDeviceLost();

    // The rate/depth the devices were actually GRANTED at open (WASAPI shared mode can override the
    // request). The GUI compares these to the user's selection and warns on a silent downgrade.
    double grantedSampleRate()    const noexcept { return grantedRate_; }
    int    grantedOutputBitDepth() const noexcept;

    // D2 raw-rail snapshot: captured at the last successful start() from the DeviceManager (was the
    // EARS input running at the endpoint mix rate, i.e. no OS SRC on our stream?). Default = unverified;
    // reset to {} in stop()/onDeviceLost() so a stale snapshot never outlives its device.
    RawRailState rawRail() const noexcept;

    // Drive a short tone into ONE earcup (user routes playback to that earcup) and report which
    // mic channel responded. Runs only while Stopped; opens a dedicated capture-only stream that
    // feeds the LrVerify state machine, then the GUI polls the verdict. begin -> poll complete ->
    // read result -> end (closes the stream). begin returns false (with errorOut) if it can't open.
    bool     beginLrVerify (Ear earUnderTest, juce::String& errorOut);
    void     endLrVerify();                       // stop + close the verify capture stream
    bool     lrVerifyActive() const noexcept;
    LrResult lrVerifyResult() const noexcept;     // lock-free snapshot (atomic)
    bool     lrVerifyComplete() const noexcept;

    // Last ASIO->WASAPI/CoreAudio fallback message (empty when no fallback occurred).
    juce::String lastFallbackMessage() const { return lastFallbackMessage_; }

    // ---- Headless test seam ----
    // Re-prepare the graph at the given rate and process one injected 2-ch block to mono.
    // No device I/O. Spins until the async Convolution IR load + gain ramp settles.
    void prepareForTest (double sampleRate, int blockSize);
    void processCaptureBlockForTest (const float* inL, const float* inR,
                                     float* outMono, int numSamples);
    bool bridgeSweepFrozen() const noexcept { return bridge.sweepActive(); }   // D6 test accessor

    // D8: directly call hm.checkFormatChange with the given values, simulating a mid-run OS
    // format renegotiation, so tests can verify that AudioEngine::health() surfaces FormatChanged.
    // No device I/O. Only valid after prepareForTest() or start() (which register the prepared format).
    void simulateFormatChangeForTest (double sampleRate, int bitDepth, int numChannels) noexcept;

    // ---- R22 callback-level test seam ----
    // Prepares the SAME engine state start() builds (graph + ClockBridge primed to target + HealthMonitor),
    // sizes the production callbacks' scratch, and lets a test drive the REAL CaptureCallback/RenderCallback
    // bodies with synthetic buffers. No device I/O, no new thread. nominalRatio is sampleRate:sampleRate (1.0).
    void prepareCallbacksForTest (double sampleRate, int blockSize, int fifoCapacity);
    void driveCaptureCallback (const float* inL, const float* inR, int numSamples);
    // Drive the capture callback with a single input channel to exercise the numIn<2 Xrun guard.
    void driveCaptureCallbackMono (const float* in0, int numSamples);
    void driveRenderCallback  (float* outL, float* outR, int numSamples);

private:
    void rescanDevices();
    static int nextPow2 (int v);

    DeviceManager      devices;
    ProcessingGraph    graph;
    ClockBridge        bridge;
    HealthMonitor      hm;
    MeasurementSession session_;   // D5: re-armable level-threshold sweep-window state machine

    LrVerify      lrVerify_;        // Plan 4 (pure state machine; touched only on the verify audio thread)
    std::atomic<int>  verifyResult_ { (int) LrResult::Pending };  // lock-free verdict snapshot for the GUI
    std::atomic<bool> verifyActive_ { false };
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
    double   grantedRate_ = 48000.0;   // capture rate actually granted at the last successful start()
    RawRailState rawRail_;             // D2: captured once per successful start(); reset in stop()/onDeviceLost()
    int      outputBits = 24;
    int      blockSize  = 0;

    std::atomic<int>  engineStatus { (int) EngineStatus::Stopped };
    std::atomic<bool> deviceDied_  { false };   // set on the AUDIO-DEVICE thread from audioDeviceStopped()

    // Calibration generation lifecycle. The three ids are atomic so the GUI gate reads them lock-free
    // (the audio thread never reads them); appliedGen_ is written on the message thread only.
    // requestedGenId_ : last id the GUI asked to build. builtGenId_ : last id applyCalibrationGeneration
    // processed (valid OR invalid). appliedGenId_ : last id whose FIRs were actually installed (valid
    // only; 0 = none). The gate is closed unless all three match, are non-zero, and appliedGen_.valid.
    std::atomic<int> requestedGenId_ { 0 };
    std::atomic<int> builtGenId_     { 0 };
    std::atomic<int> appliedGenId_   { 0 };
    CalibrationGeneration appliedGen_;          // last generation handed in (valid or invalid); message thread only

    // Audio callback adapters (own no state beyond pointers back to this).
    struct CaptureCallback;  struct RenderCallback;  struct VerifyCallback;
    std::unique_ptr<CaptureCallback> captureCb;
    std::unique_ptr<RenderCallback>  renderCb;
    std::unique_ptr<VerifyCallback>  verifyCb;
    friend struct CaptureCallback;   friend struct RenderCallback;   friend struct VerifyCallback;
};

} // namespace eb
