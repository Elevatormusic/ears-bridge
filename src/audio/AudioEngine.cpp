#include "audio/AudioEngine.h"
#include <cmath>
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
        e.session_.observeBlockPeak (pk);
        if (e.session_.consumeSweepStarted()) e.hm.resetMeasurementLatches();
        e.bridge.setSweepActive (e.session_.sweepActive());   // D6: freeze the SRC ratio during the sweep, release between sweeps

        e.hm.analyzeInputBlock (l, r, numSamples);             // detection (raw input): peak, run, NaN
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
    }
    void audioDeviceStopped() override { if (e.status() == EngineStatus::Running) e.deviceDied_.store (true); }

    void audioDeviceIOCallbackWithContext (const float* const* /*in*/, int /*numIn*/,
                                           float* const* out, int numOut,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override {
        if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
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
int  AudioEngine::autoActiveEar()          const noexcept { return graph.activeEar(); }
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
    hm.prepare (inputId.model, cap, capRate / juce::jmax (1.0, renRate));   // reset + size + NOMINAL ratio (drift detection)
    // D2: latch the raw-rail snapshot on this converged success path (always runs before a Running
    // return). reportRawRail MUST come AFTER hm.prepare() -- prepare resets flagBits, so reporting
    // before it would clear the OsResampled guidance flag this raises when the rail isn't verified.
    rawRail_ = RawRailState { devices.rawRailVerified(),
                              devices.requestedInputSampleRate(),
                              devices.endpointMixSampleRate() };
    hm.reportRawRail (rawRail_.verified);
    session_.configure (maxBlk, capRate);   // D5: size the terminal-silence window for this BLOCK size/rate (NOT the FIFO capacity)
    session_.reset();                    // fresh measurement session each run (Idle -> Preflight -> ...)
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
    bridge.setSweepActive (false);   // D6: a fresh run starts free-running; the session arms the sweep later
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
}
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                              float* outMono, int numSamples) {
    // Mirror the capture-callback ordering: feed the session this block's peak, re-scope on the sweep
    // onset, analyze, then latch Invalid if an invalidating flag fired in-measurement.
    const float pk = eb::HealthMonitor::blockPeak (inL, inR, numSamples);
    session_.observeBlockPeak (pk);
    if (session_.consumeSweepStarted()) hm.resetMeasurementLatches();
    bridge.setSweepActive (session_.sweepActive());                  // D6: mirror the capture-callback freeze sync
    hm.analyzeInputBlock (inL, inR, numSamples);                      // same analysis as the capture callback
    if (session_.inMeasurement() && ! hm.cleanCapture()) session_.markInvalid();
    if (graph.process (inL, inR, outMono, numSamples)) hm.reportNonFinite();
}

// ---- R22 callback-level test seam -----------------------------------------------------
// Build the same state start() does (without devices), then drive the production callbacks.
void AudioEngine::prepareCallbacksForTest (double sampleRate, int block, int fifoCapacity) {
    activeRate = sampleRate; blockSize = block;
    const int cap = juce::jmax (1024, fifoCapacity);

    graph.prepare (sampleRate, block);
    bridge.prepare (sampleRate, sampleRate, 1, cap);   // clears the D6 freeze state
    hm.prepare (eb::EarsModel::Ears, cap, 1.0);        // nominal capture:render ratio == 1.0 (equal rates)
    session_.configure (block, sampleRate);            // D5: size the silence window for this block/rate
    session_.reset();                                  // D5: fresh session each run (Idle -> Preflight -> ...)
    bridge.reset();
    bridge.primeToTarget();                            // mirror start(): prime to the PI setpoint, no startup underrun
    bridge.setSweepActive (false);                     // D6: a fresh run free-runs; the session arms the sweep later

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
