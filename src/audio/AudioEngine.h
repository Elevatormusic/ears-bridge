#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/DeviceManager.h"
#include "audio/ClockBridge.h"
#include "audio/FreezeGate.h"
#include "audio/HealthMonitor.h"
#include "audio/MeasurementSession.h"
#include "audio/EngineTypes.h"
#include "audio/ProcessingGraph.h"
#include "audio/CombineMode.h"
#include "audio/LrVerify.h"
#include "audio/AsioFallback.h"
#include "audio/CalibrationGeneration.h"
#include "audio/RefMonitor.h"   // Plan 5: RefMonState (the per-ear published-grade enum)
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

    // Noise-floor primitive: the measured ambient floor. Averaged (user-facing, dBFS at the EARS capsule;
    // becomes dB SPL once the calibration feature applies the cal-file sense factor) + a validity gate.
    float noiseFloorDbAveraged() const noexcept;
    bool  noiseFloorValid()      const noexcept;

    // Match-window sweep-SNR fix: the GUI worker (pollReferenceGrade) computes the sweep-to-room-noise SNR from
    // the SAME match-aligned grade window — the path that ACTUALLY fires on a real Dirac log-sweep, unlike the
    // dead level arm — and forwards the result here. publishCompletedSweepSnrDb snapshots the dB so the existing
    // "Low SNR: sweep only N dB over the room noise" status line names it; raiseLowSnr raises the GUIDANCE LowSnr
    // flag (NOT invalidating). Both forward to HealthMonitor; message/worker-thread callers (lock-free stores).
    // Per-ear (ear 0 = LEFT, 1 = RIGHT): snapshots THAT ear's completed-sweep SNR into the per-ear store
    // (so the per-ear status line names the exact dB) AND forwards the value to HealthMonitor's combined
    // completedSnrDb()/the existing status line. The no-arg overload publishes the LEFT ear.
    void publishCompletedSweepSnrDb (int ear, float snrDbMin) noexcept;
    void publishCompletedSweepSnrDb (float snrDbMin) noexcept { publishCompletedSweepSnrDb (0, snrDbMin); }
    void raiseLowSnr() noexcept;

    // Gain-staging readout: the RAW input SAMPLE PEAK of that ear's last graded sweep, in dBFS. The grade window
    // carries RAW pre-processing capture, so this is how hot the EARS mic float ran — NOT clamped at 0 dB, so a
    // float that overshot full-scale reads as a POSITIVE dBFS (e.g. +1.6) and the GUI can say "clipped, lower the
    // output". Per-ear (one earcup can be hotter than the other). The worker publishes it on the graded edge;
    // the status line derives the clip-correction guidance from referenceSweepPeakDb(ear). Message/worker-thread
    // write, lock-free milli store. -120 (the silent floor) until that ear grades. GUIDANCE only (non-invalidating).
    void  publishCompletedSweepPeakDb (int ear, float peakDb) noexcept;
    float referenceSweepPeakDb (int ear) const noexcept;

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

    // ---- Live grading response buffers (RT-safe; capture thread writes, worker reads) ----
    // REFERENCE-MATCH grade detection. Dirac HARD-PANS the measurement sweeps (the L sweep drives ch0 only,
    // the R sweep drives ch1 only), so the LEFT earcup's mic carries the L sweep and the RIGHT earcup's mic
    // carries the R sweep. We buffer BOTH mic channels into TWO independent rolling rings every block — ringL
    // (left mic) and ringR (right mic) — whenever the engine is Running AND a reference is loaded, NOT gated on
    // level and NOT gated on the active ear (the per-channel references from Task 2 handle the L/R separation).
    // Each ring holds the last kGradingSeconds (>= 28 s, > the ~25 s Dirac L+R sequence) so the whole sequence
    // is always present. (activeEar()/autoActiveEar() now drive ONLY the meter-accent UI, not grading.)
    //
    // There is NO absolute-level grade trigger. Instead the capture thread, once per ~kGradeSnapshotSeconds,
    // SNAPSHOTS each rolling ring (oldest-to-newest) into its contiguous snapshot buffer and sets that ring's
    // ready flag (it re-snapshots only after the worker has consumed the prior one). The GUI poll copies a
    // window out (snapshotGradeRing(ear,...)), runs eb::referenceMatches over it — the MATCH is the DETECTOR,
    // firing at ANY level — and, when matched, runs gradeMeasurement against that ear's reference channel.
    // A finger snap / music / a wrong reference fails referenceMatches and is NOT graded. The audio thread does
    // NO deconvolution and NO matching; all heavy work runs off-thread in the poll.
    //
    // Per-ear API (Task 3): snapshotGradeRing(ear, dst, maxLen) copies the ready snapshot for ear 0 (left mic)
    // or ear 1 (right mic) into the CALLER's buffer (the allocation lives in the GUI), returns the number of
    // samples copied (0 when no fresh window is ready for that ear) and clears that ear's ready flag.
    // gradingResponseReady(ear) is the matching test seam.
    int  snapshotGradeRing (int ear, float* dst, int maxLen) noexcept;
    bool gradingResponseReady (int ear) const noexcept;
    // OLD single-ring API, kept this task as a LEFT-ear alias so pollReferenceGrade (and the live tests) keep
    // building + grading the LEFT mic vs ref_L (consistent with Task 2's loadedReference_ = loadedReferenceL_).
    // Task 4 switches the consumer to the per-ear API. These forward to ear 0 (left).
    int  copyGradingResponse (float* dst, int maxLen) noexcept { return snapshotGradeRing (0, dst, maxLen); }
    bool gradingResponseReady() const noexcept { return gradingResponseReady (0); }
    // The rate the response buffers were captured at (the active capture rate, SHARED by both ears), for the
    // Farina offsets. Single getter — the input rate is the same for both mics.
    double gradingResponseRate() const noexcept { return grantedRate_; }

    // Publish a reference-grade verdict for ONE EAR (message/worker thread, OFFLINE). ear 0 = LEFT, 1 = RIGHT.
    // Each earcup is graded against the channel that actually drove it (Dirac hard-pans), so the two verdicts
    // are FULLY INDEPENDENT: a silent/ungraded ear keeps its base (Learned/NotGraded) state and NEVER reads as
    // a clean grade. Snapshots that ear's workflow state + IR-SNR + THD + sweepSNR + coherence TOGETHER (the
    // SNR lesson) and raises that ear's GUIDANCE flag (RefMismatch when the match-gate failed, RefLowQuality
    // when matched-but-suspect). Neither flag invalidates the capture. The combined HealthMonitor flags
    // (RefMismatch/RefLowQuality, which feed health()/cleanCapture) are also raised so a bad ear still warns;
    // Task 5 wires the per-ear DISPLAY off the ear-indexed getters below.
    void publishReferenceGrade (int ear, int refMonState, float irSnrDb, float thdPercent,
                                bool mismatch, bool lowQuality) noexcept;
    // Convenience overload: publishes the LEFT ear (ear 0). Kept so the legacy single-grade callers (and the
    // base-state publishes in start/stop/setReferenceLoaded that already set BOTH ears) stay terse.
    void publishReferenceGrade (int refMonState, float irSnrDb, float thdPercent,
                                bool mismatch, bool lowQuality) noexcept
    { publishReferenceGrade (0, refMonState, irSnrDb, thdPercent, mismatch, lowQuality); }

    // The published per-ear reference-grade snapshot (lock-free; the int-milli idiom). ear 0 = LEFT, 1 = RIGHT.
    // refMonState(ear) is RefMonState's underlying int (0 == NotLearned). 0 / 0.0 until a grade is published for
    // that ear. The no-arg overloads return the LEFT ear (ear 0) so existing single-state readers (the heartbeat,
    // the status ladder until Task 5) compile unchanged and reflect ear 0.
    int   refMonState   (int ear) const noexcept;
    float refIrSnrDb    (int ear) const noexcept;
    float refThdPercent (int ear) const noexcept;
    float refSweepSnrDb (int ear) const noexcept;   // per-ear sweep-to-room-noise SNR of that ear's last grade
    int   refMonState()   const noexcept { return refMonState   (0); }
    float refIrSnrDb()    const noexcept { return refIrSnrDb    (0); }
    float refThdPercent() const noexcept { return refThdPercent (0); }

    // ---- Hardware-Dirac detect-and-degrade -------------------------------------------------------
    // The toggle (deterministic): ON -> publish the calm GradingOffHardware state on both ears + suppress
    // the loopback grade (the GUI poll checks diracHardwareProcessorActive()); OFF -> back to NotGraded so
    // the next sweep re-grades normally.
    void setDiracHardwareProcessor (bool on) noexcept;
    bool diracHardwareProcessorActive() const noexcept { return gradingOffHardware_.load (std::memory_order_relaxed); }
    // Auto-detect (suggests only): recompute from this run's signals at the sweep-complete edge. The GUI poll
    // supplies the live OutputActivity reads; pure cores decide. Kept here (not the GUI) so it is unit-testable.
    void updateHardwareDiracAutoDetect (bool micHeardSweep, float maxOutputRenderPeak,
                                        bool outputReadable, bool validMode) noexcept;
    bool autoDetectedHardwareDirac() const noexcept { return autoDetectedHardwareDirac_.load (std::memory_order_relaxed); }

    // ---- Diagnostic getters (read-only, lock-free) — the GUI logs the detector's internals ----
    // lastInputBlockPeak(): the most recent capture block's input peak (max |sample| over L/R), stored on
    // the audio thread at the pk site (the *Milli_ fixed-point idiom). lastMatchCoherence(): the coherence
    // from the last referenceMatches poll — Task 4's match poll writes it via setLastMatchCoherence(); it
    // reads 0 until then. Both back std::atomic<int> milli stores; the getters are pure lock-free reads.
    float lastInputBlockPeak() const noexcept;

    // LIVE per-CHANNEL input peak in dBFS (the live-readout-during-the-sweep source). The audio thread
    // stores each block's per-ear peak (max |sample| over the block) at the pk site, via the *Milli_ idiom;
    // these getters convert back to dBFS. Deliberately NOT clamped at 0 dB: a float that overshot full scale
    // reads as a POSITIVE dBFS (e.g. +1.6) so the GUI can show a live "CLIPPING +1.6 dBFS" during the sweep.
    // The floor is -120 dBFS (a silent block). Lock-free reads (relaxed); the audio thread is the single writer.
    float lastInputPeakLDb() const noexcept;
    float lastInputPeakRDb() const noexcept;
    // Per-ear match coherence (ear 0 = LEFT, 1 = RIGHT): the coherence from that ear's last referenceMatches
    // poll. The no-arg overload returns the LEFT ear (ear 0) so the heartbeat/RefMon-change line compile
    // unchanged. 0 until Task 4's poll calls setLastMatchCoherence(ear, ...) for that ear.
    float lastMatchCoherence (int ear) const noexcept;
    float lastMatchCoherence() const noexcept { return lastMatchCoherence (0); }
    // Writer for the per-ear match coherence (Task 4's poll, message/worker thread). Single writer; lock-free.
    void  setLastMatchCoherence (int ear, float coherence) noexcept;
    void  setLastMatchCoherence (float coherence) noexcept { setLastMatchCoherence (0, coherence); }

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
    // Publish a base refMon state (Learned/NotLearned/NotGraded) to BOTH per-ear stores + the combined
    // HealthMonitor snapshot, clearing each ear's per-ear metrics. Used by start/stop/setReferenceLoaded so
    // both ears start from an honest base before any per-ear grade lands. Message-thread caller.
    void publishBaseRefState (RefMonState s) noexcept;
    // Pre-allocate (message thread) the live-grading ring + snapshot buffers for kGradingSeconds at `rate`,
    // size the trailing-silence trigger for `blockLen`, and clear the capture-thread trigger scratch.
    // Called from prepare/start; NEVER from the audio thread.
    void allocateResponseBuffer (double rate, int blockLen);

    DeviceManager      devices;
    ProcessingGraph    graph;
    ClockBridge        bridge;
    HealthMonitor      hm;
    MeasurementSession session_;   // D5: re-armable level-threshold sweep-window state machine
    eb::FreezeGateState freezeGate_;   // capture-thread-only: drives the D6 ratio freeze from a reliable level gate
    double             captureRate_ = 48000.0;   // set at start(); the freeze-gate release is timed in samples

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
    // LIVE per-CHANNEL input peak (linear, the *Milli_ idiom: lin*1000 in an atomic<int>). The audio thread
    // stores each block's per-ear peak at the pk site (single writer, relaxed); lastInputPeakLDb()/RDb()
    // convert to dBFS (NOT clamped at 0) for the live in-sweep readout. 0 (silent floor) until the first block.
    std::atomic<int> lastInputPeakLMilli_ { 0 };
    std::atomic<int> lastInputPeakRMilli_ { 0 };
    // Per-ear match coherence (index 0 = LEFT, 1 = RIGHT). Each is the *Milli_ idiom (coherence*1000 in an
    // atomic<int>); Task 4's two pollers each write their own ear; the GUI reads them lock-free. 0 until graded.
    std::atomic<int> lastMatchCoherMilli_[2] { { 0 }, { 0 } };

    // Per-ear PUBLISHED reference grade (Per-Ear Per-Channel Grading, Task 4). Dirac hard-pans its sweeps, so
    // each earcup is graded against its OWN reference channel and publishes an INDEPENDENT verdict here. The
    // engine holds the state TWICE (index 0 = LEFT, 1 = RIGHT) so a silent/ungraded ear can stay Learned while
    // the other reads GradedClean — neither can mask the other. Each is the lock-free *Milli_ idiom (the
    // mismatch/lowQ flags as plain atomic<bool>); the worker writes one ear's quad TOGETHER (state first), the
    // GUI reads the quad lock-free. The COMBINED guidance (RefMismatch/RefLowQuality/LowSnr) still flows through
    // HealthMonitor so health()/cleanCapture see a bad ear; these per-ear stores feed the per-ear display (Task 5).
    std::atomic<int>  refMonStatePerEar_[2]     { { (int) 0 }, { (int) 0 } };   // RefMonState::NotLearned == 0
    std::atomic<bool> gradingOffHardware_       { false };   // the toggle: grade suppressed, GradingOffHardware published
    std::atomic<bool> autoDetectedHardwareDirac_{ false };   // the auto-detect SUGGESTION (never commits by itself)
    std::atomic<int>  refIrSnrDbMilliPerEar_[2] { { 0 }, { 0 } };
    std::atomic<int>  refThdPctMilliPerEar_[2]  { { 0 }, { 0 } };
    std::atomic<int>  refSweepSnrMilliPerEar_[2]{ { 0 }, { 0 } };
    // Per-ear RAW input sweep PEAK (gain-staging readout), the *Milli_ idiom (peakDb*1000 in an atomic<int>).
    // Initialised to the silent floor (-120 dB -> -120000 milli) so an ear that never graded reads "no peak",
    // never a false clip. The worker stores THIS ear's value on the graded edge; the status line reads it lock-free.
    std::atomic<int>  refSweepPeakMilliPerEar_[2]{ { -120000 }, { -120000 } };

    // Live grading buffers (reference-match detection). TWO independent rings — gradeRingL_ (left mic) and
    // gradeRingR_ (right mic) — each with its own snapshot buffer + ready flag. ALL are PRE-ALLOCATED in
    // prepare/start (sized for kGradingSeconds at the active rate) and NEVER resized on the audio thread.
    //   GradeRing{L,R}_ : a ROLLING ring the capture thread fills CONTINUOUSLY whenever Running + a reference
    //                     is loaded (NOT gated on level, NOT gated on the active ear). Single writer (capture
    //                     thread); each `.write` is the running write head modulo size, `.filled` the running
    //                     total. l -> ringL, r -> ringR every block (Dirac hard-pans, so each ring carries its
    //                     own channel's sweep; the per-channel references handle separation).
    //   GradeRing.snapshot : the contiguous SNAPSHOT the capture thread copies the ring into (oldest->newest)
    //                     once per ~kGradeSnapshotSeconds; the poll copies it out via snapshotGradeRing(ear,..)
    //                     and runs the MATCH over it. The capture thread re-snapshots only after the poll has
    //                     consumed THAT ear's prior one (the per-ring ready flag is the producer->consumer
    //                     handoff). The snapshot cadence/blocks counter is SHARED (one block-clock for both).
    static constexpr double kGradingSeconds      = 28.0;   // >= the ~25 s Dirac L+R sequence, with margin
    // COSMETIC activity floor: a LOW floor (~-50 dBFS) just above room noise. The block peak crossing it sets
    // gradeSignalPresent_ (drives ONLY the status text + the poll's settled check). NOT a grade gate.
    static constexpr float  kActivityFloorLinear = 0.003f;  // ~ -50 dBFS
    // The capture thread publishes a fresh ring snapshot for the poll about this often (so the off-thread
    // match always has a recent window). Converted to a per-block count in allocateResponseBuffer.
    static constexpr double kGradeSnapshotSeconds = 0.5;
    // One per-mic grade ring (rolling buffer + its published snapshot + handoff state). Symmetric L/R twins.
    struct GradeRing {
        std::vector<float> ring;                      // pre-allocated rolling ring; capture-thread writes
        int                write  = 0;                // capture-thread scratch: ring write head (mod size)
        long long          filled = 0;                // capture-thread scratch: total samples ever written this run
        std::vector<float> snapshot;                  // pre-allocated snapshot; capture-thread writes, worker reads
        std::atomic<int>   readyLen { 0 };            // length of the published snapshot
        std::atomic<bool>  ready    { false };        // a fresh ring snapshot is ready for the poll to grade
    };
    GradeRing gradeRingL_;                            // left mic  (ear 0) — the OLD single-ring API aliases here
    GradeRing gradeRingR_;                            // right mic (ear 1)
    int       gradeSnapshotBlocks_ = 0;               // capture-thread scratch: blocks since the last published snapshot (SHARED clock)
    int       gradeSnapshotNeeded_ = 1;               // configured snapshot cadence in blocks (kGradeSnapshotSeconds)
    // Capture-thread helper: write this block's `src` into `gr`'s rolling ring (wrap handled) and advance the
    // head/fill. Two bounded memcpy into pre-allocated storage -> RT-safe (no alloc/lock). Single writer.
    void writeGradeRing (GradeRing& gr, const float* src, int numSamples) noexcept;
    // Capture-thread helper: snapshot `gr`'s rolling ring (oldest->newest) into its snapshot buffer and publish
    // the length + ready flag. RT-safe (two bounded memcpy into pre-allocated storage). Single writer.
    void snapshotGradeRing (GradeRing& gr) noexcept;
    // Capture-thread helper: when a reference is loaded, write left->ringL and right->ringR EVERY block (no
    // active-ear gating), set the COSMETIC gradeSignalPresent_ from blockPeak, and (on the snapshot cadence,
    // once the poll consumed the prior one) publish a fresh snapshot of EACH ring for the off-thread match. It
    // does NO matching/deconvolution and NO absolute-level grade trigger. Called by BOTH the real capture
    // callback and the headless test seam so the two paths stay aligned. blockPeak is the already-computed
    // per-block input peak (max over L/R, so the caller doesn't recompute it). RT-safe (no alloc/lock/syscall).
    void processGradeDetectionStereo (const float* left, const float* right, int numSamples, float blockPeak) noexcept;

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
