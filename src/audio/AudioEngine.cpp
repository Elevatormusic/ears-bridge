#include "audio/AudioEngine.h"
#include <cmath>
namespace eb {

int AudioEngine::nextPow2 (int v) {
    int p = 1; while (p < v) p <<= 1; return p;
}

int firTapsForRate (double sampleRate) {
    const double scaled = 8192.0 * (sampleRate / 48000.0);
    int v = (int) std::ceil (scaled);
    int p = 1; while (p < v) p <<= 1; return p;
}

// ---- Audio callback adapters ----------------------------------------------------------
// Capture: 2ch in -> ProcessingGraph -> mono -> bridge.pushCapture. No alloc/lock/syscall.
struct AudioEngine::CaptureCallback : juce::AudioIODeviceCallback {
    AudioEngine& e;
    std::vector<float> mono;   // sized in prepare; NOT resized on the callback
    explicit CaptureCallback (AudioEngine& o) : e (o) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override {
        mono.assign ((size_t) juce::jmax (1, dev->getCurrentBufferSizeSamples()), 0.0f);
    }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* /*out*/, int /*numOut*/,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override {
        if (numIn < 2 || (int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        const float* l = in[0]; const float* r = in[1];
        // Input level/clip telemetry.
        float pkL = 0, pkR = 0;
        for (int i = 0; i < numSamples; ++i) { pkL = juce::jmax (pkL, std::abs (l[i])); pkR = juce::jmax (pkR, std::abs (r[i])); }
        e.hm.reportInLevels (pkL, pkR, pkL >= 0.999f, pkR >= 0.999f);

        e.graph.process (l, r, mono.data(), numSamples);   // per-ear FIR + combine
        e.bridge.pushCapture (mono.data(), numSamples);
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
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* /*in*/, int /*numIn*/,
                                           float* const* out, int numOut,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override {
        if ((int) mono.size() < numSamples) { e.hm.reportXrun(); return; }
        e.bridge.pullRender (mono.data(), numSamples);
        float pk = 0;
        for (int i = 0; i < numSamples; ++i) pk = juce::jmax (pk, std::abs (mono[i]));
        e.hm.reportOutLevel (pk, pk >= 0.999f);
        for (int ch = 0; ch < numOut; ++ch)            // duplicate mono to both channels
            if (out[ch] != nullptr)
                juce::FloatVectorOperations::copy (out[ch], mono.data(), numSamples);
        e.hm.setFifoFill (e.bridge.fifoFill());
    }
};

// ---- Lifecycle ------------------------------------------------------------------------
AudioEngine::AudioEngine() {
    captureCb = std::make_unique<CaptureCallback> (*this);
    renderCb  = std::make_unique<RenderCallback>  (*this);
    devices.rescan();
}
AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::rescanDevices() { devices.rescan(); }

std::vector<DeviceId> AudioEngine::inputDevices()  const { return devices.inputs(); }
std::vector<DeviceId> AudioEngine::outputDevices() const { return devices.outputs(); }

std::vector<double> AudioEngine::supportedSampleRates (const DeviceId& id) const {
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
void AudioEngine::setCombineMode (eb::CombineMode m) { graph.setCombineMode (m); }

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
    auto h = hm.snapshot();
    h.fifoFill = bridge.fifoFill();
    // Report the actual trimmed capture:render ratio the ClockBridge control loop is using.
    // devices.outputDevice() is const-qualified in DeviceManager, so this stays const-legal.
    double ren = activeRate;
    if (auto* od = devices.outputDevice()) ren = od->getCurrentSampleRate();
    h.captureToRenderRatio = activeRate / juce::jmax (1.0, ren);
    return h;
}

bool AudioEngine::start (juce::String& errorOut) {
    if (status() == EngineStatus::Running) return true;
    hm.reset();

    const int bufSize = 512;   // typical WASAPI shared block; device may adjust
    auto eIn = devices.openInput (inputId, activeRate, bufSize);
    if (eIn.isNotEmpty()) { errorOut = eIn; engineStatus.store ((int) EngineStatus::Error); return false; }
    auto eOut = devices.openOutput (outputId, activeRate, bufSize, outputBits);   // best-effort depth
    if (eOut.isNotEmpty()) { errorOut = eOut; devices.closeAll(); engineStatus.store ((int) EngineStatus::Error); return false; }

    auto* inD  = devices.inputDevice();
    auto* outD = devices.outputDevice();
    const double capRate = inD->getCurrentSampleRate();
    const double renRate = outD->getCurrentSampleRate();
    const int    maxBlk  = juce::jmax (inD->getCurrentBufferSizeSamples(),
                                       outD->getCurrentBufferSizeSamples());

    graph.prepare (capRate, maxBlk);
    // Capacity: ~250 ms of capture-rate mono, power-of-two-ish, never below 4096.
    const int cap = juce::jmax (4096, juce::nextPowerOfTwo ((int) (capRate * 0.25)));
    bridge.prepare (capRate, renRate, 1, cap);
    bridge.reset();

    // Render is the master: start it last so the FIFO has primed once capture runs.
    inD->start (captureCb.get());
    outD->start (renderCb.get());
    engineStatus.store ((int) EngineStatus::Running);
    return true;
}

void AudioEngine::stop() {
    if (status() != EngineStatus::Running) { devices.closeAll(); return; }
    devices.closeAll();
    bridge.reset();
    engineStatus.store ((int) EngineStatus::Stopped);
}

// ---- Headless test seam ---------------------------------------------------------------
void AudioEngine::prepareForTest (double sampleRate, int block) {
    activeRate = sampleRate; blockSize = block;
    graph.prepare (sampleRate, block);
}
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                              float* outMono, int numSamples) {
    graph.process (inL, inR, outMono, numSamples);
}

} // namespace eb
