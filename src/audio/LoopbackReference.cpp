#include "audio/LoopbackReference.h"

#include <juce_dsp/juce_dsp.h>                      // juce::dsp::FFT (out-of-place) for the single-ridge self-test
#include <juce_cryptography/juce_cryptography.h>    // juce::SHA256 for contentHash
#include <cmath>
#include <algorithm>
#include <functional>   // std::function (the Dirac-version XML tree walk)
#include <vector>

namespace eb {

namespace {

// A clipping RUN is this many consecutive samples at/near full scale. A single
// full-scale sample is just a peak; a sustained run is an overload that smears
// the sweep. ~1 ms at 48 k.
constexpr int   kClipRunSamples = 48;
constexpr float kClipThreshold  = 0.999f;   // |sample| >= this counts toward a clip run

float peakAbs (const float* x, int n) {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max (p, std::abs (x[i]));
    return p;
}

// Longest run of consecutive samples whose |value| >= kClipThreshold.
int longestClipRun (const float* x, int n) {
    int longest = 0, run = 0;
    for (int i = 0; i < n; ++i) {
        if (std::abs (x[i]) >= kClipThreshold) { if (++run > longest) longest = run; }
        else run = 0;
    }
    return longest;
}

// ---- The clean-single-sweep self-test -------------------------------------
// A Dirac sweep is an exponential sine sweep: a SINGLE tone whose frequency
// rises monotonically over time — its spectrogram is one diagonal ridge. We
// detect exactly that structure (no reference signal needed, so it works without
// knowing Dirac's exact sweep parameters):
//   - per STFT frame, the spectral CREST (dominant-bin energy / total energy)
//     is high (one tone dominates each instant);
//   - the dominant-bin index trends UPWARD across frames (monotone rising).
// A second source (a steady tone mixed onto the sweep) puts a SECOND peak in
// every frame -> per-frame crest collapses AND the dominant bin sticks at the
// tone instead of rising. Broadband noise has a flat spectrum -> low crest in
// every frame. Either way the ridge metrics fall below the gates below.
struct RidgeMetrics {
    float meanCrest    = 0.0f;   // mean per-frame dominant-bin energy fraction (0..1)
    float monotonicity = 0.0f;   // fraction of voiced frame-to-frame steps where the ridge rises (0..1)
    int   voicedFrames = 0;      // frames with enough energy to have a defined ridge
};

// PROVISIONAL gates (on-device ratification, like the match-gate cutoffs): tuned
// on the synthetic harness. Measured per-frame mean spectral crest:
//   - clean ESS (one tone per instant):        ~0.63, ridge rises 1.00
//   - sweep + an equal-level steady tone:       ~0.32 (a second peak in every
//                                                frame halves the crest), ridge 0.85
//   - broadband noise (flat spectrum):          ~0.02 crest
// A crest gate of 0.45 sits ~0.18 above the clean score and ~0.13 below the
// contaminated score, separating clean from BOTH contamination and noise; the
// monotonicity gate is a complementary check (a clean rising ridge is ~1.0; noise
// and a stalled ridge fall below). On-device ratification is carried in the plan.
constexpr float kMinMeanCrest    = 0.45f;
constexpr float kMinMonotonicity = 0.75f;

// Per-frame minimum energy (relative to the loudest frame) to count as "voiced".
constexpr float kVoicedFloor = 0.01f;

RidgeMetrics analyseRidge (const float* x, int n, double /*rate*/) {
    RidgeMetrics rm;
    constexpr int kFftOrder = 10;            // 1024-point frames
    constexpr int kFrame    = 1 << kFftOrder;
    const int     kHop      = kFrame / 2;    // 50% overlap
    const int     kBins     = kFrame / 2;    // positive-frequency bins
    if (n < kFrame * 2) return rm;

    juce::dsp::FFT fft (kFftOrder);
    std::vector<float> in  ((size_t) kFrame * 2, 0.0f);
    std::vector<float> out ((size_t) kFrame * 2, 0.0f);
    std::vector<float> mag ((size_t) kBins, 0.0f);

    // Pass 1: gather per-frame total energy to set the voiced floor.
    std::vector<int>   domBin;
    std::vector<float> crest;
    std::vector<float> frameEnergy;
    float maxFrameEnergy = 0.0f;

    for (int start = 0; start + kFrame <= n; start += kHop) {
        std::fill (in.begin(), in.end(), 0.0f);
        for (int i = 0; i < kFrame; ++i) {
            // Hann window so the dominant-bin crest isn't smeared by leakage.
            const float w = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                     * (float) i / (float) (kFrame - 1)));
            in[(size_t) i * 2] = x[start + i] * w;
        }
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (in.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (out.data()), false);

        double total = 0.0; int peakBin = 0; float peakMag = 0.0f;
        for (int k = 1; k < kBins; ++k) {          // skip DC
            const float re = out[(size_t) k * 2], im = out[(size_t) k * 2 + 1];
            const float m  = re * re + im * im;    // power
            mag[(size_t) k] = m;
            total += m;
            if (m > peakMag) { peakMag = m; peakBin = k; }
        }
        const float frameCrest = (total > 0.0) ? (float) ((double) peakMag / total) : 0.0f;
        crest.push_back (frameCrest);
        domBin.push_back (peakBin);
        frameEnergy.push_back ((float) total);
        maxFrameEnergy = std::max (maxFrameEnergy, (float) total);
    }
    if (frameEnergy.empty() || maxFrameEnergy <= 0.0f) return rm;

    // Pass 2: aggregate over VOICED frames only.
    const float floorE = maxFrameEnergy * kVoicedFloor;
    double crestSum = 0.0; int voiced = 0;
    int prevBin = -1; int rising = 0, steps = 0;
    for (size_t f = 0; f < frameEnergy.size(); ++f) {
        if (frameEnergy[f] < floorE) { prevBin = -1; continue; }
        crestSum += crest[f];
        ++voiced;
        if (prevBin >= 0) {
            ++steps;
            if (domBin[f] >= prevBin) ++rising;     // ridge rises (or holds) -> monotone-up
        }
        prevBin = domBin[f];
    }
    rm.voicedFrames = voiced;
    rm.meanCrest    = voiced > 0 ? (float) (crestSum / voiced) : 0.0f;
    rm.monotonicity = steps  > 0 ? (float) rising / (float) steps : 0.0f;
    return rm;
}

} // namespace

// ---------------------------------------------------------------------------
// PURE validation
// ---------------------------------------------------------------------------
ReferenceValidation validateReferenceCapture (const float* samples, int n, double rate,
                                              double minSeconds, double minPeakDb, double maxPeakDb) {
    if (samples == nullptr || n <= 0 || rate <= 0.0)
        return { false, "no capture data" };

    // 1. LENGTH — the L-then-R Dirac sweep sequence is ~25 s; a short capture
    //    cannot contain the whole thing.
    const double seconds = (double) n / rate;
    if (seconds < minSeconds)
        return { false, "capture too short - the full Dirac sweep sequence is ~"
                        + juce::String ((int) std::lround (minSeconds))
                        + " s (got " + juce::String (seconds, 1) + " s); re-run the sweep" };

    // 2. LEVEL — peak in a sane window, and no sustained clipping run.
    const int clipRun = longestClipRun (samples, n);
    if (clipRun >= kClipRunSamples)
        return { false, "capture clips - a clipping run of " + juce::String (clipRun)
                        + " samples was found; reduce the level and re-run" };

    const float peak = peakAbs (samples, n);
    const float peakDb = peak > 0.0f ? 20.0f * std::log10 (peak) : -144.0f;
    if (peakDb < minPeakDb)
        return { false, "capture too quiet - peak " + juce::String (peakDb, 1)
                        + " dBFS is below the " + juce::String (minPeakDb, 0)
                        + " dBFS floor; raise the level and re-run" };
    if (peakDb > maxPeakDb)
        return { false, "capture level too hot - peak " + juce::String (peakDb, 1)
                        + " dBFS exceeds " + juce::String (maxPeakDb, 0)
                        + " dBFS; reduce the level and re-run" };

    // 3. SINGLE-SOURCE self-test — the clean-single-sweep check. A Dirac sweep is
    //    ONE tone whose frequency rises monotonically: its spectrogram is a single
    //    diagonal ridge. analyseRidge() asserts exactly that (high per-frame
    //    spectral crest + a monotonically-rising dominant bin). A second source (a
    //    steady tone mixed onto the sweep) adds a second peak in every frame, which
    //    pulls the per-frame crest down and stalls the ridge; broadband noise has a
    //    flat spectrum (low crest in every frame). Either fails the gates below.
    const RidgeMetrics ridge = analyseRidge (samples, n, rate);
    const bool singleSweep = ridge.voicedFrames >= 4
                          && ridge.meanCrest    >= kMinMeanCrest
                          && ridge.monotonicity >= kMinMonotonicity;
    if (! singleSweep)
        return { false, "capture is not a clean single sweep - a second source or noise was "
                        "detected (tone-purity " + juce::String (ridge.meanCrest, 2)
                        + ", rising " + juce::String (ridge.monotonicity, 2)
                        + "); silence the room and re-run the sweep" };

    return { true, {} };
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------
// Pure classifier (cross-platform, testable): does the Dirac deviceType string name a
// Windows-Audio / WASAPI shared output? Case-insensitive substring match on "windows
// audio" or "wasapi". Unknown/empty -> false (we can't confirm Windows Audio, so the
// GUI errs toward the cautionary hint rather than a false all-clear).
bool diracDeviceTypeIsWindowsAudio (const juce::String& deviceType) {
    if (deviceType.isEmpty()) return false;
    return deviceType.containsIgnoreCase ("windows audio")
        || deviceType.containsIgnoreCase ("wasapi");
}

ReferenceMetadata makeReferenceMetadata (const float* samples, int n, double rate) {
    ReferenceMetadata md;
    md.rate          = rate;
    md.lengthSamples = juce::jmax (0, n);
    md.diracVersion  = readDiracVersion();
    if (samples != nullptr && n > 0)
        md.contentHash = juce::SHA256 (samples, (size_t) n * sizeof (float)).toHexString();
    return md;
}

} // namespace eb

// ===========================================================================
// Platform-specific: Dirac version read + the WASAPI loopback capture.
// Mirrors the EndpointFormat/EndpointUid split — the Windows impl plus a
// non-Apple/other fallback live here; macOS gets the stubs too (no WASAPI).
// ===========================================================================
#if JUCE_WINDOWS

#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>

namespace eb {

juce::String readDiracVersion() {
    // %APPDATA%\Dirac\Dirac_Live_Processor\DiracLiveProcessor.settings — best
    // effort. The file is XML-ish; look for a version attribute/element if present.
    auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    juce::File settings = appData.getChildFile ("Dirac")
                                 .getChildFile ("Dirac_Live_Processor")
                                 .getChildFile ("DiracLiveProcessor.settings");
    if (! settings.existsAsFile())
        return {};

    const juce::String text = settings.loadFileAsString();
    if (text.isEmpty())
        return {};

    // Try to parse as XML and read a "version" attribute anywhere in the tree.
    if (auto xml = juce::parseXML (text)) {
        std::function<juce::String (const juce::XmlElement&)> findVersion =
            [&] (const juce::XmlElement& e) -> juce::String {
                for (auto* name : { "version", "Version", "appVersion", "AppVersion" })
                    if (e.hasAttribute (name))
                        return e.getStringAttribute (name);
                for (auto* child : e.getChildIterator()) {
                    auto v = findVersion (*child);
                    if (v.isNotEmpty()) return v;
                }
                return {};
            };
        auto v = findVersion (*xml);
        if (v.isNotEmpty()) return v;
    }

    // Fallback: a loose textual scan for version="x.y.z".
    const int idx = text.indexOfIgnoreCase ("version");
    if (idx >= 0) {
        auto after = text.substring (idx);
        auto q = after.fromFirstOccurrenceOf ("\"", false, false)
                      .upToFirstOccurrenceOf ("\"", false, false);
        if (q.isNotEmpty() && q.length() < 32) return q.trim();
    }
    return {};
}

juce::String readDiracDeviceType() {
    // READ-ONLY: the same settings file, looking for a "deviceType" attribute anywhere in the tree
    // (the output/playback device's type — "Windows Audio", "ASIO", ...). We never write the file.
    auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    juce::File settings = appData.getChildFile ("Dirac")
                                 .getChildFile ("Dirac_Live_Processor")
                                 .getChildFile ("DiracLiveProcessor.settings");
    if (! settings.existsAsFile())
        return {};
    const juce::String text = settings.loadFileAsString();
    if (text.isEmpty())
        return {};

    if (auto xml = juce::parseXML (text)) {
        std::function<juce::String (const juce::XmlElement&)> findType =
            [&] (const juce::XmlElement& e) -> juce::String {
                for (auto* name : { "deviceType", "DeviceType", "outputType", "OutputDeviceType", "audioType" })
                    if (e.hasAttribute (name))
                        return e.getStringAttribute (name);
                for (auto* child : e.getChildIterator()) {
                    auto v = findType (*child);
                    if (v.isNotEmpty()) return v;
                }
                return {};
            };
        auto v = findType (*xml);
        if (v.isNotEmpty()) return v;
    }

    // Fallback: a loose textual scan for deviceType="...".
    const int idx = text.indexOfIgnoreCase ("deviceType");
    if (idx >= 0) {
        auto after = text.substring (idx);
        auto q = after.fromFirstOccurrenceOf ("\"", false, false)
                      .upToFirstOccurrenceOf ("\"", false, false);
        if (q.isNotEmpty() && q.length() < 48) return q.trim();
    }
    return {};
}

namespace {

// Decode one packet's worth of mix-format bytes into mono float samples (average
// the channels) appended to `out`. Mirrors eb_diag's packetPeak decode paths.
void decodePacketToMono (const unsigned char* data, UINT32 frames, WORD channels,
                         WORD bits, bool isFloat, std::vector<float>& out) {
    const int ch = juce::jmax (1, (int) channels);
    auto pushMono = [&] (auto sampleAt) {
        for (UINT32 f = 0; f < frames; ++f) {
            float acc = 0.0f;
            for (int c = 0; c < ch; ++c) acc += sampleAt ((size_t) f * ch + c);
            out.push_back (acc / (float) ch);
        }
    };
    if (isFloat && bits == 32) {
        auto* s = reinterpret_cast<const float*> (data);
        pushMono ([&] (size_t i) { return s[i]; });
    } else if (bits == 16) {
        auto* s = reinterpret_cast<const short*> (data);
        pushMono ([&] (size_t i) { return (float) s[i] / 32768.0f; });
    } else if (bits == 24) {
        pushMono ([&] (size_t i) {
            const unsigned char* p = data + i * 3;
            int v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;            // sign-extend 24->32
            return (float) v / 8388608.0f;
        });
    } else if (bits == 32) {                              // 32-bit signed PCM
        auto* s = reinterpret_cast<const int*> (data);
        pushMono ([&] (size_t i) { return (float) s[i] / 2147483648.0f; });
    }
}

} // namespace

LoopbackCaptureResult captureLoopback (const juce::String& filter, double seconds, double expectedRate) {
    LoopbackCaptureResult res;

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);

    auto fail = [&] (juce::String why) -> LoopbackCaptureResult {
        if (weInited) CoUninitialize();
        res.ok = false; res.reason = std::move (why); return res;
    };

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), (void**) &en)) || en == nullptr)
        return fail ("no audio endpoint enumerator");

    IMMDeviceCollection* coll = nullptr;
    en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT count = 0; if (coll) coll->GetCount (&count);

    // First eRender endpoint whose FriendlyName contains the substring.
    IMMDevice* match = nullptr; juce::String matchName;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr; coll->Item (i, &dev);
        IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit (&nm);
        if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
        juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
        PropVariantClear (&nm); if (ps) ps->Release();
        if (match == nullptr && name.containsIgnoreCase (filter)) { match = dev; matchName = name; }
        else dev->Release();
    }
    if (coll) coll->Release();
    if (match == nullptr) { en->Release(); return fail ("no render endpoint matched \"" + filter + "\""); }

    auto releaseAndFail = [&] (juce::String why) -> LoopbackCaptureResult {
        match->Release(); en->Release(); return fail (std::move (why));
    };

    IAudioClient* ac = nullptr;
    if (FAILED (match->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &ac)) || ac == nullptr)
        return releaseAndFail ("could not activate the render endpoint");

    WAVEFORMATEX* mix = nullptr;
    if (FAILED (ac->GetMixFormat (&mix)) || mix == nullptr) {
        ac->Release(); return releaseAndFail ("could not read the endpoint mix format");
    }

    const DWORD rate     = mix->nSamplesPerSec;
    const WORD  channels = mix->nChannels;
    const WORD  bits     = mix->wBitsPerSample;
    bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
        auto* wx = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (mix);
        isFloat = (wx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    res.rate     = (double) rate;
    res.channels = (int) channels;

    // MIXER TRANSPARENCY: the endpoint's shared mix rate must equal the expected
    // rate, else the OS would resample Dirac's sweep before we capture it and we'd
    // store a resampled reference. Reject up front.
    if (expectedRate > 0.0 && (double) rate != expectedRate) {
        CoTaskMemFree (mix); ac->Release();
        return releaseAndFail ("endpoint mix rate is " + juce::String ((int) rate)
                               + " Hz, not " + juce::String ((int) expectedRate)
                               + " Hz - set the render device to " + juce::String ((int) expectedRate)
                               + " Hz so the reference is captured without resampling");
    }

    const REFERENCE_TIME hnsBuffer = 10000000;   // 1 s
    HRESULT hr = ac->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 hnsBuffer, 0, mix, nullptr);
    if (FAILED (hr)) {
        CoTaskMemFree (mix); ac->Release();
        return releaseAndFail ("loopback init failed (hr=0x"
                               + juce::String::toHexString ((int) hr)
                               + ") - the endpoint may be held in exclusive mode");
    }

    IAudioCaptureClient* cap = nullptr;
    if (FAILED (ac->GetService (__uuidof (IAudioCaptureClient), (void**) &cap)) || cap == nullptr) {
        CoTaskMemFree (mix); ac->Release();
        return releaseAndFail ("could not obtain the capture client");
    }

    if (FAILED (ac->Start())) {
        cap->Release(); CoTaskMemFree (mix); ac->Release();
        return releaseAndFail ("could not start the loopback capture");
    }

    res.samples.reserve ((size_t) ((double) rate * juce::jmax (0.0, seconds)));
    long long nonSilent = 0, totalFrames = 0;

    const juce::int64 endMs = juce::Time::currentTimeMillis()
                            + (juce::int64) (juce::jmax (0.0, seconds) * 1000.0);
    while (juce::Time::currentTimeMillis() < endMs) {
        UINT32 packetFrames = 0;
        if (FAILED (cap->GetNextPacketSize (&packetFrames))) break;
        if (packetFrames == 0) { juce::Thread::sleep (5); continue; }
        BYTE* data = nullptr; UINT32 got = 0; DWORD flags = 0;
        if (FAILED (cap->GetBuffer (&data, &got, &flags, nullptr, nullptr))) break;
        totalFrames += got;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            res.samples.insert (res.samples.end(), (size_t) got, 0.0f);   // loopback silence -> zeros
        } else {
            const size_t before = res.samples.size();
            decodePacketToMono (data, got, channels, bits, isFloat, res.samples);
            for (size_t i = before; i < res.samples.size(); ++i)
                if (std::abs (res.samples[i]) > 0.0001f) { ++nonSilent; break; }
        }
        cap->ReleaseBuffer (got);
    }
    ac->Stop();

    cap->Release(); CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release();
    if (weInited) CoUninitialize();

    if (totalFrames <= 0 || nonSilent <= 0)
        { res.ok = false; res.reason = "captured silence - nothing was playing on the render endpoint"; return res; }

    res.ok = true;
    return res;
}

} // namespace eb

#else   // ---- non-Windows: no WASAPI loopback; pure stubs ----

namespace eb {

juce::String readDiracVersion()    { return {}; }
juce::String readDiracDeviceType() { return {}; }

LoopbackCaptureResult captureLoopback (const juce::String&, double, double) {
    LoopbackCaptureResult res;
    res.ok = false;
    res.reason = "loopback capture is only available on Windows";
    return res;
}

} // namespace eb

#endif
