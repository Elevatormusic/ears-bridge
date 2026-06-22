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
// PURE per-channel decode (cross-platform; the testable core behind
// captureLoopbackStereo). Mirrors decodePacketToMono's per-format sample
// addressing (32f / 16 / 24 / 32-int) and the pancheck per-channel accumulator's
// index = frame*channels + c, but APPENDS the FULL per-channel sample vectors
// (ch0 -> outL, ch1 -> outR) instead of folding to mono or to block-RMS. A mono
// endpoint (channels < 2) duplicates its single channel into both outputs.
// ---------------------------------------------------------------------------
namespace detail {

void decodePacketPerChannel (const unsigned char* data, unsigned int frames, int channels,
                             int bits, bool isFloat,
                             std::vector<float>& outL, std::vector<float>& outR) {
    const int ch = juce::jmax (1, channels);
    const int cR = (ch >= 2) ? 1 : 0;   // mono endpoint -> R reads ch0 too (duplicate L into R)
    auto push = [&] (auto sampleAt) {
        for (unsigned int f = 0; f < frames; ++f) {
            outL.push_back (sampleAt ((size_t) f * ch + 0));
            outR.push_back (sampleAt ((size_t) f * ch + cR));
        }
    };
    if (isFloat && bits == 32) {
        auto* s = reinterpret_cast<const float*> (data);
        push ([&] (size_t i) { return s[i]; });
    } else if (bits == 16) {
        auto* s = reinterpret_cast<const short*> (data);
        push ([&] (size_t i) { return (float) s[i] / 32768.0f; });
    } else if (bits == 24) {
        push ([&] (size_t i) {
            const unsigned char* p = data + i * 3;
            int v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;            // sign-extend 24->32
            return (float) v / 8388608.0f;
        });
    } else if (bits == 32) {                              // 32-bit signed PCM
        auto* s = reinterpret_cast<const int*> (data);
        push ([&] (size_t i) { return (float) s[i] / 2147483648.0f; });
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// PURE active-sweep span — trim a hard-panned per-channel reference to JUST its
// own sweep, dropping the silent half (where the OTHER ear sweeps). Block-RMS over
// ~20 ms blocks; threshold at the loudest block minus 40 dB; the span is the first
// to the last above-threshold block plus a ~50 ms margin, clamped to [0, n).
// ---------------------------------------------------------------------------
ActiveSpan findActiveSpan (const float* samples, int n, double rate) {
    ActiveSpan span;
    if (samples == nullptr || n <= 0 || rate <= 0.0)
        return span;   // valid=false (no data)

    constexpr double kBlockSeconds   = 0.020;   // ~20 ms RMS blocks
    constexpr double kThresholdDb     = -40.0;  // below the loudest block -> sweep vs the ~-100 dB silent half
    constexpr double kMarginSeconds   = 0.050;  // ~50 ms guard each side of the detected span
    constexpr double kMinSpanSeconds  = 2.0;    // a plausible sweep is at least this long
    // An ABSOLUTE silence floor (~-80 dBFS): a channel whose LOUDEST block is below this carries no real
    // sweep, only the loopback noise floor. The -40 dB threshold is RELATIVE, so without this an all-quiet
    // channel (uniform low RMS) would clear its own minus-40 dB threshold everywhere and read as one long
    // "active" span. The real sweep sits ~-14 dBFS, far above this; a hard-panned silent half ~-100 dBFS, below.
    constexpr float  kSilenceFloor    = 1.0e-4f; // ~-80 dBFS

    const int blockLen = juce::jmax (1, (int) std::llround (rate * kBlockSeconds));
    const int numBlocks = (n + blockLen - 1) / blockLen;   // ceil; the last block may be partial

    // Pass 1: per-block RMS, and the loudest block (the sweep level).
    std::vector<float> blockRms ((size_t) numBlocks, 0.0f);
    float maxRms = 0.0f;
    for (int b = 0; b < numBlocks; ++b) {
        const int start = b * blockLen;
        const int end   = juce::jmin (n, start + blockLen);
        double sumSq = 0.0;
        for (int i = start; i < end; ++i) sumSq += (double) samples[i] * (double) samples[i];
        const int count = end - start;
        const float rms = count > 0 ? (float) std::sqrt (sumSq / (double) count) : 0.0f;
        blockRms[(size_t) b] = rms;
        if (rms > maxRms) maxRms = rms;
    }
    if (maxRms < kSilenceFloor)
        return span;   // valid=false (all-silent channel — no block even reaches the absolute floor)

    // Threshold 40 dB below the loudest block (linear): a hard-panned silent half (~100 dB down)
    // sits far below this, while the whole ESS sweep — flat-ish envelope and all — stays above it.
    const float threshold = maxRms * std::pow (10.0f, (float) (kThresholdDb / 20.0));

    // Pass 2: first and last block whose RMS clears the threshold.
    int firstBlock = -1, lastBlock = -1;
    for (int b = 0; b < numBlocks; ++b) {
        if (blockRms[(size_t) b] >= threshold) {
            if (firstBlock < 0) firstBlock = b;
            lastBlock = b;
        }
    }
    if (firstBlock < 0)
        return span;   // valid=false (no block cleared the threshold)

    // Convert blocks -> samples, add the margin, clamp to [0, n).
    const int margin = (int) std::llround (rate * kMarginSeconds);
    int first = firstBlock * blockLen - margin;
    int last  = juce::jmin (n, (lastBlock + 1) * blockLen) + margin;   // one-past-the-last sample
    first = juce::jlimit (0, n, first);
    last  = juce::jlimit (0, n, last);

    // Reject an implausibly short span (a stray transient, not a real sweep).
    const double spanSeconds = (double) (last - first) / rate;
    if (last <= first || spanSeconds < kMinSpanSeconds)
        return span;   // valid=false (too short)

    span.first = first;
    span.last  = last;
    span.valid = true;
    return span;
}

// ---------------------------------------------------------------------------
// PURE validation
// ---------------------------------------------------------------------------
ReferenceValidation validateReferenceCapture (const float* samples, int n, double rate,
                                              double minSeconds, double minPeakDb, double maxPeakDb) {
    if (samples == nullptr || n <= 0 || rate <= 0.0)
        return { false, "no capture data" };

    // 1. LENGTH — a SANITY FLOOR, not the old fixed ~25 s bound. With end-of-sweep
    //    auto-stop the captured length is variable (the real sequence + ~3 s trailing
    //    silence), so a legitimately-shorter-but-complete sequence must pass here; the
    //    single-sweep self-test below is the real completeness gate. This only rejects a
    //    too-short blip (a stray transient that armed + trailing-stopped).
    const double seconds = (double) n / rate;
    if (seconds < minSeconds)
        return { false, "capture too short - got only " + juce::String (seconds, 1)
                        + " s (need at least " + juce::String (minSeconds, 0)
                        + " s); re-run the Dirac sweep" };

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

// Pure parser (cross-platform, testable): pull the OUTPUT device name -- the device Dirac plays the
// sweep to -- from a DiracLiveProcessor.settings XML document. It lives on the <DEVICESETUP> element
// as audioOutputDeviceName (NOT audioInputDeviceName, which is the recording side / the cable, and is
// usually blank). Returns "" when absent/unparseable.
juce::String parseDiracOutputDeviceName (const juce::String& settingsXml) {
    if (settingsXml.isEmpty()) return {};
    if (auto xml = juce::parseXML (settingsXml)) {
        std::function<juce::String (const juce::XmlElement&)> findName =
            [&] (const juce::XmlElement& e) -> juce::String {
                for (auto* name : { "audioOutputDeviceName", "OutputDeviceName", "outputDeviceName" })
                    if (e.hasAttribute (name)) {
                        auto v = e.getStringAttribute (name);
                        if (v.isNotEmpty()) return v;
                    }
                for (auto* child : e.getChildIterator()) {
                    auto v = findName (*child);
                    if (v.isNotEmpty()) return v;
                }
                return {};
            };
        auto v = findName (*xml);
        if (v.isNotEmpty()) return v;
    }
    // Fallback: a loose textual scan for audioOutputDeviceName="...".
    const int idx = settingsXml.indexOfIgnoreCase ("audioOutputDeviceName");
    if (idx >= 0) {
        auto q = settingsXml.substring (idx).fromFirstOccurrenceOf ("\"", false, false)
                                            .upToFirstOccurrenceOf ("\"", false, false);
        if (q.isNotEmpty() && q.length() < 128) return q.trim();
    }
    return {};
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

juce::String readDiracOutputDeviceName() {
    // READ-ONLY: the device Dirac plays the sweep TO -- the loopback target for the reference capture.
    // Read the settings file and hand it to the pure parser. We never write the file.
    auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    juce::File settings = appData.getChildFile ("Dirac")
                                 .getChildFile ("Dirac_Live_Processor")
                                 .getChildFile ("DiracLiveProcessor.settings");
    if (! settings.existsAsFile())
        return {};
    return parseDiracOutputDeviceName (settings.loadFileAsString());
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

LoopbackCaptureResult captureLoopback (const juce::String& filter, double seconds, double expectedRate,
                                       const std::atomic<bool>* cancel) {
    LoopbackCaptureResult res;

    // Helper: a user-cancel result (distinct from a real failure so the GUI can show it neutrally).
    auto cancelledNow = [&] { return cancel != nullptr && cancel->load (std::memory_order_relaxed); };

    // Honour an already-set cancel BEFORE touching COM/WASAPI, so a pre-armed token
    // returns promptly without doing any device work (also the headless-test path).
    if (cancelledNow()) {
        res.ok = false; res.cancelled = true; res.reason = "cancelled"; return res;
    }

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

    // Pick the render endpoint to loop back -- entirely name-driven, so it works for ANY user's output
    // device (no model is assumed). With a NAME: prefer an EXACT FriendlyName match, then fall back to a
    // substring contains (tolerates minor formatting differences across Windows versions/locales). With
    // an EMPTY name (Dirac's setting couldn't be read): use the system DEFAULT render endpoint -- the
    // most likely place Dirac's sweep plays -- NOT an arbitrary first device and NOT the cable.
    IMMDevice* match = nullptr; juce::String matchName;
    if (filter.isEmpty()) {
        if (SUCCEEDED (en->GetDefaultAudioEndpoint (eRender, eConsole, &match)) && match != nullptr) {
            IPropertyStore* ps = nullptr; match->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm);
            if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
            matchName = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            PropVariantClear (&nm); if (ps) ps->Release();
        }
    } else {
        IMMDeviceCollection* coll = nullptr;
        en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll);
        UINT count = 0; if (coll) coll->GetCount (&count);
        IMMDevice* sub = nullptr; juce::String subName;   // best substring (contains) fallback
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr; coll->Item (i, &dev);
            IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm);
            if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
            juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            PropVariantClear (&nm); if (ps) ps->Release();
            if      (match == nullptr && name.equalsIgnoreCase   (filter)) match    = dev, matchName = name;  // exact wins
            else if (sub   == nullptr && name.containsIgnoreCase (filter)) sub      = dev, subName   = name;  // contains fallback
            else                                                          dev->Release();
        }
        if (coll) coll->Release();
        if (match == nullptr && sub != nullptr) { match = sub; matchName = subName; }   // no exact -> the contains match
        else if (sub != nullptr)                  sub->Release();                       // had exact -> drop the spare
    }
    if (match == nullptr) { en->Release(); return fail (filter.isEmpty()
        ? juce::String ("no default render endpoint found")
        : ("no render endpoint matched \"" + filter + "\"")); }

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

    // END-OF-SWEEP AUTO-STOP: `seconds` is the MAXIMUM. The detector arms on sustained
    // activity, then ends the capture once the loopback is quiet past the trailing-
    // silence threshold (~3 s) — so a sequence shorter than the cap returns early and a
    // longer one isn't truncated as long as it finishes within the cap. Driven per
    // decoded chunk with that chunk's peak and its duration in seconds.
    SweepEndDetector endDet;
    bool endedEarly = false;

    bool wasCancelled = false;
    const juce::int64 endMs = juce::Time::currentTimeMillis()
                            + (juce::int64) (juce::jmax (0.0, seconds) * 1000.0);
    while (juce::Time::currentTimeMillis() < endMs) {
        // Cooperative cancel: checked each iteration alongside the time/SILENT logic.
        // Break out and fall through to the clean Stop()+Release() below — no leaked
        // client, no half-state — then return a cancelled (not failed) result.
        if (cancelledNow()) { wasCancelled = true; break; }
        UINT32 packetFrames = 0;
        if (FAILED (cap->GetNextPacketSize (&packetFrames))) break;
        if (packetFrames == 0) { juce::Thread::sleep (5); continue; }
        BYTE* data = nullptr; UINT32 got = 0; DWORD flags = 0;
        if (FAILED (cap->GetBuffer (&data, &got, &flags, nullptr, nullptr))) break;
        totalFrames += got;
        const double chunkSeconds = rate > 0 ? (double) got / (double) rate : 0.0;
        float chunkPeak = 0.0f;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            res.samples.insert (res.samples.end(), (size_t) got, 0.0f);   // loopback silence -> zeros
        } else {
            const size_t before = res.samples.size();
            decodePacketToMono (data, got, channels, bits, isFloat, res.samples);
            for (size_t i = before; i < res.samples.size(); ++i) {
                const float a = std::abs (res.samples[i]);
                if (a > chunkPeak) chunkPeak = a;
                if (a > 0.0001f) ++nonSilent;   // note: counts samples, only used as a >0 nonSilent flag below
            }
        }
        cap->ReleaseBuffer (got);

        // Feed the chunk to the end detector. Once it reports the sweep sequence has
        // ended, stop — we've captured the whole run plus the trailing silence.
        if (endDet.update (chunkPeak, chunkSeconds)) { endedEarly = true; break; }
    }
    ac->Stop();
    juce::ignoreUnused (endedEarly);

    cap->Release(); CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release();
    if (weInited) CoUninitialize();

    // A user-cancel wins over any silence/empty verdict: report it neutrally, not as a failure.
    if (wasCancelled)
        { res.ok = false; res.cancelled = true; res.reason = "cancelled"; return res; }

    if (totalFrames <= 0 || nonSilent <= 0)
        { res.ok = false; res.reason = "captured silence - nothing was playing on the render endpoint"; return res; }

    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------
// Per-channel loopback capture (no downmix) — the per-ear grading reference.
// IDENTICAL WASAPI setup to captureLoopback (device-find, init, mix-format read,
// mixer-transparency rate guard, loopback flag, SweepEndDetector auto-stop,
// cooperative cancel); the ONLY differences are it decodes each packet into
// samplesL/samplesR via detail::decodePacketPerChannel (no mono fold) and feeds the
// end detector the per-block MAX of |L| and |R| so a hard-panned (one-channel-silent)
// sequence isn't trailing-stopped mid-run.
// ---------------------------------------------------------------------------
StereoLoopbackResult captureLoopbackStereo (const juce::String& filter, double seconds, double expectedRate,
                                            const std::atomic<bool>* cancel) {
    StereoLoopbackResult res;

    // Helper: a user-cancel result (distinct from a real failure so the GUI can show it neutrally).
    auto cancelledNow = [&] { return cancel != nullptr && cancel->load (std::memory_order_relaxed); };

    // Honour an already-set cancel BEFORE touching COM/WASAPI (also the headless-test path).
    if (cancelledNow()) {
        res.ok = false; res.cancelled = true; res.reason = "cancelled"; return res;
    }

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);

    auto fail = [&] (juce::String why) -> StereoLoopbackResult {
        if (weInited) CoUninitialize();
        res.ok = false; res.reason = std::move (why); return res;
    };

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), (void**) &en)) || en == nullptr)
        return fail ("no audio endpoint enumerator");

    // Pick the render endpoint to loop back — identical name-driven selection to captureLoopback:
    // empty -> system DEFAULT render endpoint; else prefer an EXACT FriendlyName match then a substring.
    IMMDevice* match = nullptr; juce::String matchName;
    if (filter.isEmpty()) {
        if (SUCCEEDED (en->GetDefaultAudioEndpoint (eRender, eConsole, &match)) && match != nullptr) {
            IPropertyStore* ps = nullptr; match->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm);
            if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
            matchName = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            PropVariantClear (&nm); if (ps) ps->Release();
        }
    } else {
        IMMDeviceCollection* coll = nullptr;
        en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll);
        UINT count = 0; if (coll) coll->GetCount (&count);
        IMMDevice* sub = nullptr; juce::String subName;   // best substring (contains) fallback
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr; coll->Item (i, &dev);
            IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm);
            if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
            juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            PropVariantClear (&nm); if (ps) ps->Release();
            if      (match == nullptr && name.equalsIgnoreCase   (filter)) match = dev, matchName = name;  // exact wins
            else if (sub   == nullptr && name.containsIgnoreCase (filter)) sub   = dev, subName   = name;  // contains fallback
            else                                                          dev->Release();
        }
        if (coll) coll->Release();
        if (match == nullptr && sub != nullptr) { match = sub; matchName = subName; }   // no exact -> the contains match
        else if (sub != nullptr)                  sub->Release();                       // had exact -> drop the spare
    }
    if (match == nullptr) { en->Release(); return fail (filter.isEmpty()
        ? juce::String ("no default render endpoint found")
        : ("no render endpoint matched \"" + filter + "\"")); }

    auto releaseAndFail = [&] (juce::String why) -> StereoLoopbackResult {
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

    // MIXER TRANSPARENCY: the endpoint's shared mix rate must equal the expected rate, else the OS
    // would resample Dirac's sweep before we capture it and we'd store a resampled reference. Reject up front.
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

    const size_t reserveFrames = (size_t) ((double) rate * juce::jmax (0.0, seconds));
    res.samplesL.reserve (reserveFrames);
    res.samplesR.reserve (reserveFrames);
    long long nonSilent = 0, totalFrames = 0;

    // END-OF-SWEEP AUTO-STOP (same contract as captureLoopback) — but driven by the COMBINED
    // per-block activity (max of |L| and |R|). Because Dirac hard-pans, one channel is silent while
    // the other sweeps; feeding a single channel would trail-stop in its quiet half and truncate the
    // L-then-R sequence. max(L,R) stays loud across the whole sequence, so only the real trailing
    // silence (both channels quiet) ends it.
    SweepEndDetector endDet;
    bool endedEarly = false;

    bool wasCancelled = false;
    const juce::int64 endMs = juce::Time::currentTimeMillis()
                            + (juce::int64) (juce::jmax (0.0, seconds) * 1000.0);
    while (juce::Time::currentTimeMillis() < endMs) {
        // Cooperative cancel: break out and fall through to the clean Stop()+Release() below.
        if (cancelledNow()) { wasCancelled = true; break; }
        UINT32 packetFrames = 0;
        if (FAILED (cap->GetNextPacketSize (&packetFrames))) break;
        if (packetFrames == 0) { juce::Thread::sleep (5); continue; }
        BYTE* data = nullptr; UINT32 got = 0; DWORD flags = 0;
        if (FAILED (cap->GetBuffer (&data, &got, &flags, nullptr, nullptr))) break;
        totalFrames += got;
        const double chunkSeconds = rate > 0 ? (double) got / (double) rate : 0.0;
        float chunkPeak = 0.0f;   // COMBINED: max of |L| and |R| over this packet
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            res.samplesL.insert (res.samplesL.end(), (size_t) got, 0.0f);   // loopback silence -> zeros
            res.samplesR.insert (res.samplesR.end(), (size_t) got, 0.0f);
        } else {
            const size_t before = res.samplesL.size();
            detail::decodePacketPerChannel (data, got, (int) channels, (int) bits, isFloat,
                                            res.samplesL, res.samplesR);
            for (size_t i = before; i < res.samplesL.size(); ++i) {
                // COMBINED activity = the larger of |L| and |R| (NOT std::max — the Windows
                // headers pulled in above define a `max` macro that would break it here).
                const float al = std::abs (res.samplesL[i]);
                const float ar = std::abs (res.samplesR[i]);
                const float a  = (al > ar) ? al : ar;
                if (a > chunkPeak) chunkPeak = a;
                if (a > 0.0001f) ++nonSilent;   // counts samples; only used as a >0 flag below
            }
        }
        cap->ReleaseBuffer (got);

        // Feed the COMBINED chunk peak to the end detector. Once it reports the sweep sequence has
        // ended, stop — we've captured the whole L-then-R run plus the trailing silence.
        if (endDet.update (chunkPeak, chunkSeconds)) { endedEarly = true; break; }
    }
    ac->Stop();
    juce::ignoreUnused (endedEarly);

    cap->Release(); CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release();
    if (weInited) CoUninitialize();

    // A user-cancel wins over any silence/empty verdict: report it neutrally, not as a failure.
    if (wasCancelled)
        { res.ok = false; res.cancelled = true; res.reason = "cancelled"; return res; }

    if (totalFrames <= 0 || nonSilent <= 0)
        { res.ok = false; res.reason = "captured silence - nothing was playing on the render endpoint"; return res; }

    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------
// Per-channel pan-check capture (eb_diag pancheck diagnostic).
// ---------------------------------------------------------------------------
namespace {

// Accumulate one packet's sum-of-squares into separate ch0 / ch1 accumulators WITHOUT
// averaging the channels. Mirrors decodePacketToMono's per-format sample addressing
// (32f / 16 / 24 / 32-int) but indexes channel c explicitly: sample index = f*ch + c. The
// caller folds these running sums into ~100 ms blocks to produce the per-channel RMS series.
struct ChannelSumSq { double l = 0.0, r = 0.0; };

void accumulatePacketPerChannel (const unsigned char* data, UINT32 frames, WORD channels,
                                 WORD bits, bool isFloat, ChannelSumSq& acc) {
    const int ch = juce::jmax (1, (int) channels);
    const int cR = (ch >= 2) ? 1 : 0;   // mono device -> R reads ch0 too (we flag mono separately)
    auto addSq = [&] (auto sampleAt) {
        for (UINT32 f = 0; f < frames; ++f) {
            const float l = sampleAt ((size_t) f * ch + 0);
            const float r = sampleAt ((size_t) f * ch + cR);
            acc.l += (double) l * (double) l;
            acc.r += (double) r * (double) r;
        }
    };
    if (isFloat && bits == 32) {
        auto* s = reinterpret_cast<const float*> (data);
        addSq ([&] (size_t i) { return s[i]; });
    } else if (bits == 16) {
        auto* s = reinterpret_cast<const short*> (data);
        addSq ([&] (size_t i) { return (float) s[i] / 32768.0f; });
    } else if (bits == 24) {
        addSq ([&] (size_t i) {
            const unsigned char* p = data + i * 3;
            int v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;            // sign-extend 24->32
            return (float) v / 8388608.0f;
        });
    } else if (bits == 32) {                              // 32-bit signed PCM
        auto* s = reinterpret_cast<const int*> (data);
        addSq ([&] (size_t i) { return (float) s[i] / 2147483648.0f; });
    }
}

// Convert an accumulated mean-square into a dBFS RMS, floored at -120 dB (matches the
// "all blocks near -120 on silence" smoke-test expectation).
inline float meanSqToDb (double sumSq, long long n) {
    if (n <= 0) return -120.0f;
    const double meanSq = sumSq / (double) n;
    if (meanSq <= 0.0) return -120.0f;
    const float db = (float) (20.0 * std::log10 (std::sqrt (meanSq)));
    return juce::jmax (-120.0f, db);
}

} // namespace

PanCheckResult captureLoopbackPanCheck (const juce::String& filter, double seconds, double expectedRate) {
    PanCheckResult res;
    juce::ignoreUnused (expectedRate);   // diagnostic: capture whatever Dirac plays, don't reject on rate

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);

    auto fail = [&] (juce::String why) -> PanCheckResult {
        if (weInited) CoUninitialize();
        res.ok = false; res.reason = std::move (why); return res;
    };

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), (void**) &en)) || en == nullptr)
        return fail ("no audio endpoint enumerator");

    // Pick the render endpoint to loop back -- identical name-driven selection to captureLoopback:
    // empty -> system DEFAULT render endpoint; else prefer an EXACT FriendlyName match then a substring.
    IMMDevice* match = nullptr; juce::String matchName;
    if (filter.isEmpty()) {
        if (SUCCEEDED (en->GetDefaultAudioEndpoint (eRender, eConsole, &match)) && match != nullptr) {
            IPropertyStore* ps = nullptr; match->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm);
            if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
            matchName = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            PropVariantClear (&nm); if (ps) ps->Release();
        }
    } else {
        IMMDeviceCollection* coll = nullptr;
        en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll);
        UINT count = 0; if (coll) coll->GetCount (&count);
        IMMDevice* sub = nullptr; juce::String subName;
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr; coll->Item (i, &dev);
            IPropertyStore* ps = nullptr; dev->OpenPropertyStore (STGM_READ, &ps);
            PROPVARIANT nm; PropVariantInit (&nm);
            if (ps) ps->GetValue (PKEY_Device_FriendlyName, &nm);
            juce::String name = (nm.vt == VT_LPWSTR && nm.pwszVal) ? juce::String (nm.pwszVal) : juce::String();
            PropVariantClear (&nm); if (ps) ps->Release();
            if      (match == nullptr && name.equalsIgnoreCase   (filter)) match = dev, matchName = name;
            else if (sub   == nullptr && name.containsIgnoreCase (filter)) sub   = dev, subName   = name;
            else                                                          dev->Release();
        }
        if (coll) coll->Release();
        if (match == nullptr && sub != nullptr) { match = sub; matchName = subName; }
        else if (sub != nullptr)                  sub->Release();
    }
    if (match == nullptr) { en->Release(); return fail (filter.isEmpty()
        ? juce::String ("no default render endpoint found")
        : ("no render endpoint matched \"" + filter + "\"")); }

    auto releaseAndFail = [&] (juce::String why) -> PanCheckResult {
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

    res.rate         = (double) rate;
    res.channels     = (int) channels;
    res.monoDevice   = (channels < 2);
    res.blockSeconds = 0.10;   // ~100 ms buckets

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

    // Per-block accumulation: fold each packet's per-channel sum-of-squares into the CURRENT block,
    // and at every block boundary (blockFrames worth) push the L and R RMS in dBFS. No auto-stop:
    // run the whole fixed `seconds` so the entire L-then-R sweep timeline (and the gaps) is captured.
    const long long blockFrames = juce::jmax<long long> (1, (long long) std::llround ((double) rate * res.blockSeconds));
    ChannelSumSq blockAcc;          // running sum-of-squares for the in-progress block
    long long    blockSampleCount = 0;   // frames accumulated into the in-progress block

    const juce::int64 endMs = juce::Time::currentTimeMillis()
                            + (juce::int64) (juce::jmax (0.0, seconds) * 1000.0);
    while (juce::Time::currentTimeMillis() < endMs) {
        UINT32 packetFrames = 0;
        if (FAILED (cap->GetNextPacketSize (&packetFrames))) break;
        if (packetFrames == 0) { juce::Thread::sleep (5); continue; }
        BYTE* data = nullptr; UINT32 got = 0; DWORD flags = 0;
        if (FAILED (cap->GetBuffer (&data, &got, &flags, nullptr, nullptr))) break;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // Loopback silence -> zeros contribute 0 to the sum-of-squares; just advance the frame count.
            blockSampleCount += (long long) got;
        } else {
            ChannelSumSq pkt;
            accumulatePacketPerChannel (data, got, channels, bits, isFloat, pkt);
            blockAcc.l += pkt.l; blockAcc.r += pkt.r;
            blockSampleCount += (long long) got;
        }
        cap->ReleaseBuffer (got);

        // Emit completed blocks. A single packet can span more than one block, so loop until the
        // in-progress block is under blockFrames again. We attribute the whole packet's energy to
        // the block it completes in (good enough at 100 ms granularity for a pan/mono verdict).
        while (blockSampleCount >= blockFrames) {
            res.lRmsDb.push_back (meanSqToDb (blockAcc.l, blockSampleCount));
            res.rRmsDb.push_back (meanSqToDb (blockAcc.r, blockSampleCount));
            blockAcc = {}; blockSampleCount = 0;
        }
    }
    ac->Stop();

    // Flush a trailing partial block so the timeline isn't truncated at the end.
    if (blockSampleCount > 0) {
        res.lRmsDb.push_back (meanSqToDb (blockAcc.l, blockSampleCount));
        res.rRmsDb.push_back (meanSqToDb (blockAcc.r, blockSampleCount));
    }

    cap->Release(); CoTaskMemFree (mix); ac->Release(); match->Release(); en->Release();
    if (weInited) CoUninitialize();

    if (res.lRmsDb.empty())
        { res.ok = false; res.reason = "captured no audio frames from the render endpoint"; return res; }

    res.ok = true;
    return res;
}

} // namespace eb

#else   // ---- non-Windows: no WASAPI loopback; pure stubs ----

namespace eb {

juce::String readDiracVersion()         { return {}; }
juce::String readDiracDeviceType()      { return {}; }
juce::String readDiracOutputDeviceName() { return {}; }

LoopbackCaptureResult captureLoopback (const juce::String&, double, double,
                                       const std::atomic<bool>* cancel) {
    LoopbackCaptureResult res;
    res.ok = false;
    // Honour a pre-set cancel token here too, so the cancelled result is reported the
    // same way on every platform (a user-cancel, not a "needs Windows" failure).
    if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) {
        res.cancelled = true; res.reason = "cancelled"; return res;
    }
    res.reason = "loopback capture is only available on Windows";
    return res;
}

StereoLoopbackResult captureLoopbackStereo (const juce::String&, double, double,
                                            const std::atomic<bool>* cancel) {
    StereoLoopbackResult res;
    res.ok = false;
    // Honour a pre-set cancel token here too, so the cancelled result is reported the
    // same way on every platform (a user-cancel, not a "needs Windows" failure).
    if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) {
        res.cancelled = true; res.reason = "cancelled"; return res;
    }
    res.reason = "not supported on this platform";
    return res;
}

PanCheckResult captureLoopbackPanCheck (const juce::String&, double, double) {
    PanCheckResult res;
    res.ok = false;
    res.reason = "loopback pan-check is only available on Windows";
    return res;
}

} // namespace eb

#endif
