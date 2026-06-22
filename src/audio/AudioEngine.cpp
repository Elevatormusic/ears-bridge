#include "audio/AudioEngine.h"
#include "gui/SnrStatus.h"   // SNR: pure evaluateSnr verdict (RT-safe; snrNote/String is GUI-side only)
#include "audio/RefMonitor.h"   // Plan 5: RefMonState (the published-state enum; gradeMeasurement is GUI-worker-side)
#include <cmath>
#include <cstring>   // std::memcpy (RT-safe response-buffer copy on the capture thread)
namespace eb {

int AudioEngine::nextPow2 (int v) {
    int p = 1; while (p < v) p <<= 1; return p;
}

// firTapsForRate() now lives in audio/FirTaps.h (shared with the GUI's numTapsForRate).

// ---- Audio callback adapters ----------------------------------------------------------
// Capture: 2ch in -> ProcessingGraph -> mono -> bridge.pushCapture. No alloc/lock/syscall.
struct AudioEngine::CaptureCallback : juce::AudioIODeviceCallback {
    AudioEngine& e;
    std::vector<float> mono;   // sized in prepare; NOT resized on the callback
    explicit CaptureCallback (AudioEngine& o) : e (o) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override {
        mono.assign ((size_t) juce::jmax (1, dev->getCurrentBufferSizeSamples()), 0.0f);
    }
    // Fires on JUCE's audio-device thread (NOT the message thread) when the OS removes the device
    // mid-run -- so we ONLY set an atomic here and defer the teardown to the GUI timer (calling
    // stop()/closeAll() from inside this callback would re-enter JUCE's own close()). stop() flips
    // status away from Running BEFORE closing, so a deliberate stop doesn't trip this; only a loss does.
    void audioDeviceStopped() override { if (e.status() == EngineStatus::Running) e.deviceDied_.store (true); }

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* /*out*/, int /*numOut*/,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override {
        if (numIn < 2 || (int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        const float* l = in[0]; const float* r = in[1];
        if (l == nullptr || r == nullptr) { e.hm.reportXrun(); return; }   // per-pointer guard (cf. render cb)

        // D5: feed the session the CURRENT block's peak; on the genuine sweep onset re-scope validity
        // BEFORE analyzing this block (so a pre-sweep room event is dropped and a clip ON the onset
        // block is re-detected fresh by analyzeInputBlock below). The arm needs kArmSustainBlocks
        // sustained blocks, so a clip during the brief pre-arm ramp is intentionally inside the dropped
        // pre-sweep region; a clip on the block that COMPLETES the arm still latches.
        const float pk = eb::HealthMonitor::blockPeak (l, r, numSamples);
        // Per-EAR block peak, computed UNCONDITIONALLY every block (not just inside the sweep) so the live
        // in-sweep readout has a fresh L/R level each block. The sweep-SNR numerator below reuses these.
        float spkL = 0.0f, spkR = 0.0f;
        eb::HealthMonitor::blockPeakPerEar (l, r, numSamples, spkL, spkR);
        // Diagnostic getter (Task 2): publish the live input block peak (single writer, audio thread,
        // relaxed; the *Milli_ fixed-point idiom). lastInputBlockPeak() reads it lock-free GUI-side.
        // The per-channel twins (Milli L/R) drive the LIVE in-sweep dBFS readout; same single-writer/relaxed idiom.
        e.lastInputPeakMilli_.store  ((int) std::lround (pk   * 1000.0f), std::memory_order_relaxed);
        e.lastInputPeakLMilli_.store ((int) std::lround (spkL * 1000.0f), std::memory_order_relaxed);
        e.lastInputPeakRMilli_.store ((int) std::lround (spkR * 1000.0f), std::memory_order_relaxed);
        e.session_.observeBlockPeak (pk);
        if (e.session_.consumeSweepStarted())
            e.hm.resetMeasurementLatches();
        // D6: freeze the SRC ratio during the sweep. Driven by a RELIABLE fixed-threshold level gate (FreezeGate),
        // NOT session_.sweepActive() — an on-device probe proved the session's +12 dB-rise arm never fires on a
        // gradual Dirac log-sweep (frozen=no x590), so the ratio free-ran and crept ~1100 ppm, smearing the
        // sweep's phase. The gate holds ONE ratio across both earcup sweeps (long release bridges the L<->R gap),
        // then releases so the PI loop re-centers. session_ still scopes the SNR numerator below (unchanged).
        e.bridge.setSweepActive (eb::freezeGateStep (e.freezeGate_, pk, numSamples, e.captureRate_));

        e.hm.analyzeInputBlock (l, r, numSamples);             // detection (raw input): peak, run, NaN
        // SNR numerator: while the sweep is active, latch this block's per-ear peak (the per-ear max
        // over the SweepActive window). Gated here -- observeSweepPeak only runs inside the sweep, so
        // pre/post-sweep room noise never inflates the numerator. resetMeasurementLatches() above
        // already zeroed the latch on the onset edge.
        if (e.session_.sweepActive())
            e.hm.observeSweepPeak (spkL, spkR);   // spkL/spkR computed unconditionally above

        // Live grading (Plan 5): BOTH mic channels are buffered CONTINUOUSLY (NOT gated on level, NOT gated on
        // the active ear) and the grade fires from the off-thread reference MATCH. Dirac hard-pans the sweeps,
        // so the LEFT mic carries the L sweep and the RIGHT mic the R sweep; we feed l->ringL and r->ringR every
        // block and grade each against its own per-channel reference (Task 2). activeEar() now drives ONLY the
        // meter-accent UI, not grading. Single writer, RT-safe.
        e.processGradeDetectionStereo (l, r, numSamples, pk);
        // SNR (LEVEL-ARM path, mostly DEAD on real sweeps -- kept harmless): once per SweepActive->Complete edge
        // (the sweep just finished), compute the per-ear SNR verdict ONCE and raise the LowSnr GUIDANCE flag on a
        // noisy sweep. The catch: consumeSweepComplete() is driven by MeasurementSession's rise-ratio level arm,
        // which a gradual Dirac log-sweep does NOT trip -- so on a real measurement this almost never fires (the
        // user-reported "Dirac said low SNR but EARS Bridge said clean" bug). The LIVE sweep-SNR check now runs off
        // the match-aligned grade window in pollReferenceGrade (ReferenceGradePoller::computeSweepSnr), which fires
        // on ANY sweep that grades. This arm path is LEFT in place: LowSnr is a sticky OR so a double-raise is
        // idempotent, and publishCompletedSnrDb writes the same snapshot, so on the rare arm fire it is harmless;
        // the consumeSweepComplete edge still also scopes the ClockBridge freeze + the per-ear clip peaks below.
        // RT-safe: evaluateSnr is a few log10 + compares + atomic reads, raiseLowSnr is one atomic OR.
        if (e.session_.consumeSweepComplete()) {
            const auto v = eb::evaluateSnr (e.session_.armNoiseFloor(),
                                            e.hm.maxSweepPeakL(), e.hm.maxSweepPeakR(),
                                            e.session_.completedFloorStable());
            if (v.lowSnr) e.hm.raiseLowSnr();
            // SNR review fix: SNAPSHOT this sweep's verdict dB so the GUI names the exact dB that raised
            // the flag (it must NOT recompute from the live atomics, which the next sweep mutates), THEN
            // scope the per-ear peaks to ONE sweep -- zero them so the NEXT earcup sweep accumulates a
            // fresh numerator instead of reading max() across both sweeps. Order matters: publish BEFORE
            // resetSweepPeaks() (the snapshot reads this sweep's peaks via evaluateSnr above). Both are
            // RT-safe atomic stores on the capture thread; only the two SNR peak atomics are zeroed, so
            // the clip-run / validity / drift latches are untouched.
            e.hm.publishCompletedSnrDb (v.snrDbMin);
            e.hm.resetSweepPeaks();

            // Reference-Based Measurement Monitor (Plan 5, Task 4): grading is now TRIGGERED by the off-thread
            // reference MATCH poll (not this level-arm edge, which a low/gradual Dirac sweep never reaches).
            // Here we ONLY publish the honest NotGraded state when NO reference is loaded, so a non-adopter run
            // still reads "not graded - learn a reference" and never green. RT-safe single store.
            if (! e.referenceLoaded_.load())
                e.publishBaseRefState (RefMonState::NotGraded);   // both ears; RT-safe relaxed stores
        }
        // In-measurement only: an invalidating flag (clip/NaN/dropout/drift) latches the session Invalid
        // so the GUI's phase-gated wording matches cleanCapture(). Single-writer: the capture thread is
        // the only writer of session phase_; a render-side dropout sets cleanCapture=false and is
        // re-derived here on the next capture block.
        if (e.session_.inMeasurement() && ! e.hm.cleanCapture()) e.session_.markInvalid();

        if (e.graph.process (l, r, mono.data(), numSamples))   // sanitizes in+out; true => non-finite seen
            e.hm.reportNonFinite();                            // flag a FIR-produced non-finite too
        e.bridge.pushCapture (mono.data(), numSamples);

        // Surface producer-side FIFO-full losses into Health (the render callback handles the
        // consumer-side underrun via observeRenderBlock; overruns originate HERE). Diff the bridge's
        // cumulative dropped-frame count against a capture-thread-only baseline and forward the delta.
        // RT-safe: atomic load + an int member + an atomic fetch_add inside reportDroppedFrames.
        const long long dropped = e.bridge.droppedCaptureFrames();
        const long long delta   = dropped - e.lastDroppedCapture_;
        e.lastDroppedCapture_ = dropped;
        if (delta > 0) e.hm.reportDroppedFrames (delta);   // latches cleanCapture=false + grows droppedFrames
    }
};

// Render (MASTER clock): bridge.pullRender -> duplicate mono to L=R. No alloc/lock/syscall.
struct AudioEngine::RenderCallback : juce::AudioIODeviceCallback {
    AudioEngine& e;
    std::vector<float> mono;
    explicit RenderCallback (AudioEngine& o) : e (o) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override {
        mono.assign ((size_t) juce::jmax (1, dev->getCurrentBufferSizeSamples()), 0.0f);
        e.bridge.setRenderRate (dev->getCurrentSampleRate());
        // D8: register the granted render format as the reference snapshot. checkFormatChange
        // compares every render block against this; a mid-run mismatch raises FormatChanged.
        e.hm.notifyPreparedFormat (dev->getCurrentSampleRate(), dev->getCurrentBitDepth(),
                                   dev->getActiveOutputChannels().countNumberOfSetBits());
    }
    void audioDeviceStopped() override { if (e.status() == EngineStatus::Running) e.deviceDied_.store (true); }

    void audioDeviceIOCallbackWithContext (const float* const* /*in*/, int /*numIn*/,
                                           float* const* out, int numOut,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override {
        if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        // D8: re-read the render device's live format every block. On WASAPI shared mode the
        // getCurrent* accessors return cached values (no syscall). A sleep/wake or OS shared-mode
        // renegotiation changes them and mismatches the prepared snapshot, raising FormatChanged.
        // RT-safe: a trivial raw-pointer getter + atomic loads/compares (see DeviceManager::outputDevice).
        if (auto* outD = e.devices.outputDevice())
            e.hm.checkFormatChange (outD->getCurrentSampleRate(), outD->getCurrentBitDepth(),
                                    outD->getActiveOutputChannels().countNumberOfSetBits());
        const int got = e.bridge.pullRender (mono.data(), numSamples);   // capture frames written
        float pk = 0;
        for (int i = 0; i < got; ++i) pk = juce::jmax (pk, std::abs (mono[i]));
        e.hm.reportOutLevel (pk, pk >= HealthMonitor::kRailCeiling);     // meter CLIP latches at the rail; reportOutLevel still raises ClipOutput guidance at kClipLinear (-1 dBFS)
        // pullRender ALWAYS returns numSamples (it zero-pads on starvation), so `got` alone never signals
        // a shortfall. The LIVE starvation signal is the ClockBridge underrun delta: a NEW underrun this
        // block means the FIFO starved and the interpolator zero-padded -> report 0 delivered so
        // observeRenderBlock raises FifoStarved+Dropout and invalidates cleanCapture (the Plan-2 gap).
        const int und    = e.bridge.underruns();
        const int effGot = (und > e.lastUnderruns_) ? 0 : got;
        e.lastUnderruns_ = und;
        // One new additive observer: forwards ratio+fill to the Plan-2 setters AND adds dropout/drift.
        // On the macOS aggregate path (Task 7) the FIFO runs at a clock-locked 1:1, so report a NOMINAL
        // ratio/fill -- the aggregate's own drift-correction wobble must not trip the drift latch.
        if (e.usingAggregate_) e.hm.observeRenderBlock (numSamples, numSamples, 1.0, 0.5);
        else                   e.hm.observeRenderBlock (numSamples, effGot, e.bridge.currentRatio(), e.bridge.fifoFill(), e.bridge.sweepActive());
        // D6: while frozen the held SRC ratio absorbs short-term drift with FIFO headroom; if drift
        // outran the headroom the bridge flagged a forced retiming -> invalidate. Drain the edge every
        // block. Skip the macOS aggregate path: there the FIFO is clock-locked 1:1, the freeze is inert,
        // and the aggregate's own drift wobble must not be mistaken for a forced sweep correction.
        if (! e.usingAggregate_ && e.bridge.consumeEmergencyCorrection())
            e.hm.reportSweepRetimed();
        for (int ch = 0; ch < numOut; ++ch)                             // duplicate mono to both channels
            if (out[ch] != nullptr) {
                for (int i = 0; i < got; ++i)        out[ch][i] = mono[i];
                for (int i = got; i < numSamples; ++i) out[ch][i] = 0.0f;   // silence-fill on starvation
            }
    }
};

// Verify (capture-only): feed per-block L/R peaks to the LrVerify state machine and publish the
// verdict to an atomic the GUI polls. lrVerify_ is touched ONLY here (begin() happens-before the
// stream starts; the GUI reads the atomic, never lrVerify_ directly), so there is no data race.
struct AudioEngine::VerifyCallback : juce::AudioIODeviceCallback {
    AudioEngine& e;
    explicit VerifyCallback (AudioEngine& o) : e (o) {}
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* /*out*/, int /*numOut*/,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override {
        if (numIn < 2) return;
        const float* l = in[0]; const float* r = in[1];
        float pkL = 0, pkR = 0;
        for (int i = 0; i < numSamples; ++i) { pkL = juce::jmax (pkL, std::abs (l[i])); pkR = juce::jmax (pkR, std::abs (r[i])); }
        e.lrVerify_.observe (pkL, pkR);
        e.verifyResult_.store ((int) e.lrVerify_.result());   // publish the verdict snapshot
    }
};

// ---- Lifecycle ------------------------------------------------------------------------
AudioEngine::AudioEngine() {
    captureCb = std::make_unique<CaptureCallback> (*this);
    renderCb  = std::make_unique<RenderCallback>  (*this);
    verifyCb  = std::make_unique<VerifyCallback>  (*this);
    devices.onListChanged = [this] { if (onDevicesChanged) onDevicesChanged(); };
    devices.rescan();
}
AudioEngine::~AudioEngine() { endLrVerify(); stop(); }

void AudioEngine::rescanDevices() { devices.rescan(); }

std::vector<DeviceId> AudioEngine::inputDevices()  const { return devices.inputs(); }
std::vector<DeviceId> AudioEngine::outputDevices() const { return devices.outputs(); }

std::vector<double> AudioEngine::supportedSampleRates (const DeviceId& id) {
    return devices.nativeRatesFor (id);
}
std::vector<int> AudioEngine::supportedBitDepths (const DeviceId& id) const {
    return devices.nativeBitDepthsFor (id);
}

void AudioEngine::setInput  (const DeviceId& id) { inputId = id; }
void AudioEngine::setOutput (const DeviceId& id) { outputId = id; }
void AudioEngine::setSampleRate (double sr) { activeRate = sr; }
void AudioEngine::setOutputBitDepth (int bits) {   // 16/24/32; anything else -> 24 default
    outputBits = (bits == 16 || bits == 24 || bits == 32) ? bits : 24;
}

void AudioEngine::setLeftCalFir  (juce::AudioBuffer<float> fir) { graph.setFir (0, std::move (fir)); }
void AudioEngine::setRightCalFir (juce::AudioBuffer<float> fir) { graph.setFir (1, std::move (fir)); }
void AudioEngine::clearLeftCalFir()  { graph.clearFir (0); }   // back to unity + headroom 1.0
void AudioEngine::clearRightCalFir() { graph.clearFir (1); }
void AudioEngine::setCombineMode (eb::CombineMode m) { graph.setCombineMode (m); }
void AudioEngine::setOutputTrimDb (double db) {
    // Slider is <= 0 dB; treat the bottom of the range as a hard mute, else convert dB -> linear.
    graph.setOutputGain (db <= -60.0 ? 0.0f : juce::Decibels::decibelsToGain ((float) db));
}

void AudioEngine::loadLeftCal (const CalFile& cal) {
    FirDesignParams p; p.sampleRate = activeRate; p.numTaps = firTapsForRate (activeRate);
    graph.setFir (0, FirDesigner::design (cal, p));
}
void AudioEngine::loadRightCal (const CalFile& cal) {
    FirDesignParams p; p.sampleRate = activeRate; p.numTaps = firTapsForRate (activeRate);
    graph.setFir (1, FirDesigner::design (cal, p));
}

// ---- Calibration generation lifecycle (message thread) ----
void AudioEngine::setRequestedGeneration (int id) noexcept {
    requestedGenId_.store (id, std::memory_order_relaxed);
}

void AudioEngine::applyCalibrationGeneration (CalibrationGeneration gen) {
    // Engine backstop (P0-02): never swap FIRs while the audio callback is live-capturing. A cal change
    // mid-sweep would corrupt the in-flight Dirac measurement. The GUI freezes the controls so this is
    // never requested, but we guarantee it here: while Running we mutate NOTHING (no builtGenId_, no
    // appliedGen_, no setFirPair) and return. Stopped/Error (and any pre-start config) proceed normally.
    if (! reconfigAllowed())
        return;

    // Read out the fields we still need BEFORE we move gen into appliedGen_ (move leaves it valid but
    // unspecified). builtGenId_ records EVERY processed generation (valid or not) so the diagnostic
    // surfaces and the gate can tell "built but invalid" from "never built".
    const int  id    = gen.id;
    const bool valid = gen.valid;

    appliedGen_ = std::move (gen);
    builtGenId_.store (id, std::memory_order_relaxed);

    if (valid) {
        // Install BOTH ears atomically (Task 3's pair installer takes its buffers by value -> copy).
        graph.setFirPair (appliedGen_.leftFir, appliedGen_.rightFir);
        appliedGenId_.store (id, std::memory_order_relaxed);
    }
    // An INVALID generation installs NOTHING: the graph keeps its prior/unity state and appliedGenId_
    // is left untouched, so calibrationApplied() stays closed while calibrationDiagnostic() surfaces why.
}

int  AudioEngine::requestedGeneration() const noexcept { return requestedGenId_.load (std::memory_order_relaxed); }
int  AudioEngine::builtGeneration()     const noexcept { return builtGenId_.load     (std::memory_order_relaxed); }
int  AudioEngine::appliedGeneration()   const noexcept { return appliedGenId_.load   (std::memory_order_relaxed); }

bool AudioEngine::calibrationApplied() const noexcept {
    const int req = requestedGenId_.load (std::memory_order_relaxed);
    const int blt = builtGenId_.load     (std::memory_order_relaxed);
    const int app = appliedGenId_.load   (std::memory_order_relaxed);
    return req == blt && blt == app && app != 0 && appliedGen_.valid;
}

juce::String AudioEngine::calibrationDiagnostic() const { return appliedGen_.diagnostic; }

EngineStatus AudioEngine::status() const { return (EngineStatus) engineStatus.load(); }
Levels AudioEngine::levels() const { return hm.levels(); }
Health AudioEngine::health() const {
    // The single monitor `hm` now carries fifoFill + captureToRenderRatio (from observeRenderBlock),
    // plus the sticky flags and cleanCapture. Layer the D5 session phase on top.
    auto h = hm.snapshot();
    h.session = session_.phase();   // D5: surface the measurement-session phase to the GUI
    return h;
}

HealthFlag AudioEngine::healthFlags() const noexcept     { return hm.flags(); }
bool       AudioEngine::cleanCapture() const noexcept    { return hm.cleanCapture(); }
DipGainProfile AudioEngine::gainProfile() const noexcept { return hm.gainProfile(); }
bool AudioEngine::consumeRecentInputClip() noexcept      { return hm.recentInputClip(); }
bool AudioEngine::reachedGoodLevel()       const noexcept { return hm.reachedGoodLevel(); }
float AudioEngine::completedSweepSnrDb()   const noexcept {
    // SNR review fix (Finding 3): read the dB SNAPSHOTTED at the SweepActive->Complete edge (the exact
    // min-ear dB the verdict used to raise LowSnr), NOT a live recompute from maxSweepPeak*/armFloor.
    // Recomputing here let the displayed number drift once a SUBSEQUENT sweep mutated those atomics (e.g.
    // the right-earcup sweep changing the peaks after the left sweep raised the sticky flag), so the GUI
    // could show a dB that contradicted the flag. The snapshot is frozen, so the displayed dB always
    // matches the dB that raised the flag. Pure lock-free read.
    return hm.completedSnrDb();
}
// Match-window sweep-SNR fix: forward the GUI-worker-computed sweep SNR to HealthMonitor. publishCompletedSweepSnrDb
// reuses publishCompletedSnrDb so completedSweepSnrDb()/the status line read the SAME snapshot; raiseLowSnr raises the
// GUIDANCE LowSnr flag (not invalidating). Both are message/worker-thread callers writing lock-free atomics off the
// audio thread (the grade math already ran on the worker; this is just the publish step).
void AudioEngine::publishCompletedSweepSnrDb (int ear, float snrDbMin) noexcept {
    // Snapshot THIS ear's sweep-SNR into the per-ear store (the per-ear status line names the dB) AND forward
    // to HealthMonitor's combined completedSnrDb()/the existing single status line. Lock-free milli store.
    refSweepSnrMilliPerEar_[(ear == 1) ? 1 : 0].store ((int) std::lround (snrDbMin * 1000.0f));
    hm.publishCompletedSnrDb (snrDbMin);
}
// Gain-staging readout: snapshot THIS ear's RAW input sweep PEAK (dBFS) into the per-ear store so the status line
// can derive the clip-correction guidance. NOT clamped at 0 — a clipping float reads as a positive dBFS. Lock-free
// milli store; message/worker-thread caller (the grade math already ran off-thread; this is just the publish step).
void AudioEngine::publishCompletedSweepPeakDb (int ear, float peakDb) noexcept {
    refSweepPeakMilliPerEar_[(ear == 1) ? 1 : 0].store ((int) std::lround (peakDb * 1000.0f));
}
void AudioEngine::raiseLowSnr() noexcept { hm.raiseLowSnr(); }
int  AudioEngine::autoActiveEar()          const noexcept { return graph.activeEar(); }
float AudioEngine::headroomAttenuationDb() const noexcept { return graph.headroomAttenuationDb(); }

// ---- Reference-Based Measurement Monitor (Plan 5) ----
// Publish a BASE refMon state (Learned / NotLearned / NotGraded) to BOTH per-ear stores AND the combined
// HealthMonitor snapshot, clearing each ear's per-ear metrics. The base state is published for both ears so a
// later per-ear grade overwrites only the ear that graded; the other ear keeps the honest base state. Resets
// each ear's coherence/sweep-SNR so a stale value never outlives a learn/start/stop. Lock-free atomic stores.
void AudioEngine::publishBaseRefState (RefMonState s) noexcept {
    for (int ear = 0; ear < 2; ++ear) {
        refMonStatePerEar_[ear].store ((int) s);
        refIrSnrDbMilliPerEar_[ear].store (0);
        refThdPctMilliPerEar_[ear].store (0);
        refSweepSnrMilliPerEar_[ear].store (0);
        refSweepPeakMilliPerEar_[ear].store (-120000);   // reset to the silent floor (no stale clip across runs)
        lastMatchCoherMilli_[ear].store (0);
    }
    hm.publishRefGrade ((int) s, 0.0f, 0.0f);   // the combined snapshot the legacy status ladder still reads
}

void AudioEngine::setReferenceLoaded (bool loaded) noexcept {
    referenceLoaded_.store (loaded);
    // Publish an HONEST base state immediately so the GUI/log don't read the NotLearned default while a
    // reference IS loaded: a loaded-but-ungraded reference is Learned, not NotLearned. Published for BOTH ears
    // (Task 4) so each starts honest; a later per-ear match-poll grade overwrites only its ear; clearing the
    // reference returns BOTH to NotLearned. (Message-thread caller -- learn / startup reload.)
    publishBaseRefState (loaded ? RefMonState::Learned : RefMonState::NotLearned);
}
bool AudioEngine::referenceLoaded()        const noexcept { return referenceLoaded_.load(); }
bool AudioEngine::gradeSignalPresent()     const noexcept { return gradeSignalPresent_.load (std::memory_order_relaxed); }

void AudioEngine::allocateResponseBuffer (double rate, int blockLen) {
    // Pre-allocate (message thread) for kGradingSeconds at this rate so the capture thread NEVER resizes.
    // BOTH per-mic rings and their snapshots are sized identically; the capture thread only memcpy's into
    // them (never grows them). Reset all capture-thread trigger scratch for a fresh run.
    const int len = juce::jmax (1, (int) std::lround (rate * kGradingSeconds));
    for (GradeRing* gr : { &gradeRingL_, &gradeRingR_ }) {
        gr->ring.assign     ((size_t) len, 0.0f);
        gr->snapshot.assign ((size_t) len, 0.0f);
        gr->write  = 0;
        gr->filled = 0;
        gr->readyLen.store (0);
        gr->ready.store (false);
    }
    gradeSnapshotBlocks_ = 0;
    gradeSignalPresent_.store (false, std::memory_order_relaxed);
    // Snapshot cadence for THIS block size/rate: the capture thread counts WHOLE blocks, so convert
    // kGradeSnapshotSeconds into blocks for the real block size (>= 1 so a degenerate block can't disable it).
    const int blk = juce::jmax (1, blockLen);
    gradeSnapshotNeeded_ = juce::jmax (1, (int) std::lround (kGradeSnapshotSeconds * rate / (double) blk));
}

void AudioEngine::writeGradeRing (GradeRing& gr, const float* src, int numSamples) noexcept {
    // Capture-thread: copy `src` into the rolling ring, wrapping at the ring end; advance the head + the
    // running fill count. Two bounded memcpy into pre-allocated storage -> RT-safe (no alloc/lock). A ~25 s
    // sequence is far shorter than the ring, so the most-recent kGradingSeconds always holds the whole sequence.
    const int size = (int) gr.ring.size();
    if (size <= 0) return;
    int idx = gr.write;
    const int first = juce::jmin (numSamples, size - idx);
    if (first > 0) std::memcpy (gr.ring.data() + idx, src, (size_t) first * sizeof (float));
    const int rest = numSamples - first;
    if (rest > 0) std::memcpy (gr.ring.data(), src + first, (size_t) juce::jmin (rest, size) * sizeof (float));
    idx += numSamples;
    while (idx >= size) idx -= size;
    gr.write  = idx;
    gr.filled += numSamples;
}

void AudioEngine::snapshotGradeRing (GradeRing& gr) noexcept {
    // Capture-thread: copy the rolling ring into its snapshot in OLDEST->NEWEST order, then publish the length
    // + ready flag. Two bounded memcpy into pre-allocated storage -> RT-safe (no alloc/lock). When the run has
    // not yet filled the ring, the valid span is [0, gr.filled) starting at index 0; once wrapped, the oldest
    // sample sits at gr.write and the span is the whole ring.
    const int size = (int) gr.ring.size();
    if (size <= 0 || (int) gr.snapshot.size() < size) { gr.readyLen.store (0); gr.ready.store (true); return; }
    if (gr.filled < (long long) size) {
        // Not wrapped yet: the valid samples are the first gr.filled entries, already in order.
        const int n = (int) gr.filled;
        if (n > 0) std::memcpy (gr.snapshot.data(), gr.ring.data(), (size_t) n * sizeof (float));
        gr.readyLen.store (n);
    } else {
        // Wrapped: oldest is at gr.write. Copy [write..end) then [0..write).
        const int tail = size - gr.write;
        if (tail > 0) std::memcpy (gr.snapshot.data(), gr.ring.data() + gr.write, (size_t) tail * sizeof (float));
        if (gr.write > 0) std::memcpy (gr.snapshot.data() + tail, gr.ring.data(), (size_t) gr.write * sizeof (float));
        gr.readyLen.store (size);
    }
    gr.ready.store (true);
}

void AudioEngine::processGradeDetectionStereo (const float* left, const float* right,
                                               int numSamples, float blockPeak) noexcept {
    // Reference-match detection. Runs ONLY when a reference is loaded (so non-adopters are completely
    // unaffected). The audio thread does NO matching and NO absolute-level grade trigger — it only buffers BOTH
    // mic channels (l->ringL, r->ringR, NO active-ear gating), maintains the COSMETIC activity flag, and
    // periodically publishes a ring snapshot per ear for the off-thread match poll (the sole grade detector).
    if (! referenceLoaded_.load() || gradeRingL_.ring.empty() || numSamples <= 0)
        return;

    // 1) Continuous per-mic rolling-ring write (single writer, capture thread, pre-allocated -> RT-safe). Each
    //    channel goes to its OWN ring: Dirac hard-pans, so ringL holds the L sweep, ringR holds the R sweep.
    if (left  != nullptr) writeGradeRing (gradeRingL_, left,  numSamples);
    if (right != nullptr) writeGradeRing (gradeRingR_, right, numSamples);

    // 2) COSMETIC room-floor activity flag (NOT a grade gate). True when this block's peak (max over L/R, as
    //    before) clears the LOW activity floor (~-50 dBFS, just above room noise). Single writer, relaxed; the
    //    GUI reads it ONLY for the "Sweep in progress..." status. It does NOT gate grading.
    gradeSignalPresent_.store (blockPeak >= kActivityFloorLinear, std::memory_order_relaxed);

    // 3) Periodic ring snapshot for the off-thread match poll. On a SHARED block-clock, publish a fresh window
    //    for EACH ear about every kGradeSnapshotSeconds, but ONLY after the poll has consumed THAT ear's prior
    //    one (its ready flag false) so the capture thread never writes a snapshot the poll is reading. The MATCH
    //    (run in the poll over this window) is the detector; there is no level trigger here.
    if (++gradeSnapshotBlocks_ >= gradeSnapshotNeeded_) {
        gradeSnapshotBlocks_ = 0;
        if (! gradeRingL_.ready.load()) snapshotGradeRing (gradeRingL_);
        if (! gradeRingR_.ready.load()) snapshotGradeRing (gradeRingR_);
    }
}

int AudioEngine::snapshotGradeRing (int ear, float* dst, int maxLen) noexcept {
    // Worker/message thread: copy the ready snapshot for `ear` (0=left mic, 1=right mic) into the CALLER's
    // buffer (the allocation lives in the GUI, never here), then clear that ear's ready flag. Returns the
    // number of samples copied, or 0 when no fresh response is ready for that ear. The snapshot buffer is not
    // resized once a run is prepared, so reading up to readyLen (snapshotted by the capture thread) is safe.
    GradeRing& gr = (ear == 1) ? gradeRingR_ : gradeRingL_;
    if (! gr.ready.exchange (false) || dst == nullptr || maxLen <= 0) return 0;
    const int n = juce::jmin (maxLen, juce::jmin (gr.readyLen.load(), (int) gr.snapshot.size()));
    if (n > 0) std::memcpy (dst, gr.snapshot.data(), (size_t) n * sizeof (float));
    return n;
}

bool AudioEngine::gradingResponseReady (int ear) const noexcept {
    return ((ear == 1) ? gradeRingR_ : gradeRingL_).ready.load();
}
void AudioEngine::publishReferenceGrade (int ear, int refMonState, float irSnrDb, float thdPercent,
                                         bool mismatch, bool lowQuality) noexcept {
    // OFFLINE (message/worker thread). Publish THIS ear's verdict independently of the other ear. Snapshot
    // the trio (state/IR-SNR/THD) into the per-ear store TOGETHER (state LAST is not required since the GUI
    // reads relaxed; but write the metrics first then state so a reader that gates on state sees fresh numbers),
    // so the displayed numbers always match that ear's flag. The COMBINED HealthMonitor snapshot + flags still
    // get this ear's verdict so the legacy status ladder + cleanCapture see a bad ear; Task 5 splits the display.
    const int e = (ear == 1) ? 1 : 0;
    refIrSnrDbMilliPerEar_[e].store ((int) std::lround (irSnrDb   * 1000.0f));
    refThdPctMilliPerEar_[e].store  ((int) std::lround (thdPercent * 1000.0f));
    refMonStatePerEar_[e].store (refMonState);
    // Combined snapshot (legacy single-state readers + cleanCapture guidance flags).
    hm.publishRefGrade (refMonState, irSnrDb, thdPercent);
    if (mismatch)   hm.raiseRefMismatch();
    if (lowQuality) hm.raiseRefLowQuality();
}
int   AudioEngine::refMonState   (int ear) const noexcept { return refMonStatePerEar_[(ear == 1) ? 1 : 0].load(); }
float AudioEngine::refIrSnrDb    (int ear) const noexcept { return refIrSnrDbMilliPerEar_[(ear == 1) ? 1 : 0].load() / 1000.0f; }
float AudioEngine::refThdPercent (int ear) const noexcept { return refThdPctMilliPerEar_[(ear == 1) ? 1 : 0].load() / 1000.0f; }
float AudioEngine::refSweepSnrDb (int ear) const noexcept { return refSweepSnrMilliPerEar_[(ear == 1) ? 1 : 0].load() / 1000.0f; }
float AudioEngine::referenceSweepPeakDb (int ear) const noexcept { return refSweepPeakMilliPerEar_[(ear == 1) ? 1 : 0].load() / 1000.0f; }

// ---- Diagnostic getters (Task 2): the *Milli_ fixed-point idiom, lock-free reads ----
// lastInputBlockPeak() converts the audio-thread store back to float (relaxed; the GUI only needs the
// latest value, not ordering against other state). lastMatchCoherence() returns 0 until Task 4's poll
// calls setLastMatchCoherence().
float AudioEngine::lastInputBlockPeak() const noexcept {
    return lastInputPeakMilli_.load (std::memory_order_relaxed) / 1000.0f;
}
// LIVE per-channel input peak -> dBFS for the in-sweep readout. NOT clamped at 0: gainToDecibels of a
// linear value > 1.0 (a float that overshot full scale) returns a positive dB, so a clip reads e.g. +1.6.
// Floor at -120 dBFS (a silent block). The *Milli_ store rounds to 0.001 linear; the GUI rounds to 0.1 dB.
float AudioEngine::lastInputPeakLDb() const noexcept {
    return juce::Decibels::gainToDecibels (lastInputPeakLMilli_.load (std::memory_order_relaxed) / 1000.0f, -120.0f);
}
float AudioEngine::lastInputPeakRDb() const noexcept {
    return juce::Decibels::gainToDecibels (lastInputPeakRMilli_.load (std::memory_order_relaxed) / 1000.0f, -120.0f);
}
float AudioEngine::lastMatchCoherence (int ear) const noexcept {
    return lastMatchCoherMilli_[(ear == 1) ? 1 : 0].load (std::memory_order_relaxed) / 1000.0f;
}
void AudioEngine::setLastMatchCoherence (int ear, float coherence) noexcept {
    lastMatchCoherMilli_[(ear == 1) ? 1 : 0].store ((int) std::lround (coherence * 1000.0f), std::memory_order_relaxed);
}

bool AudioEngine::consumeDeviceDied()      noexcept      { return deviceDied_.exchange (false); }
int  AudioEngine::grantedOutputBitDepth()  const noexcept { return devices.grantedOutputBitDepth(); }
RawRailState AudioEngine::rawRail()        const noexcept { return rawRail_; }

void AudioEngine::onDeviceLost() {
    // Set status away from Running FIRST so the closeAll() below (which fires audioDeviceStopped on
    // the remaining device) can't re-latch deviceDied_, then tear down and surface Error.
    engineStatus.store ((int) EngineStatus::Error);
    devices.closeAll();
    aggregate_.destroy(); usingAggregate_ = false;
    bridge.reset();
    rawRail_ = RawRailState {};   // D2: the device is gone -- the snapshot must not outlive it
    deviceDied_.store (false);
}

bool AudioEngine::beginLrVerify (Ear earUnderTest, juce::String& errorOut) {
    if (status() == EngineStatus::Running) { errorOut = "Stop the bridge before verifying L/R."; return false; }
    endLrVerify();                                   // idempotent: tear down any prior verify stream
    lrVerify_.begin (earUnderTest);                  // resets the state machine (happens-before start)
    verifyResult_.store ((int) LrResult::Pending);
    auto err = devices.openInput (inputId, activeRate, 512);   // capture-only
    if (err.isNotEmpty()) { errorOut = err; return false; }
    if (auto* d = devices.inputDevice()) d->start (verifyCb.get());
    verifyActive_.store (true);
    return true;
}
void AudioEngine::endLrVerify() {
    if (! verifyActive_.exchange (false)) return;
    if (auto* d = devices.inputDevice()) d->stop();
    devices.closeAll();                              // only the capture device is open here
}
bool     AudioEngine::lrVerifyActive()   const noexcept { return verifyActive_.load(); }
LrResult AudioEngine::lrVerifyResult()   const noexcept { return (LrResult) verifyResult_.load(); }
bool     AudioEngine::lrVerifyComplete() const noexcept { return verifyResult_.load() != (int) LrResult::Pending; }

bool AudioEngine::start (juce::String& errorOut) {
    if (status() == EngineStatus::Running) return true;
    endLrVerify();   // a pending L/R-verify stream holds the capture device; release it first
    deviceDied_.store (false);   // fresh run: clear any stale device-loss latch

    // ASIO-incapability guard: ASIO cannot pair an EARS capture with a different virtual render.
    // Build the decision from the DeviceManager's type capabilities and fall back if needed.
    {
        auto cur = devices.currentTypeCaps();                       // DeviceManager::TypeCaps
        eb::DeviceTypeCaps preferred { cur.typeName, cur.separateInputsAndOutputs };
        juce::Array<eb::DeviceTypeCaps> available;
        for (auto& c : devices.availableTypeCaps())
            available.add ({ c.typeName, c.separateInputsAndOutputs });
        auto decision = eb::AsioFallback::decide (preferred, available);
        if (decision.mustFallback) {
            if (decision.chosenTypeName.isEmpty()) {
                errorOut = decision.message;
                engineStatus.store ((int) EngineStatus::Error);
                return false;
            }
            devices.setCurrentType (decision.chosenTypeName);       // honored by the next open
            lastFallbackMessage_ = decision.message;                // surfaced by the GUI
        } else {
            lastFallbackMessage_.clear();
        }
    }

#if JUCE_MAC
    // macOS-preferred path: build a private CoreAudio aggregate (EARS clock-master + virtual
    // output, OS drift-corrects the follower) and open it for BOTH capture and render. The two
    // sub-devices then share one clock, so the ClockBridge ASRC runs at a trivial 1:1 and never has
    // to correct drift (the FIFO conduit between the capture/render IOProcs is still required and is
    // sized below). On any failure or partial open, fall through to the Plan-2 two-device path.
    usingAggregate_ = false;
    {
        juce::String aggErr;
        if (aggregate_.create (inputId.uid, outputId.uid, aggErr)) {
            const auto aggName = aggregate_.aggregateUid();
            eb::DeviceId aggDev; aggDev.typeName = "CoreAudio"; aggDev.name = aggName; aggDev.uid = aggName;
            auto eAggIn  = devices.openInput  (aggDev, activeRate, 512);
            auto eAggOut = devices.openOutput (aggDev, activeRate, 512, outputBits);
            if (eAggIn.isEmpty() && eAggOut.isEmpty()) {
                usingAggregate_ = true;   // proceed with the aggregate; skip the ClockBridge prepare below
            } else {
                devices.closeAll();
                aggregate_.destroy();     // partial open: fall through to the ClockBridge path
            }
        }
        // If usingAggregate_ stayed false, fall through to the Plan-2 two-device open + ClockBridge.
    }
#endif

    // On the aggregate path the device is already open (for both capture and render); skip the
    // Plan-2 two-device open. Otherwise open the separate capture + render devices as before.
    if (! usingAggregate_) {
        const int bufSize = 512;   // typical WASAPI shared block; device may adjust
        auto eIn = devices.openInput (inputId, activeRate, bufSize);
        if (eIn.isNotEmpty()) { errorOut = eIn; engineStatus.store ((int) EngineStatus::Error); return false; }
        auto eOut = devices.openOutput (outputId, activeRate, bufSize, outputBits);   // best-effort depth
        if (eOut.isNotEmpty()) { errorOut = eOut; devices.closeAll(); engineStatus.store ((int) EngineStatus::Error); return false; }
    }

    auto* inD  = devices.inputDevice();
    auto* outD = devices.outputDevice();
    const double capRate = inD->getCurrentSampleRate();
    const double renRate = outD->getCurrentSampleRate();
    grantedRate_ = capRate;   // what the EARS actually runs at (may differ from the user's selection)
    const int    maxBlk  = juce::jmax (inD->getCurrentBufferSizeSamples(),
                                       outD->getCurrentBufferSizeSamples());

    graph.prepare (capRate, maxBlk);
    // Capacity: ~0.5 s of capture-rate mono, power-of-two-ish, never below 8192. Sized so the
    // half-full prime below (~0.25-0.34 s of headroom) comfortably exceeds the measured ~150 ms
    // render-before-capture startup gap with margin for slower machines / larger WASAPI buffers.
    // (The prime MUST stay at the 0.5 setpoint, so margin comes from a bigger FIFO, not a deeper
    // prime -- priming above 0.5 would re-introduce the startup ExcessDrift this fix removed.)
    const int cap = juce::jmax (8192, juce::nextPowerOfTwo ((int) (capRate * 0.5)));
    // Prepare the ClockBridge on BOTH paths. The capture and render IOProcs are always separate
    // callbacks (DeviceManager opens input-only + output-only devices), so the lock-free FIFO is the
    // REQUIRED conduit between them in every case. On the macOS aggregate path the two sub-devices
    // share one clock (capRate == renRate == activeRate), so the ASRC runs at a trivial 1:1 with no
    // drift to correct -- but the FIFO must still be sized, or render pulls from an empty FIFO and the
    // output is silent. (The render callback still reports a NOMINAL ratio on the aggregate path so the
    // aggregate's internal drift-correction wobble can't trip the drift latch.)
    bridge.prepare (capRate, renRate, 1, cap);
    captureRate_ = capRate;   // the freeze-gate release is timed in samples at the capture rate
    hm.prepare (inputId.model, cap, capRate / juce::jmax (1.0, renRate));   // reset + size + NOMINAL ratio (drift detection)
    // hm.prepare() reset the published refMon state back to its NotLearned default -- which would OVERWRITE
    // the honest Learned that setReferenceLoaded() published when the user learned a reference, making the
    // GUI/log lie ("learn a reference") for the whole run. Re-publish the honest state from the live
    // referenceLoaded_ latch immediately, so a measurement that starts with a reference loaded reads Learned
    // (a later per-ear match-poll grade overwrites this; the capture-callback/complete-edge NotGraded publishes
    // are untouched and still fire when ! referenceLoaded). Published for BOTH ears (Task 4) so each starts honest.
    publishBaseRefState (referenceLoaded_.load() ? RefMonState::Learned : RefMonState::NotLearned);
    // D2: latch the raw-rail snapshot on this converged success path (always runs before a Running
    // return). reportRawRail MUST come AFTER hm.prepare() -- prepare resets flagBits, so reporting
    // before it would clear the OsResampled guidance flag this raises when the rail isn't verified.
    rawRail_ = RawRailState { devices.rawRailVerified(),
                              devices.requestedInputSampleRate(),
                              devices.endpointMixSampleRate() };
    hm.reportRawRail (rawRail_.verified);
    session_.configure (maxBlk, capRate);   // D5: size the terminal-silence window for this BLOCK size/rate (NOT the FIFO capacity)
    session_.reset();                    // fresh measurement session each run (Idle -> Preflight -> ...)
    allocateResponseBuffer (capRate, maxBlk);   // Plan 5: pre-allocate the live-grading ring + size the trigger (off the audio thread)
    bridge.reset();
    // Pre-fill the FIFO to the PI controller's half-full target BEFORE the streams start. The two
    // device callbacks begin asynchronously and the render callback can pull before capture has
    // primed the FIFO; without this, that one-time startup gap zero-pads (FifoStarved) and the PI
    // loop then drags the SRC ratio off-nominal for seconds (ExcessDrift) while it refills -- both
    // latch cleanCapture=false permanently ("Dropouts detected") even though the audio is clean.
    // primeToTarget() fills to ClockBridge::kTargetFill (the same 0.5 the PI loop steers to), so the
    // prime depth and the setpoint can never drift apart. It absorbs the startup gap and starts fill
    // AT target (no drift).
    bridge.primeToTarget();
    bridge.setSweepActive (false);   // D6: a fresh run starts free-running; the freeze gate arms on the first sweep
    freezeGate_ = {};                 // reset the level gate so a prior run's hold can't carry into this one
    // Reset the per-callback diff baselines: bridge.reset() zeroed the bridge's under/over/dropped
    // counters, so the callbacks' "last seen" baselines must also return to 0 or a stale-high value
    // from a prior run would suppress detection (underruns) / inject a spurious negative delta
    // (dropped frames) at the top of THIS run.
    lastUnderruns_      = 0;
    lastDroppedCapture_ = 0;

    // Render is the master: start it last so the FIFO has primed once capture runs.
    inD->start (captureCb.get());
    outD->start (renderCb.get());
    // A device can open yet fail to begin streaming (in use, driver glitch); don't claim Running with
    // no callbacks (which would show a false "Running - clean" while Dirac records silence).
    if (! inD->isPlaying() || ! outD->isPlaying()) {
        errorOut = "Device opened but did not start streaming: "
                 + (! inD->isPlaying() ? inD->getLastError() : outD->getLastError());
        devices.closeAll();
        aggregate_.destroy(); usingAggregate_ = false;
        bridge.reset();
        rawRail_ = RawRailState {};   // streams never came up: drop the snapshot latched above
        engineStatus.store ((int) EngineStatus::Error);
        return false;
    }
    engineStatus.store ((int) EngineStatus::Running);
    return true;
}

void AudioEngine::stop() {
    if (status() != EngineStatus::Running) {
        devices.closeAll();
        aggregate_.destroy();      // idempotent: tear down any aggregate even from a non-Running state
        usingAggregate_ = false;
        rawRail_ = RawRailState {};   // D2: clear any snapshot left from a prior run
        session_.reset();             // D5: clear the measurement session too
        return;
    }
    // Flip out of Running BEFORE closing so our own audioDeviceStopped() doesn't latch deviceDied_
    // (it only latches while status()==Running). Then tear down.
    engineStatus.store ((int) EngineStatus::Stopped);
    devices.closeAll();
    aggregate_.destroy();          // idempotent (no-op on Windows / when not using the aggregate)
    usingAggregate_ = false;
    bridge.reset();
    rawRail_ = RawRailState {};    // D2: stopped -- the snapshot must not outlive the device
    session_.reset();              // D5: clear the measurement session (symmetry with rawRail_)
}

// ---- Headless test seam ---------------------------------------------------------------
void AudioEngine::prepareForTest (double sampleRate, int block) {
    activeRate = sampleRate; blockSize = block;
    graph.prepare (sampleRate, block);
    hm.prepare (eb::EarsModel::Ears, juce::jmax (8192, block * 4));   // reset + size so the seam mirrors a run
    session_.configure (block, sampleRate);
    session_.reset();
    grantedRate_ = sampleRate;             // so gradingResponseRate() reports the seam's capture rate
    allocateResponseBuffer (sampleRate, block);   // Plan 5: pre-allocate the live-grading ring + size the trigger
    hm.notifyPreparedFormat (sampleRate, 32, 2);   // D8: register the test format so simulateFormatChangeForTest has a reference
}
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                              float* outMono, int numSamples) {
    // Mirror the capture-callback ordering: feed the session this block's peak, re-scope on the sweep
    // onset, analyze, then latch Invalid if an invalidating flag fired in-measurement.
    const float pk = eb::HealthMonitor::blockPeak (inL, inR, numSamples);
    float spkL = 0.0f, spkR = 0.0f;                                  // per-ear peak (mirror the capture callback)
    eb::HealthMonitor::blockPeakPerEar (inL, inR, numSamples, spkL, spkR);
    // Diagnostic getter (Task 2): mirror the capture-callback store of the live input block peak (single
    // writer, relaxed; the *Milli_ idiom) so the headless seam reflects the same getter as a real run.
    // The per-channel twins drive the live in-sweep dBFS readout (same idiom).
    lastInputPeakMilli_.store  ((int) std::lround (pk   * 1000.0f), std::memory_order_relaxed);
    lastInputPeakLMilli_.store ((int) std::lround (spkL * 1000.0f), std::memory_order_relaxed);
    lastInputPeakRMilli_.store ((int) std::lround (spkR * 1000.0f), std::memory_order_relaxed);
    session_.observeBlockPeak (pk);
    if (session_.consumeSweepStarted()) hm.resetMeasurementLatches();
    bridge.setSweepActive (session_.sweepActive());                  // D6: mirror the capture-callback freeze sync
    hm.analyzeInputBlock (inL, inR, numSamples);                      // same analysis as the capture callback
    if (session_.sweepActive())                                      // SNR numerator (mirror the capture callback)
        hm.observeSweepPeak (spkL, spkR);                            // spkL/spkR computed unconditionally above
    // Live grading: continuous per-mic ring write (l->ringL, r->ringR, no active-ear gating) + cosmetic
    // activity flag + periodic snapshot (mirror the capture callback). No absolute level trigger; the
    // off-thread match is the detector. Single writer.
    processGradeDetectionStereo (inL, inR, numSamples, pk);
    // SNR: per-sweep verdict at the SweepActive->Complete edge (mirror the capture callback) -- raise
    // LowSnr on a noisy sweep, SNAPSHOT the verdict dB, then scope the per-ear peaks to ONE sweep.
    if (session_.consumeSweepComplete()) {
        const auto v = eb::evaluateSnr (session_.armNoiseFloor(),
                                        hm.maxSweepPeakL(), hm.maxSweepPeakR(),
                                        session_.completedFloorStable());
        if (v.lowSnr) hm.raiseLowSnr();
        hm.publishCompletedSnrDb (v.snrDbMin);
        hm.resetSweepPeaks();
        // Plan 5 (Task 4): grading is triggered by the off-thread reference MATCH poll, not this edge. Only
        // publish the honest NotGraded state when NO reference is loaded (mirror the capture callback).
        if (! referenceLoaded_.load())
            publishBaseRefState (RefMonState::NotGraded);   // both ears; RT-safe relaxed stores
    }
    if (session_.inMeasurement() && ! hm.cleanCapture()) session_.markInvalid();
    if (graph.process (inL, inR, outMono, numSamples)) hm.reportNonFinite();
}

void AudioEngine::simulateFormatChangeForTest (double sampleRate, int bitDepth, int numChannels) noexcept {
    hm.checkFormatChange (sampleRate, bitDepth, numChannels);   // D8: drive the render-side check directly
}

// ---- R22 callback-level test seam -----------------------------------------------------
// Build the same state start() does (without devices), then drive the production callbacks.
void AudioEngine::prepareCallbacksForTest (double sampleRate, int block, int fifoCapacity) {
    activeRate = sampleRate; blockSize = block;
    const int cap = juce::jmax (1024, fifoCapacity);

    graph.prepare (sampleRate, block);
    bridge.prepare (sampleRate, sampleRate, 1, cap);   // clears the D6 freeze state
    hm.prepare (eb::EarsModel::Ears, cap, 1.0);        // nominal capture:render ratio == 1.0 (equal rates)
    // Mirror start(): hm.prepare() reset the published refMon state to NotLearned; re-publish the honest
    // state from referenceLoaded_ so the headless seam reflects a real run (Learned when a reference is loaded).
    publishBaseRefState (referenceLoaded_.load() ? RefMonState::Learned : RefMonState::NotLearned);   // both ears
    session_.configure (block, sampleRate);            // D5: size the silence window for this block/rate
    session_.reset();                                  // D5: fresh session each run (Idle -> Preflight -> ...)
    grantedRate_ = sampleRate;                         // gradingResponseRate() reports the seam's capture rate
    allocateResponseBuffer (sampleRate, block);        // Plan 5: pre-allocate the live-grading ring + size the trigger
    bridge.reset();
    bridge.primeToTarget();                            // mirror start(): prime to the PI setpoint, no startup underrun
    bridge.setSweepActive (false);                     // D6: a fresh run free-runs; the freeze gate arms on the sweep
    freezeGate_ = {};                                  // reset the level gate (mirror start())
    captureRate_ = sampleRate;                          // keep the gate's release timing correct on this path too

    // Reset the per-callback diff baselines exactly as start() does (a stale value would suppress
    // underrun detection or inject a spurious dropped-frame delta on the first driven block).
    lastUnderruns_      = 0;
    lastDroppedCapture_ = 0;
    usingAggregate_     = false;                   // exercise the Windows two-device path (real observeRenderBlock)

    // Size the production callbacks' scratch (audioDeviceAboutToStart normally does this from the device).
    captureCb->mono.assign ((size_t) juce::jmax (1, block), 0.0f);
    renderCb->mono.assign  ((size_t) juce::jmax (1, block), 0.0f);

    engineStatus.store ((int) EngineStatus::Running);   // so audioDeviceStopped()-style guards behave as in a run
}

void AudioEngine::driveCaptureCallback (const float* inL, const float* inR, int numSamples) {
    const float* in[2] = { inL, inR };
    juce::AudioIODeviceCallbackContext ctx;
    captureCb->audioDeviceIOCallbackWithContext (in, 2, nullptr, 0, numSamples, ctx);
}

void AudioEngine::driveCaptureCallbackMono (const float* in0, int numSamples) {
    const float* in[1] = { in0 };
    juce::AudioIODeviceCallbackContext ctx;
    captureCb->audioDeviceIOCallbackWithContext (in, 1, nullptr, 0, numSamples, ctx);
}

void AudioEngine::driveRenderCallback (float* outL, float* outR, int numSamples) {
    float* out[2] = { outL, outR };
    juce::AudioIODeviceCallbackContext ctx;
    renderCb->audioDeviceIOCallbackWithContext (nullptr, 0, out, 2, numSamples, ctx);
}

} // namespace eb
