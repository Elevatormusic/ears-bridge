// Console diagnostic for Plan 2: lists devices with detected model + native rates/bit-depths,
// then opens an EARS/EARS-Pro input + a virtual sink and runs a ~5 s passthrough.
// Usage:  eb_diag                 -> list only
//         eb_diag run "<outName>" -> list + 5 s passthrough into the named output
//         eb_diag loopcap "<renderName>" [seconds] -> WASAPI loopback-capture a render endpoint
//                 (intercept Dirac's PLAYED sweep; saves loopcap.wav; OK-vs-SILENT verdict)
//         eb_diag selftest        -> HEADLESS reference-monitor grade-flow self-test (no hardware/GUI);
//                 prints a per-scenario trace + "SELFTEST: N/N passed", writes selftest.log, exit!=0 on FAIL
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/AudioEngine.h"
#include "audio/ModelDetect.h"
#include "audio/ReferenceGradePoller.h"   // selftest: the headless grade-poll decision
#include "audio/RefMonitor.h"             // selftest: RefMonState
#include "diag/DiagnosticLog.h"           // selftest: write the trace through the real log path
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>

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

// Minimal RIFF/WAVE writer for the captured loopback samples. Writes a self-describing header
// matching the device mix format (float or PCM, channel count, rate, bit depth) followed by the
// raw interleaved bytes so the user can open the file in any editor. Returns true on success.
static bool writeWav (const juce::String& path, const std::vector<unsigned char>& pcm,
                      DWORD rate, WORD channels, WORD bits, bool isFloat) {
    FILE* f = nullptr;
   #if defined(_MSC_VER)
    _wfopen_s (&f, path.toWideCharPointer(), L"wb");
   #else
    f = fopen (path.toRawUTF8(), "wb");
   #endif
    if (! f) return false;
    const WORD  blockAlign = (WORD) (channels * bits / 8);
    const DWORD byteRate   = rate * blockAlign;
    const DWORD dataBytes  = (DWORD) pcm.size();
    const WORD  fmtTag     = isFloat ? 3 /*WAVE_FORMAT_IEEE_FLOAT*/ : 1 /*WAVE_FORMAT_PCM*/;
    const DWORD riffSize   = 36 + dataBytes;
    auto w32 = [&] (DWORD v) { fwrite (&v, 4, 1, f); };
    auto w16 = [&] (WORD  v) { fwrite (&v, 2, 1, f); };
    fwrite ("RIFF", 1, 4, f); w32 (riffSize); fwrite ("WAVE", 1, 4, f);
    fwrite ("fmt ", 1, 4, f); w32 (16); w16 (fmtTag); w16 (channels);
    w32 (rate); w32 (byteRate); w16 (blockAlign); w16 (bits);
    fwrite ("data", 1, 4, f); w32 (dataBytes);
    if (dataBytes) fwrite (pcm.data(), 1, dataBytes, f);
    fclose (f);
    return true;
}

// Convert one captured loopback packet into a peak |sample| reading, honouring the mix format:
// IEEE float (32-bit) vs signed PCM 16/24/32. Returns the peak as a normalised [0,1] float.
static float packetPeak (const unsigned char* data, UINT32 frames, WORD channels, WORD bits, bool isFloat) {
    float peak = 0.0f;
    const size_t samples = (size_t) frames * channels;
    if (isFloat && bits == 32) {
        auto* s = reinterpret_cast<const float*> (data);
        for (size_t i = 0; i < samples; ++i) peak = juce::jmax (peak, std::abs (s[i]));
    } else if (bits == 16) {
        auto* s = reinterpret_cast<const short*> (data);
        for (size_t i = 0; i < samples; ++i) peak = juce::jmax (peak, std::abs ((float) s[i]) / 32768.0f);
    } else if (bits == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const unsigned char* p = data + i * 3;
            int v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;   // sign-extend 24->32
            peak = juce::jmax (peak, std::abs ((float) v) / 8388608.0f);
        }
    } else if (bits == 32) {  // 32-bit signed PCM
        auto* s = reinterpret_cast<const int*> (data);
        for (size_t i = 0; i < samples; ++i) peak = juce::jmax (peak, std::abs ((float) s[i]) / 2147483648.0f);
    }
    return peak;
}

// loopcap "<render-name filter>" [seconds]: WASAPI LOOPBACK capture of a RENDER (eRender) endpoint.
// Opens the matching playback device in SHARED mode with AUDCLNT_STREAMFLAGS_LOOPBACK, which makes
// the IAudioCaptureClient deliver whatever is being PLAYED to that endpoint -- i.e. Dirac Live's
// digital sweep. Saves the clean reference to loopcap.wav next to the exe and reports OK-vs-SILENT.
// This de-risks the reference-based measurement plan: it proves we can intercept Dirac's played sweep.
static int loopCapture (const juce::String& filter, int seconds) {
    if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) { std::cout << "CoInitialize failed\n"; return 40; }
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), (void**) &en))) { std::cout << "no enumerator\n"; CoUninitialize(); return 41; }
    IMMDeviceCollection* coll = nullptr;
    en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT count = 0; if (coll) coll->GetCount (&count);

    // Find the first eRender endpoint whose FriendlyName contains the (case-insensitive) substring.
    IMMDevice* match = nullptr; juce::String matchName;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr; coll->Item (i, &dev);
        IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit (&nm); ps->GetValue (PKEY_Device_FriendlyName, &nm);
        juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
        PropVariantClear (&nm); ps->Release();
        if (match == nullptr && name.containsIgnoreCase (filter)) { match = dev; matchName = name; }
        else dev->Release();
    }
    if (match == nullptr) {
        std::cout << "No RENDER endpoint matched \"" << filter << "\". Available render endpoints:\n";
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr; coll->Item (i, &dev);
            IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm); ps->GetValue (PKEY_Device_FriendlyName, &nm);
            juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            std::cout << "  - " << name << "\n";
            PropVariantClear (&nm); ps->Release(); dev->Release();
        }
        if (coll) coll->Release(); en->Release(); CoUninitialize();
        return 42;
    }
    if (coll) coll->Release();
    std::cout << "Matched RENDER endpoint: " << matchName << "\n";

    IAudioClient* ac = nullptr;
    if (FAILED (match->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &ac)) || ac == nullptr) {
        std::cout << "Activate IAudioClient FAILED\n"; match->Release(); en->Release(); CoUninitialize(); return 43;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED (ac->GetMixFormat (&mix)) || mix == nullptr) {
        std::cout << "GetMixFormat FAILED\n"; ac->Release(); match->Release(); en->Release(); CoUninitialize(); return 44;
    }
    // Decode the mix format (handle WAVEFORMATEXTENSIBLE so float-vs-PCM is correct for the WAV + peak).
    const DWORD rate     = mix->nSamplesPerSec;
    const WORD  channels = mix->nChannels;
    const WORD  bits     = mix->wBitsPerSample;
    bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
        auto* wx = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (mix);
        isFloat = (wx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    std::cout << "  mixFormat: " << rate << " Hz / " << channels << "ch / " << bits << "-bit "
              << (isFloat ? "float" : "PCM") << "\n";

    // Initialise the LOOPBACK capture: SHARED mode + AUDCLNT_STREAMFLAGS_LOOPBACK on the RENDER
    // endpoint, using its own mix format. 1 s buffer (hns = 100-ns units). No event/playback needed.
    const REFERENCE_TIME hnsBuffer = 10000000;  // 1 second
    HRESULT hr = ac->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 hnsBuffer, 0, mix, nullptr);
    if (FAILED (hr)) {
        std::cout << "Initialize(LOOPBACK) FAILED hr=0x" << std::hex << (unsigned) hr << std::dec << "\n";
        std::cout << "=> loopback not available on this endpoint (exclusive-mode hold, or unsupported).\n";
        CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release(); CoUninitialize(); return 45;
    }
    IAudioCaptureClient* cap = nullptr;
    if (FAILED (ac->GetService (__uuidof (IAudioCaptureClient), (void**) &cap)) || cap == nullptr) {
        std::cout << "GetService(IAudioCaptureClient) FAILED\n";
        CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release(); CoUninitialize(); return 46;
    }

    std::cout << "Capturing loopback for " << seconds << " s ... (RUN A DIRAC SWEEP NOW)\n";
    if (FAILED (ac->Start())) {
        std::cout << "IAudioClient::Start FAILED\n";
        cap->Release(); CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release(); CoUninitialize(); return 47;
    }

    std::vector<unsigned char> pcm;
    const WORD frameBytes = (WORD) (channels * bits / 8);
    pcm.reserve ((size_t) rate * frameBytes * (size_t) juce::jmax (1, seconds));
    float overallPeak = 0.0f;
    long long nonSilentPackets = 0, totalPackets = 0, totalFrames = 0;

    const juce::int64 endMs = juce::Time::currentTimeMillis() + (juce::int64) seconds * 1000;
    while (juce::Time::currentTimeMillis() < endMs) {
        UINT32 packetFrames = 0;
        if (FAILED (cap->GetNextPacketSize (&packetFrames))) break;
        if (packetFrames == 0) { juce::Thread::sleep (5); continue; }   // poll loop: no data yet
        BYTE* data = nullptr; UINT32 got = 0; DWORD flags = 0;
        if (FAILED (cap->GetBuffer (&data, &got, &flags, nullptr, nullptr))) break;
        const size_t bytes = (size_t) got * frameBytes;
        ++totalPackets; totalFrames += got;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // Loopback delivers SILENT packets when nothing is playing -> write zeros, don't peak.
            pcm.insert (pcm.end(), bytes, 0);
        } else {
            const float pk = packetPeak (data, got, channels, bits, isFloat);
            if (pk > 0.0001f) ++nonSilentPackets;
            overallPeak = juce::jmax (overallPeak, pk);
            pcm.insert (pcm.end(), data, data + bytes);
        }
        cap->ReleaseBuffer (got);
    }
    ac->Stop();

    // Write the captured reference next to the running exe.
    juce::File exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    juce::File wav = exe.getSiblingFile ("loopcap.wav");
    const bool wrote = writeWav (wav.getFullPathName(), pcm, rate, channels, bits, isFloat);

    const double durSec = totalFrames > 0 ? (double) totalFrames / (double) rate : 0.0;
    const float  peakDb = overallPeak > 0.0f ? 20.0f * std::log10 (overallPeak) : -144.0f;

    std::cout << "\n==== LOOPCAP RESULT ====\n";
    std::cout << "device      : " << matchName << "\n";
    std::cout << "format      : " << rate << " Hz / " << channels << "ch / " << bits << "-bit "
              << (isFloat ? "float" : "PCM") << "\n";
    std::cout << "duration    : " << juce::String (durSec, 2) << " s  (" << totalFrames << " frames)\n";
    std::cout << "packets     : " << totalPackets << " total, " << nonSilentPackets << " non-silent\n";
    std::cout << "peak        : " << overallPeak << "  (" << juce::String (peakDb, 1) << " dBFS)\n";
    std::cout << "wav         : " << (wrote ? wav.getFullPathName() : juce::String ("WRITE FAILED")) << "\n";
    // Verdict: a real sweep peaks well above the noise floor; silence/exclusive-hold stays ~0.
    if (nonSilentPackets > 0 && overallPeak > 0.0005f)   // ~ -66 dBFS threshold
        std::cout << "VERDICT     : LOOPBACK OK - captured the playback stream (peak "
                  << juce::String (peakDb, 1) << " dBFS)\n";
    else
        std::cout << "VERDICT     : SILENT - no audio captured (wrong device, nothing playing, "
                     "or loopback blocked / exclusive-mode)\n";

    cap->Release(); CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release(); CoUninitialize();
    return (nonSilentPackets > 0 && overallPeak > 0.0005f) ? 0 : 48;
}
#endif

// =====================================================================================================
// selftest: a HEADLESS self-test of the reference-monitor grade flow. No hardware, no GUI. It synthesises
// a reference ESS + several response windows and drives them through eb::ReferenceGradePoller (the same
// match + stable-match-debounce + grade decision the GUI poll uses), printing a human-readable trace + a
// PASS/FAIL per scenario, a "SELFTEST: N/N passed" summary, and the same trace through eb::DiagnosticLog
// to a temp selftest.log (so the log-reading path is exercised). Returns non-zero on any FAIL.
// =====================================================================================================
namespace {

constexpr double kSelfPi = 3.14159265358979323846;

// Farina exponential sine sweep (matches tests/test_deconvolver.cpp / test_referencegradepoller.cpp).
std::vector<float> selfEss (int n, double fs, double f1 = 20.0, double f2 = 20000.0) {
    std::vector<float> x ((size_t) n, 0.0f);
    const double T  = (double) n / fs;
    const double w1 = 2.0 * kSelfPi * f1;
    const double w2 = 2.0 * kSelfPi * f2;
    const double K  = std::log (w2 / w1);
    const double A  = w1 * T / K;
    for (int i = 0; i < n; ++i) {
        const double t   = (double) i / fs;
        const double phi = A * (std::exp ((t / T) * K) - 1.0);
        x[(size_t) i] = (float) std::sin (phi);
    }
    const int fade = (std::min) (n / 4, (int) std::lround (0.002 * fs));
    for (int i = 0; i < fade; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kSelfPi * (float) i / (float) fade));
        x[(size_t) i]           *= w;
        x[(size_t) (n - 1 - i)] *= w;
    }
    return x;
}

std::vector<float> selfConvolve (const std::vector<float>& sig, const std::vector<float>& ir) {
    const int ns = (int) sig.size(), ni = (int) ir.size();
    std::vector<float> y ((size_t) (ns + ni - 1), 0.0f);
    for (int i = 0; i < ns; ++i) {
        const float s = sig[(size_t) i];
        if (s == 0.0f) continue;
        for (int k = 0; k < ni; ++k) y[(size_t) (i + k)] += s * ir[(size_t) k];
    }
    return y;
}

void selfScaleToPeak (std::vector<float>& x, float targetPeak) {
    float pk = 0.0f;
    for (float v : x) pk = (std::max) (pk, std::abs (v));
    if (pk <= 0.0f) return;
    const float g = targetPeak / pk;
    for (float& v : x) v *= g;
}

void selfAddNoise (std::vector<float>& x, float amp, unsigned seed) {
    for (auto& v : x) { seed = seed * 1664525u + 1013904223u; v += amp * ((float) (seed >> 9) / (float) (1u << 23) - 0.5f); }
}

const char* stateName (eb::RefMonState s) {
    switch (s) {
        case eb::RefMonState::NotLearned:     return "NotLearned";
        case eb::RefMonState::Learned:        return "Learned";
        case eb::RefMonState::ReferenceStale: return "ReferenceStale";
        case eb::RefMonState::GradedClean:    return "GradedClean";
        case eb::RefMonState::GradedSuspect:  return "GradedSuspect";
        case eb::RefMonState::NotGraded:      return "NotGraded";
    }
    return "?";
}

float dbfs (float peak) { return peak > 0.0f ? 20.0f * std::log10 (peak) : -144.0f; }

// A tiny trace sink: writes one line to BOTH std::cout and the DiagnosticLog (Info), so the log-reading
// path is exercised with the exact same trace the console shows.
struct Trace {
    eb::DiagnosticLog& log;
    void operator() (const std::string& line) {
        std::cout << line << "\n";
        log.write (eb::DiagnosticLog::Level::Info, juce::String (line));
    }
};

// A scenario = a name + a response window + the reference, and a predicate over the poller polls. We run
// the poller across `polls` consecutive polls (the throttle is a GUI concern; here each call IS a poll) and
// record: did ANY poll grade, the grade state, the count of grades, and the last coherence. The predicate
// decides PASS/FAIL from those. Traces every poll.
struct PollSummary {
    bool             anyGraded   = false;
    int              gradeCount  = 0;
    eb::RefMonState  gradeState  = eb::RefMonState::NotGraded;
    float            lastCoh     = 0.0f;
    float            gradeIrSnr  = 0.0f;
};

PollSummary runScenario (Trace& trace, const std::string& name,
                         const std::vector<float>& reference,
                         const std::vector<float>& window, double rate, int polls) {
    float pk = 0.0f; for (float v : window) pk = (std::max) (pk, std::abs (v));
    trace ("-- scenario: " + name + "  (window peak " + std::to_string (dbfs (pk)).substr (0, 6) + " dBFS, "
           + std::to_string (polls) + " polls)");
    eb::ReferenceGradePoller poller;
    PollSummary sum;
    for (int i = 0; i < polls; ++i) {
        const auto r = poller.poll (window.data(), (int) window.size(),
                                    reference.data(), (int) reference.size(), rate);
        sum.lastCoh = r.coherence;
        std::string line = "   poll " + std::to_string (i + 1)
                         + ": matched=" + (r.matched ? "yes" : "no ")
                         + " coherence=" + std::to_string (r.coherence).substr (0, 5)
                         + " didGrade=" + (r.didGrade ? "YES" : "no ");
        if (r.didGrade) {
            ++sum.gradeCount;
            sum.anyGraded  = true;
            sum.gradeState = r.state;
            sum.gradeIrSnr = r.irSnrDb;
            line += std::string (" state=") + stateName (r.state)
                  + " irSnr=" + std::to_string (r.irSnrDb).substr (0, 6) + " dB";
        }
        trace (line);
    }
    return sum;
}

bool gradedClean (const PollSummary& s) {
    return s.anyGraded && (s.gradeState == eb::RefMonState::GradedClean
                        || s.gradeState == eb::RefMonState::GradedSuspect);
}

int runSelfTest() {
    const double fs = 48000.0;
    const int    sweepLen = 1 << 18;   // 262144 samples ~= 5.46 s at 48k (fast but a full sweep)

    // The synthetic REFERENCE: a clean 20 Hz..20 kHz ESS, zero-padded to a window length (leading-silence +
    // sweep inside, the rolling-ring shape the poller expects). The clean response is the room response to it.
    auto refSweep = selfEss (sweepLen, fs);
    std::vector<float> roomIr ((size_t) 256, 0.0f); roomIr[50] = 1.0f;
    std::vector<float> cleanResp = selfConvolve (refSweep, roomIr);     // length sweepLen + 255
    std::vector<float> reference (cleanResp.size(), 0.0f);
    std::copy (refSweep.begin(), refSweep.end(), reference.begin());

    // The log: a temp selftest.log so the log-reading path is exercised. Same trace as the console.
    auto logDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("EarsBridge").getChildFile ("selftest");
    logDir.createDirectory();
    // A fresh file each run so the path holds only this run's trace.
    logDir.getChildFile ("eb.log").deleteFile();
    eb::DiagnosticLog log (logDir);
    const juce::File logFile = logDir.getChildFile ("selftest.log");
    Trace trace { log };

    trace ("==== EARS Bridge reference-monitor SELFTEST ====");
    trace (std::string ("reference: ") + std::to_string (sweepLen) + " sample ESS @ "
           + std::to_string ((int) fs) + " Hz (" + std::to_string (sweepLen / fs).substr (0, 4) + " s)");

    int passed = 0, total = 0;
    auto report = [&] (const std::string& name, bool ok) {
        ++total; if (ok) ++passed;
        trace (std::string ("   ") + (ok ? "PASS" : "FAIL") + ": " + name);
    };

    // 1) CLEAN MATCH — reference (x) a small IR, ~-25 dBFS, + a touch of noise. Expect a grade, state Graded*.
    {
        auto w = cleanResp; selfScaleToPeak (w, 0.0562f); selfAddNoise (w, 0.001f, 7u);
        auto s = runScenario (trace, "clean match (~-25 dBFS + touch of noise)", reference, w, fs, 3);
        report ("clean match grades (state Graded*)", gradedClean (s));
    }
    // 2) LOW LEVEL — ~-40 dBFS matching. Expect it STILL grades (no level gate).
    {
        auto w = cleanResp; selfScaleToPeak (w, 0.01f);
        auto s = runScenario (trace, "low level (~-40 dBFS matching)", reference, w, fs, 3);
        report ("low-level still grades (no level gate)", gradedClean (s));
    }
    // 3) NOISY — matching + heavy noise. Expect a grade (match is noise-robust); report coherence/IR-SNR.
    {
        auto w = cleanResp; selfScaleToPeak (w, 0.3f); selfAddNoise (w, 0.15f, 4242u);
        auto s = runScenario (trace, "noisy (matching + heavy noise)", reference, w, fs, 3);
        trace ("   info: noisy coherence=" + std::to_string (s.lastCoh).substr (0, 5)
               + " grade IR-SNR=" + std::to_string (s.gradeIrSnr).substr (0, 6) + " dB");
        report ("noisy match still grades (not stale)", s.anyGraded && s.gradeState != eb::RefMonState::ReferenceStale);
    }
    // 4) NON-SWEEP — a white-noise response. Expect NO clean grade (no compact lobe -> no stable match).
    {
        std::vector<float> w (reference.size(), 0.0f); selfAddNoise (w, 0.4f, 1234567u);
        auto s = runScenario (trace, "non-sweep (white noise)", reference, w, fs, 4);
        report ("non-sweep is NOT graded clean", ! gradedClean (s));
    }
    // 5) WRONG REFERENCE — the room response to a DIFFERENT sweep. Expect ReferenceStale (never clean).
    {
        auto refB = selfEss (sweepLen, fs, 100.0, 8000.0);
        std::vector<float> w = selfConvolve (refB, roomIr);
        auto s = runScenario (trace, "wrong reference (different sweep)", reference, w, fs, 4);
        // PASS if not graded clean; if it DID grade, it must be ReferenceStale.
        const bool ok = ! gradedClean (s)
                     && (! s.anyGraded || s.gradeState == eb::RefMonState::ReferenceStale);
        report ("wrong reference is ReferenceStale / not clean", ok);
    }
    // 6) DEBOUNCE — a sustained match across many polls. Expect EXACTLY ONE grade.
    {
        auto w = cleanResp; selfScaleToPeak (w, 0.3f);
        auto s = runScenario (trace, "debounce (sustained match, 6 polls)", reference, w, fs, 6);
        report ("sustained match grades exactly once", s.gradeCount == 1);
    }

    const std::string summary = "SELFTEST: " + std::to_string (passed) + "/" + std::to_string (total) + " passed";
    trace (summary);

    // Mirror the rotating eb.log to the named selftest.log path the spec asks for (so the path is stable and
    // self-describing), then print it. The DiagnosticLog wrote to eb.log; copy it to selftest.log.
    logDir.getChildFile ("eb.log").copyFileTo (logFile);
    std::cout << "selftest.log: " << logFile.getFullPathName() << "\n";

    return (passed == total) ? 0 : 1;
}

} // namespace

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
    // loopcap "<render-name>" [seconds]: WASAPI loopback-capture a render endpoint (Dirac's played
    // sweep). Raw WASAPI, so run BEFORE ScopedJuceInitialiser_GUI claims the COM apartment.
    if (argc >= 3 && juce::String (argv[1]) == "loopcap") {
        const int secs = (argc >= 4) ? juce::jlimit (1, 600, juce::String (argv[3]).getIntValue()) : 12;
        return loopCapture (juce::String (argv[2]), secs);
    }
   #endif

    juce::ScopedJuceInitialiser_GUI juceInit;   // needed for device subsystem on some OSes

    // selftest: HEADLESS reference-monitor grade-flow self-test (no hardware, no GUI). Runs BEFORE any device
    // enumeration so it can be invoked on a machine with no EARS/cable attached. Returns non-zero on any FAIL.
    if (argc >= 2 && juce::String (argv[1]) == "selftest")
        return runSelfTest();

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
