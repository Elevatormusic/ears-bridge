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
    // Defensive engine backstop (P0-02): reconfiguration (a FIR/cal swap) is only safe when the audio
    // callback is NOT actively processing. True while Stopped/Error; false only while Running (live
    // capture). applyCalibrationGeneration() consults this and no-ops while Running so an in-flight Dirac
    // measurement can never be corrupted by a mid-sweep FIR swap (the GUI also freezes the controls).
    bool reconfigAllowed() const noexcept { return status() != EngineStatus::Running; }
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

    // SNR: the min-ear sweep-to-noise dB of the sweep that just completed (the value that drives the
    // LowSnr guidance flag). Recomputed GUI-side from the already-published lock-free atomics
    // (armNoiseFloor / maxSweepPeakL/R / completedFloorStable) via eb::evaluateSnr, so the status line
    // can name the dB without the engine carrying extra state. 0 until a sweep completes. Message-thread
    // read only; the flag (health().flags & LowSnr) is the source of truth for WHETHER to warn.
    float completedSweepSnrDb() const noexcept;

    // AutoPerEar: which earcup is currently being fed to Dirac (0 = left, 1 = right). Drives the GUI
    // "capturing Left/Right" indicator; only meaningful while running in AutoPerEar with signal.
    int autoActiveEar() const noexcept;

    // ---- Reference-Based Measurement Monitor (Plan 5) ----
    // The GUI sets this true once a validated loopback reference is learned/loaded (and false when it is
    // cleared). It gates the per-sweep grading: the engine only flags a measurement as graded/mismatched
    // when a reference is actually present — with NO reference the workflow state is NotGraded and the GUI
    // shows the honest "not graded - learn a reference" copy (NEVER green). Atomic so the audio thread
    // reads it lock-free at the completed-sweep edge. Message-thread write only.
    void setReferenceLoaded (bool loaded) noexcept;
    bool referenceLoaded() const noexcept;

    // Drained (read-and-cleared) by the GUI worker each poll: true when a sweep COMPLETED with a reference
    // loaded, i.e. there is a measurement to grade OFF the audio thread. The audio thread only sets this
    // one atomic at the edge (no deconvolution on the audio thread); the worker runs gradeMeasurement and
    // calls publishReferenceGrade. The per-segment mic-response is buffered DURING the sweep into a
    // pre-allocated response buffer (capture thread, memcpy only) and snapshotted at this edge; the GUI
    // worker copies it out via copyGradingResponse() and grades it OFFLINE.
    bool consumePendingGrade() noexcept;

    // ---- Live grading response buffer (RT-safe; capture thread writes, worker reads) ----
    // During SweepActive the capture callback memcpy's the mic RESPONSE (left channel) into a
    // PRE-ALLOCATED buffer (sized for ~25 s at the active rate in prepare/start — NEVER on the audio
    // thread). At the SweepActive->Complete edge the written length is snapshotted (responseReadyLen_)
    // and responseReady_ is set, so the GUI worker can copy out a complete, consistent response. The
    // audio thread does NO deconvolution — it only memcpy's and stores two atomics.
    //
    // The GUI worker calls copyGradingResponse(dst, maxLen) to copy the ready response into ITS OWN
    // buffer (the allocation lives in the GUI, never the engine). Returns the number of samples copied
    // (0 when no fresh response is ready) and clears the ready flag. The reference itself is held by the
    // GUI (from the learn), so the worker has both halves and runs gradeMeasurement off-thread.
    int  copyGradingResponse (float* dst, int maxLen) noexcept;
    // The rate the response buffer was captured at (the active capture rate), for the Farina offsets.
    double gradingResponseRate() const noexcept { return grantedRate_; }
    // Test seam: true once a fresh response snapshot is ready to grade (set at the sweep-complete edge).
    bool gradingResponseReady() const noexcept { return responseReady_.load(); }

    // Publish a reference-grade verdict (message/worker thread, OFFLINE). Snapshots the workflow state +
    // IR-SNR + THD TOGETHER (the SNR lesson) and raises the matching GUIDANCE flag (RefMismatch when the
    // match-gate failed, RefLowQuality when matched-but-suspect) — neither invalidates the capture.
    void publishReferenceGrade (int refMonState, float irSnrDb, float thdPercent,
                                bool mismatch, bool lowQuality) noexcept;

    // The published reference-grade snapshot (lock-free; the int-milli idiom). refMonState() is
    // RefMonState's underlying int (0 == NotLearned). 0 / 0.0 until a grade is published.
    int   refMonState()   const noexcept;
    float refIrSnrDb()    const noexcept;
    float refThdPercent() const noexcept;

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
    // SNR review-fix test accessors: the per-ear sweep-peak numerators (so a test can assert the stale
    // left peak does NOT leak into the second earcup's sweep after resetSweepPeaks() runs per Complete).
    float maxSweepPeakLForTest() const noexcept { return hm.maxSweepPeakL(); }
    float maxSweepPeakRForTest() const noexcept { return hm.maxSweepPeakR(); }

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
    // Pre-allocate (message thread) the live-grading response buffer for kGradingSeconds at `rate` and
    // clear the per-sweep capture state. Called from prepare/start; NEVER from the audio thread.
    void allocateResponseBuffer (double rate);

    DeviceManager      devices;
    ProcessingGraph    graph;
    ClockBridge        bridge;
    HealthMonitor      hm;
    MeasurementSession session_;   // D5: re-armable level-threshold sweep-window state machine

    LrVerify      lrVerify_;        // Plan 4 (pure state machine; touched only on the verify audio thread)
    std::atomic<int>  verifyResult_ { (int) LrResult::Pending };  // lock-free verdict snapshot for the GUI
    std::atomic<bool> verifyActive_ { false };
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

    // Plan 5 (Reference Monitor): referenceLoaded_ gates per-sweep grading (message-thread write, audio-
    // thread read). pendingGrade_ is set at the completed-sweep edge when a reference is loaded and drained
    // by the GUI worker (which runs the OFFLINE deconvolution + gradeMeasurement). The audio thread does NO
    // deconvolution — it only stores these two atomics at the edge.
    std::atomic<bool> referenceLoaded_ { false };
    std::atomic<bool> pendingGrade_    { false };

    // Live grading response buffer. responseBuffer_ is PRE-ALLOCATED in prepare/start (sized for
    // kGradingSeconds at the active rate) and NEVER resized on the audio thread. The capture thread is
    // the SINGLE writer: it memcpy's the mic response during SweepActive (responseWriteIdx_ is
    // capture-thread scratch) and, at the SweepActive->Complete edge, snapshots the written length into
    // responseReadyLen_ and sets responseReady_. The GUI worker reads them via copyGradingResponse().
    static constexpr double kGradingSeconds = 25.0;   // L-then-R Dirac sweep sequence is ~25 s
    std::vector<float> responseBuffer_;               // pre-allocated; capture-thread writes, worker reads
    int                responseWriteIdx_ = 0;         // capture-thread scratch: next write offset this sweep
    std::atomic<int>   responseReadyLen_ { 0 };       // snapshot of the captured length at the complete edge
    std::atomic<bool>  responseReady_    { false };   // a fresh response snapshot is ready to grade

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
