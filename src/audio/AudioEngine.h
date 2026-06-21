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

    // Match-window sweep-SNR fix: the GUI worker (pollReferenceGrade) computes the sweep-to-room-noise SNR from
    // the SAME match-aligned grade window — the path that ACTUALLY fires on a real Dirac log-sweep, unlike the
    // dead level arm — and forwards the result here. publishCompletedSweepSnrDb snapshots the dB so the existing
    // "Low SNR: sweep only N dB over the room noise" status line names it; raiseLowSnr raises the GUIDANCE LowSnr
    // flag (NOT invalidating). Both forward to HealthMonitor; message/worker-thread callers (lock-free stores).
    void publishCompletedSweepSnrDb (float snrDbMin) noexcept;
    void raiseLowSnr() noexcept;

    // AutoPerEar: which earcup is currently being fed to Dirac (0 = left, 1 = right). Drives the GUI
    // "capturing Left/Right" indicator; only meaningful while running in AutoPerEar with signal.
    int autoActiveEar() const noexcept;

    // How much (>= 0 dB) the graph's auto makeup-headroom is currently attenuating the output. The GUI
    // shows this as "add about +N dB on Dirac's Mic gain" so the user compensates for the attenuation
    // EARS Bridge applies for headroom. Updates when a cal loads. Forwards to ProcessingGraph; lock-free.
    float headroomAttenuationDb() const noexcept;

    // ---- Reference-Based Measurement Monitor (Plan 5) ----
    // The GUI sets this true once a validated loopback reference is learned/loaded (and false when it is
    // cleared). It gates the per-sweep grading: the engine only flags a measurement as graded/mismatched
    // when a reference is actually present — with NO reference the workflow state is NotGraded and the GUI
    // shows the honest "not graded - learn a reference" copy (NEVER green). Atomic so the audio thread
    // reads it lock-free at the completed-sweep edge. Message-thread write only.
    void setReferenceLoaded (bool loaded) noexcept;
    bool referenceLoaded() const noexcept;

    // Cosmetic room-floor activity flag (Task 4). True when the most recent capture block's peak exceeds a
    // LOW activity floor (kActivityFloorLinear, ~-50 dBFS, just above room noise). It drives ONLY the GUI's
    // "Sweep in progress..." vs "Listening..." status text and the match poll's SETTLED check — it NEVER
    // gates grading (there is NO absolute level gate in the grade path; the reference MATCH is the detector).
    // Set on the audio thread (single writer, relaxed); read lock-free GUI-side.
    bool gradeSignalPresent() const noexcept;

    // ---- Live grading response buffer (RT-safe; capture thread writes, worker reads) ----
    // REFERENCE-MATCH grade detection (Task 4). The mic RESPONSE (the ACTIVE earcup's channel) is memcpy'd
    // CONTINUOUSLY into a PRE-ALLOCATED ROLLING ring buffer whenever the engine is Running AND a reference is
    // loaded — NOT gated on level at all. The ring holds the last kGradingSeconds (>= 28 s, > the ~25 s Dirac
    // L+R sequence) so the whole sequence is always present.
    //
    // There is NO absolute-level grade trigger. Instead the capture thread, once per ~kGradeSnapshotSeconds,
    // SNAPSHOTS the rolling ring (oldest-to-newest) into the contiguous responseBuffer_ and sets responseReady_
    // (it re-snapshots only after the worker has consumed the prior one). The GUI poll copies that window out
    // (copyGradingResponse), runs eb::referenceMatches over it — the MATCH is the DETECTOR, firing at ANY level
    // — and, only when matched AND the signal has SETTLED (gradeSignalPresent()==false), runs gradeMeasurement.
    // A finger snap / music / a wrong reference fails referenceMatches and is NOT graded. The audio thread does
    // NO deconvolution and NO matching; all heavy work runs off-thread in the poll.
    //
    // The GUI worker calls copyGradingResponse(dst, maxLen) to copy the ready snapshot into ITS OWN buffer
    // (the allocation lives in the GUI, never the engine). Returns the number of samples copied (0 when no
    // fresh response is ready) and clears the ready flag. The reference itself is held by the GUI (from the
    // learn), so the worker has both halves and runs the match + gradeMeasurement off-thread.
    int  copyGradingResponse (float* dst, int maxLen) noexcept;
    // The rate the response buffer was captured at (the active capture rate), for the Farina offsets.
    double gradingResponseRate() const noexcept { return grantedRate_; }
    // Test seam: true once a fresh ring snapshot has been published by the capture thread (the poll grades it).
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

    // ---- Diagnostic getters (read-only, lock-free) — the GUI logs the detector's internals ----
    // lastInputBlockPeak(): the most recent capture block's input peak (max |sample| over L/R), stored on
    // the audio thread at the pk site (the *Milli_ fixed-point idiom). lastMatchCoherence(): the coherence
    // from the last referenceMatches poll — Task 4's match poll writes it via setLastMatchCoherence(); it
    // reads 0 until then. Both back std::atomic<int> milli stores; the getters are pure lock-free reads.
    float lastInputBlockPeak() const noexcept;
    float lastMatchCoherence() const noexcept;
    // Writer for the match coherence (Task 4's poll, message/worker thread). Single writer; lock-free.
    void  setLastMatchCoherence (float coherence) noexcept;

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
    // Pre-allocate (message thread) the live-grading ring + snapshot buffers for kGradingSeconds at `rate`,
    // size the trailing-silence trigger for `blockLen`, and clear the capture-thread trigger scratch.
    // Called from prepare/start; NEVER from the audio thread.
    void allocateResponseBuffer (double rate, int blockLen);

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

    // Plan 5 (Reference Monitor): referenceLoaded_ gates the ring buffering + the periodic snapshot (message-
    // thread write, audio-thread read). gradeSignalPresent_ is the COSMETIC room-floor activity flag (Task 4):
    // the audio thread sets it from the block peak vs kActivityFloorLinear; it drives ONLY the "Sweep in
    // progress..." status and the poll's SETTLED check — it NEVER gates grading. There is NO absolute level
    // gate in the grade path; the reference MATCH (run off-thread in the poll) is the sole detector.
    std::atomic<bool> referenceLoaded_    { false };
    std::atomic<bool> gradeSignalPresent_ { false };

    // Diagnostic getters (Task 2): the *Milli_ fixed-point idiom (value * 1000 in an atomic<int>).
    // lastInputPeakMilli_ is written by the AUDIO thread at the pk site (single writer, relaxed) so the
    // getter reflects the live input level. lastMatchCoherMilli_ is written by Task 4's match poll (single
    // writer, message/worker thread); 0 until then. Both are lock-free reads on the GUI side.
    std::atomic<int> lastInputPeakMilli_  { 0 };
    std::atomic<int> lastMatchCoherMilli_ { 0 };

    // Live grading buffers (Task 4 reference-match detection). BOTH are PRE-ALLOCATED in prepare/start
    // (sized for kGradingSeconds at the active rate) and NEVER resized on the audio thread.
    //   gradeRing_       : a ROLLING ring the capture thread fills CONTINUOUSLY whenever Running + a
    //                      reference is loaded (NOT gated on level). Single writer (capture thread);
    //                      gradeRingWrite_ is the running write head modulo size.
    //   responseBuffer_  : the contiguous SNAPSHOT the capture thread copies the ring into (oldest->newest)
    //                      once per ~kGradeSnapshotSeconds; the poll copies it out via copyGradingResponse()
    //                      and runs the MATCH over it. The capture thread re-snapshots only after the poll
    //                      has consumed the prior one (responseReady_ is the producer->consumer handoff).
    static constexpr double kGradingSeconds      = 28.0;   // >= the ~25 s Dirac L+R sequence, with margin
    // COSMETIC activity floor (Task 4): a LOW floor (~-50 dBFS) just above room noise. The block peak crossing
    // it sets gradeSignalPresent_ (drives ONLY the status text + the poll's settled check). NOT a grade gate.
    static constexpr float  kActivityFloorLinear = 0.003f;  // ~ -50 dBFS
    // The capture thread publishes a fresh ring snapshot for the poll about this often (so the off-thread
    // match always has a recent window). Converted to a per-block count in allocateResponseBuffer.
    static constexpr double kGradeSnapshotSeconds = 0.5;
    std::vector<float> gradeRing_;                    // pre-allocated rolling ring; capture-thread writes
    int                gradeRingWrite_  = 0;          // capture-thread scratch: ring write head (mod size)
    long long          gradeRingFilled_ = 0;          // capture-thread scratch: total samples ever written this run
    int                gradeSnapshotBlocks_  = 0;     // capture-thread scratch: blocks since the last published snapshot
    int                gradeSnapshotNeeded_  = 1;     // configured snapshot cadence in blocks (kGradeSnapshotSeconds)
    std::vector<float> responseBuffer_;               // pre-allocated snapshot; capture-thread writes, worker reads
    std::atomic<int>   responseReadyLen_ { 0 };       // length of the published snapshot
    std::atomic<bool>  responseReady_    { false };   // a fresh ring snapshot is ready for the poll to grade
    // Capture-thread helper: snapshot the rolling ring (oldest->newest) into responseBuffer_ and publish the
    // length + ready flags. RT-safe (two bounded memcpy into pre-allocated storage). Single writer.
    void snapshotGradeRing() noexcept;
    // Capture-thread helper (Task 4): when a reference is loaded, write this block's ACTIVE-EAR mic response
    // into the rolling ring, set the COSMETIC gradeSignalPresent_ from blockPeak, and (on the snapshot cadence,
    // once the poll consumed the prior one) publish a fresh ring snapshot for the off-thread match. It does NO
    // matching/deconvolution and NO absolute-level grade trigger. Called by BOTH the real capture callback and
    // the headless test seam so the two paths stay aligned. blockPeak is the already-computed per-block input
    // peak (so the caller doesn't recompute it). RT-safe (no alloc/lock/syscall).
    void processGradeDetection (const float* respCh, int numSamples, float blockPeak) noexcept;

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
