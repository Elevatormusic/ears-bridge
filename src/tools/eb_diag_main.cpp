// Console diagnostic for Plan 2: lists devices with detected model + native rates/bit-depths,
// then opens an EARS/EARS-Pro input + a virtual sink and runs a ~5 s passthrough.
// Usage:  eb_diag                 -> list only
//         eb_diag run "<outName>" -> list + 5 s passthrough into the named output
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/AudioEngine.h"
#include "audio/ModelDetect.h"
#include <iostream>

#if JUCE_WINDOWS
 #include <mmdeviceapi.h>
 #include <audioclient.h>
 #include <audiopolicy.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <ksmedia.h>

static juce::String pidName (DWORD pid) {
    juce::String n;
    if (HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH]; DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW (h, 0, buf, &sz))
            n = juce::String (buf).fromLastOccurrenceOf ("\\", false, false);
        CloseHandle (h);
    }
    return n;
}

// sessions "<name filter>": list the audio SESSIONS (and owning process) on matching capture
// endpoints -- reveals WHICH process is recording the cable (e.g. DiracLive.exe vs
// DiracLiveProcessor.exe). Note: only SHARED-mode streams appear as sessions.
static int probeSessions (const juce::String& filter) {
    if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) { std::cout << "CoInitialize failed\n"; return 30; }
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &en);
    IMMDeviceCollection* coll = nullptr;
    if (en) en->EnumAudioEndpoints (eCapture, DEVICE_STATE_ACTIVE, &coll);
    UINT count = 0; if (coll) coll->GetCount (&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr; coll->Item (i, &dev);
        IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit (&nm); ps->GetValue (PKEY_Device_FriendlyName, &nm);
        juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
        if (name.containsIgnoreCase (filter)) {
            std::cout << "== " << name << " ==\n";
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED (dev->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**) &mgr))) {
                IAudioSessionEnumerator* se = nullptr; mgr->GetSessionEnumerator (&se);
                int cnt = 0; if (se) se->GetCount (&cnt);
                if (cnt == 0) std::cout << "  (no shared-mode capture sessions)\n";
                for (int s = 0; s < cnt; ++s) {
                    IAudioSessionControl* ctrl = nullptr; se->GetSession (s, &ctrl);
                    IAudioSessionControl2* c2 = nullptr;
                    if (ctrl) ctrl->QueryInterface (__uuidof (IAudioSessionControl2), (void**) &c2);
                    DWORD pid = 0; if (c2) c2->GetProcessId (&pid);
                    AudioSessionState st = AudioSessionStateInactive; if (ctrl) ctrl->GetState (&st);
                    const char* sts = st == AudioSessionStateActive ? "ACTIVE" : st == AudioSessionStateInactive ? "inactive" : "expired";
                    std::cout << "  pid=" << pid << " (" << pidName (pid) << ")  state=" << sts << "\n";
                    if (c2) c2->Release(); if (ctrl) ctrl->Release();
                }
                if (se) se->Release(); mgr->Release();
            }
        }
        PropVariantClear (&nm); ps->Release(); dev->Release();
    }
    if (coll) coll->Release(); if (en) en->Release(); CoUninitialize();
    return 0;
}

// Build a WAVEFORMATEXTENSIBLE for IsFormatSupported probing.
static void makeWfx (WAVEFORMATEXTENSIBLE& w, int rate, int ch, int bits, bool isFloat) {
    ZeroMemory (&w, sizeof (w));
    w.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels       = (WORD) ch;
    w.Format.nSamplesPerSec  = (DWORD) rate;
    w.Format.wBitsPerSample  = (WORD) bits;
    w.Format.nBlockAlign     = (WORD) (ch * bits / 8);
    w.Format.nAvgBytesPerSec = (DWORD) (rate * w.Format.nBlockAlign);
    w.Format.cbSize          = 22;
    w.Samples.wValidBitsPerSample = (WORD) bits;
    w.dwChannelMask = (ch == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    w.SubFormat     = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

// Probe every active capture endpoint whose name matches `filter` with the SAME WASAPI call Dirac's
// audio layer uses (IAudioClient::IsFormatSupported), across rates/channels/depths, in SHARED and
// EXCLUSIVE mode. Reveals exactly which formats the standard CABLE refuses vs the Hi-Fi cable.
static int probeFormats (const juce::String& filter) {
    if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) { std::cout << "CoInitialize failed\n"; return 20; }
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), (void**) &en))) { std::cout << "no enumerator\n"; return 21; }
    IMMDeviceCollection* coll = nullptr;
    en->EnumAudioEndpoints (eCapture, DEVICE_STATE_ACTIVE, &coll);
    UINT count = 0; if (coll) coll->GetCount (&count);
    const int rates[] = { 44100, 48000, 88200, 96000, 192000 };
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr; coll->Item (i, &dev);
        IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit (&nm); ps->GetValue (PKEY_Device_FriendlyName, &nm);
        juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
        if (name.containsIgnoreCase (filter)) {
            std::cout << "== " << name << " ==\n";
            IAudioClient* ac = nullptr;
            if (SUCCEEDED (dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &ac))) {
                // The endpoint's own mix format (what shared mode runs at):
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED (ac->GetMixFormat (&mix)) && mix) {
                    std::cout << "  mixFormat: " << mix->nSamplesPerSec << " Hz / " << mix->nChannels
                              << "ch / " << mix->wBitsPerSample << "-bit tag=" << mix->wFormatTag << "\n";
                    CoTaskMemFree (mix);
                }
                for (int r : rates) for (int ch : { 1, 2 }) for (auto pr : { std::pair<int,bool>{16,false}, {24,false}, {32,true} }) {
                    WAVEFORMATEXTENSIBLE w; makeWfx (w, r, ch, pr.first, pr.second);
                    WAVEFORMATEX* closest = nullptr;
                    HRESULT sh = ac->IsFormatSupported (AUDCLNT_SHAREMODE_SHARED,    (WAVEFORMATEX*) &w, &closest);
                    if (closest) { CoTaskMemFree (closest); closest = nullptr; }
                    HRESULT ex = ac->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*) &w, nullptr);
                    auto tag = [] (HRESULT h) { return h == S_OK ? "OK  " : h == S_FALSE ? "conv" : "NO  "; };
                    std::cout << "  " << r << "/" << ch << "ch/" << pr.first << (pr.second ? "f" : " ")
                              << "  shared=" << tag (sh) << "  exclusive=" << tag (ex) << "\n";
                }
                ac->Release();
            }
        }
        PropVariantClear (&nm); ps->Release(); dev->Release();
    }
    if (coll) coll->Release(); en->Release(); CoUninitialize();
    return 0;
}
#endif

static const char* modelName (eb::EarsModel m) {
    switch (m) { case eb::EarsModel::Ears: return "EARS";
                 case eb::EarsModel::EarsPro: return "EARS Pro";
                 default: return "—"; }
}

int main (int argc, char** argv) {
   #if JUCE_WINDOWS
    // probe "<name filter>": IsFormatSupported matrix for matching capture endpoints (raw WASAPI, no
    // JUCE engine). Run BEFORE ScopedJuceInitialiser_GUI so we own the COM apartment.
    if (argc >= 3 && juce::String (argv[1]) == "probe")
        return probeFormats (juce::String (argv[2]));
    if (argc >= 3 && juce::String (argv[1]) == "sessions")
        return probeSessions (juce::String (argv[2]));
   #endif

    juce::ScopedJuceInitialiser_GUI juceInit;   // needed for device subsystem on some OSes
    eb::AudioEngine eng;

    std::cout << "== INPUT DEVICES ==\n";
    for (auto& d : eng.inputDevices()) {
        std::cout << "  [" << modelName (d.model) << "] " << d.name << "\n";
        std::cout << "      rates:";
        for (double r : eng.supportedSampleRates (d)) std::cout << " " << (int) r;
        std::cout << "   bits:";
        for (int b : eng.supportedBitDepths (d)) std::cout << " " << b;
        std::cout << "\n";
    }
    std::cout << "== OUTPUT DEVICES ==\n";
    for (auto& d : eng.outputDevices())
        std::cout << "  " << (d.isVirtualSink ? "[virtual] " : "          ") << d.name << "\n";

    if (argc >= 3 && juce::String (argv[1]) == "run") {
        // Pick the first recognised EARS/EARS-Pro input.
        eb::DeviceId chosenIn;
        for (auto& d : eng.inputDevices())
            if (d.model != eb::EarsModel::Unknown) { chosenIn = d; break; }
        if (chosenIn.name.isEmpty()) { std::cout << "No EARS input found.\n"; return 2; }

        eb::DeviceId chosenOut;
        for (auto& d : eng.outputDevices())
            if (d.name == juce::String (argv[2])) { chosenOut = d; break; }
        if (chosenOut.name.isEmpty()) { std::cout << "Output not found: " << argv[2] << "\n"; return 3; }

        auto rates = eng.supportedSampleRates (chosenIn);
        const double rate = rates.empty() ? 48000.0 : rates.front();
        eng.setInput (chosenIn); eng.setOutput (chosenOut);
        eng.setSampleRate (rate); eng.setOutputBitDepth (24);

        juce::String err;
        if (! eng.start (err)) { std::cout << "start failed: " << err << "\n"; return 4; }
        std::cout << "Passthrough running at " << (int) rate << " Hz; time-resolved health (200 ms steps):\n";
        // Time-resolved trace: sample health every 200 ms so a one-time STARTUP transient
        // (dropped jumps once then flatlines; fifoFill settles) is distinguishable from a
        // CONTINUOUS overrun (dropped grows every step). flags shows WHICH condition latched.
        auto flagStr = [] (eb::HealthFlag f) {
            juce::String s;
            auto add = [&] (eb::HealthFlag bit, const char* n) { if (eb::any (f & bit)) s << n << " "; };
            add (eb::HealthFlag::Xrun, "Xrun");          add (eb::HealthFlag::Dropout, "Dropout");
            add (eb::HealthFlag::FifoStarved, "FifoStarved"); add (eb::HealthFlag::ExcessDrift, "ExcessDrift");
            add (eb::HealthFlag::ClipInput, "ClipIn");    add (eb::HealthFlag::ClipOutput, "ClipOut");
            add (eb::HealthFlag::LowLevel, "LowLevel");
            return s.isEmpty() ? juce::String ("-") : s.trim();
        };
        long long firstDropAtMs = -1; long long prevDropped = 0;
        for (int step = 0; step < 40; ++step) {                 // 40 * 200 ms = 8 s
            juce::Thread::sleep (200);
            auto h = eng.health();
            const long long ms = (step + 1) * 200;
            if (firstDropAtMs < 0 && h.droppedFrames > 0) firstDropAtMs = ms;
            const long long ddelta = h.droppedFrames - prevDropped; prevDropped = h.droppedFrames;
            std::cout << "  t=" << ms << "ms dropped=" << h.droppedFrames << " (+" << ddelta << ")"
                      << " fifoFill=" << h.fifoFill << " ratio=" << h.captureToRenderRatio
                      << " clean=" << (h.cleanCapture ? "yes" : "no")
                      << " flags=[" << flagStr (h.flags) << "]\n";
        }
        auto h = eng.health(); auto lv = eng.levels();
        eng.stop();
        std::cout << "FINAL xruns=" << h.xruns << " dropped=" << h.droppedFrames
                  << " fifoFill=" << h.fifoFill << " clean=" << (h.cleanCapture ? "yes" : "no")
                  << " firstDropAt=" << firstDropAtMs << "ms\n";
        std::cout << "inL=" << lv.inL << " inR=" << lv.inR << " outMono=" << lv.outMono << "\n";
    }

    // capture "<inputName>": open the NAMED device for CAPTURE via WASAPI shared (exactly what a
    // recorder like Dirac does) and report success + peak, or the exact open error. Isolates a
    // device-wide problem from an app-specific one.
    if (argc >= 3 && juce::String (argv[1]) == "capture") {
        const juce::String name = argv[2];
        juce::AudioDeviceManager adm;
        juce::AudioIODeviceType* type = nullptr;
        for (auto* t : adm.getAvailableDeviceTypes())
            if (t->getTypeName() == "Windows Audio") { type = t; break; }
        if (type == nullptr) { std::cout << "no Windows Audio (WASAPI) type\n"; return 5; }
        type->scanForDevices();
        std::unique_ptr<juce::AudioIODevice> dev (type->createDevice ({}, name));
        if (dev == nullptr) { std::cout << "createDevice FAILED for: " << name << "\n"; return 6; }
        juce::BigInteger inCh; inCh.setRange (0, 2, true);
        juce::BigInteger noOut;
        auto err = dev->open (inCh, noOut, 48000.0, 512);
        if (err.isNotEmpty()) { std::cout << "OPEN FAILED: " << err << "\n"; return 7; }
        std::cout << "OPEN OK: rate=" << dev->getCurrentSampleRate()
                  << " buf=" << dev->getCurrentBufferSizeSamples()
                  << " inCh=" << dev->getActiveInputChannels().toInteger() << "\n";
        struct Cb : juce::AudioIODeviceCallback {
            std::atomic<float> peak { 0.0f };
            void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const*, int,
                                                   int n, const juce::AudioIODeviceCallbackContext&) override {
                float p = 0; for (int c = 0; c < numIn; ++c) if (in[c]) for (int i = 0; i < n; ++i) p = juce::jmax (p, std::abs (in[c][i]));
                peak.store (juce::jmax (peak.load(), p));
            }
            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
        } cb;
        dev->start (&cb);
        juce::Thread::sleep (1500);
        dev->stop(); dev->close();
        std::cout << "captured 1.5s OK, peak=" << cb.peak.load() << "\n";
    }

    // duplex "<renderName>" "<captureName>": ONE process opens the render endpoint (silence) via
    // WASAPI SHARED, then tries to open the capture endpoint via WASAPI SHARED while the render is
    // live -- exactly EARS-Bridge-feeds-the-cable + something-records-it, but both guaranteed shared.
    // If the capture open FAILS here, the cable cannot do simultaneous shared render+capture on this
    // system (a VB-Audio/system problem). If it SUCCEEDS, then whatever blocks it elsewhere is using
    // a non-shared (exclusive) open.
    if (argc >= 4 && juce::String (argv[1]) == "duplex") {
        const juce::String renderName = argv[2], captureName = argv[3];
        juce::AudioDeviceManager adm;
        juce::AudioIODeviceType* type = nullptr;
        for (auto* t : adm.getAvailableDeviceTypes())
            if (t->getTypeName() == "Windows Audio") { type = t; break; }
        if (type == nullptr) { std::cout << "no Windows Audio type\n"; return 5; }
        type->scanForDevices();

        std::unique_ptr<juce::AudioIODevice> rdev (type->createDevice (renderName, {}));   // render-only
        if (rdev == nullptr) { std::cout << "render createDevice FAILED: " << renderName << "\n"; return 8; }
        juce::BigInteger noIn, outCh; outCh.setRange (0, 2, true);
        auto rerr = rdev->open (noIn, outCh, 48000.0, 512);
        if (rerr.isNotEmpty()) { std::cout << "RENDER OPEN FAILED: " << rerr << "\n"; return 9; }
        struct RCb : juce::AudioIODeviceCallback {
            void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                                   int n, const juce::AudioIODeviceCallbackContext&) override {
                for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear (out[c], n);
            }
            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
        } rcb;
        rdev->start (&rcb);
        std::cout << "RENDER OPEN OK (feeding " << renderName << " shared) ...\n";
        juce::Thread::sleep (400);   // let the render stream establish

        std::unique_ptr<juce::AudioIODevice> cdev (type->createDevice ({}, captureName));   // capture-only
        if (cdev == nullptr) { std::cout << "capture createDevice FAILED: " << captureName << "\n"; rdev->stop(); rdev->close(); return 10; }
        juce::BigInteger inCh; inCh.setRange (0, 2, true); juce::BigInteger noOut;
        auto cerr = cdev->open (inCh, noOut, 48000.0, 512);
        if (cerr.isNotEmpty()) {
            std::cout << "CAPTURE OPEN FAILED while render active: " << cerr << "\n";
            std::cout << "=> the cable can NOT do simultaneous shared render+capture on this system.\n";
            rdev->stop(); rdev->close(); return 11;
        }
        struct CCb : juce::AudioIODeviceCallback {
            void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                                   const juce::AudioIODeviceCallbackContext&) override {}
            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
        } ccb;
        cdev->start (&ccb);
        juce::Thread::sleep (800);
        cdev->stop(); cdev->close(); rdev->stop(); rdev->close();
        std::cout << "CAPTURE OPEN OK while render active => simultaneous shared render+capture WORKS.\n";
    }
    return 0;
}
