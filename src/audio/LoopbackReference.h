#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

// Reference-Based Measurement Monitor — Task 3: the loopback reference
// (capture + validate + metadata).
//
// We acquire Dirac's CLEAN DIGITAL sweep as the measurement reference: WASAPI
// loopback-capture the render endpoint Dirac plays to (the Dirac Processor in
// Windows-Audio shared mode), then prove the capture is a clean, full,
// single-source sweep before storing it.
//
// Three layers, separated so the testable core has no platform/hardware deps:
//   1. validateReferenceCapture(...) — PURE. Length / level / single-source
//      self-test. Fully synthetic-testable (tests/test_loopbackreference.cpp).
//   2. makeReferenceMetadata(...)    — rate, length, a SHA-256 content hash, and
//      a best-effort Dirac version string.
//   3. captureLoopback(...)          — the WASAPI loopback capture (Windows-only,
//      guarded like EndpointUid/EndpointFormat; a no-op stub elsewhere). It
//      mirrors eb_diag's `loopcap` and asserts mixer transparency (mix rate == the
//      expected rate) so we never store a resampled reference. Hardware/manual —
//      verified on-device, not in the unit suite.
namespace eb {

// ---- Pure per-channel decode (the testable core) -------------------------
// detail:: holds the format-addressing primitive that backs the per-channel WASAPI
// captures. It is pure (no platform deps) so the unit suite can drive every mix
// format (32f / 16 / 24 / 32-int, mono) without a live endpoint.
namespace detail {

// Decode one packet's worth of interleaved mix-format bytes into SEPARATE per-channel
// float vectors WITHOUT downmixing: channel 0 samples are APPENDED to outL, channel 1 to
// outR (sample index = frame*channels + c, exactly like captureLoopback's mono decode and
// the pancheck per-channel accumulator). A MONO endpoint (channels < 2) duplicates its single
// channel into BOTH outL and outR so the per-ear grade path always sees two streams. This is
// the no-downmix core that keeps Dirac's hard-panned L/R sweep separation intact for grading.
void decodePacketPerChannel (const unsigned char* data, unsigned int frames, int channels,
                             int bits, bool isFloat,
                             std::vector<float>& outL, std::vector<float>& outR);

} // namespace detail

// ---- Pure validation (the testable core) ---------------------------------
struct ReferenceValidation {
    bool         ok = false;
    juce::String reason;   // empty iff ok
};

// Decide whether `samples` (n mono samples at `rate`) is a clean, full,
// single-source Dirac sweep suitable to use as the measurement reference.
// First failure wins; `reason` names the failing rule. Checks, in order:
//   1. LENGTH  — at least minSeconds of audio. With end-of-sweep AUTO-STOP the
//                captured length is now VARIABLE (the real sequence + ~3 s trailing
//                silence), so this is a SANITY FLOOR that rejects a too-short blip,
//                NOT the old fixed ~25 s "full sequence" bound — completeness is the
//                single-sweep self-test's job below. Else "capture too short …".
//   2. LEVEL   — peak within [minPeakDb, maxPeakDb] dBFS, and NO clipping run
//                (kClipRunSamples consecutive samples at/near full scale).
//   3. SINGLE-SOURCE — the clean-single-sweep self-test: referenceMatches
//                (samples, samples) must recover a compact, dominant
//                autocorrelation lobe. A second source (a tone mixed onto the
//                sweep) or noise-only delocalizes it and FAILS. This — not a fixed
//                length — is the real completeness/quality gate.
ReferenceValidation validateReferenceCapture (const float* samples, int n, double rate,
                                              double minSeconds = 8.0,
                                              double minPeakDb  = -20.0,
                                              double maxPeakDb  = -3.0);

// ---- Metadata ------------------------------------------------------------
struct ReferenceMetadata {
    juce::String diracVersion;   // best-effort, "" when unknown / not installed
    double       rate = 0.0;
    int          lengthSamples = 0;
    juce::String contentHash;    // SHA-256 hex of the raw sample bytes
};

// Populate metadata from a capture: rate + length from the arguments, a
// SHA-256 contentHash over the raw float bytes, and the Dirac version (best
// effort — see readDiracVersion()).
ReferenceMetadata makeReferenceMetadata (const float* samples, int n, double rate);

// Best-effort Dirac Live Processor version, parsed from
// %APPDATA%\Dirac\Dirac_Live_Processor\DiracLiveProcessor.settings (a version
// attribute if present). Returns "" when the file is absent/unparseable or on a
// non-Windows platform. Windows-only read, guarded.
juce::String readDiracVersion();

// READ-ONLY detection of the Dirac Processor's output deviceType from the same
// settings file (a plain XML attribute). Used to inform — not enforce — during a
// LEARN: the loopback reference can only be captured while Dirac plays through
// "Windows Audio" (WASAPI shared); in ASIO/exclusive the loopback is silent. We
// NEVER edit the file (auto-switch is deferred to v2); we only surface what we read.
// Returns the raw deviceType string (e.g. "Windows Audio", "ASIO") or "" when the
// file is absent/unparseable or on a non-Windows platform.
juce::String readDiracDeviceType();

// READ-ONLY: the output device Dirac PLAYS THE SWEEP TO -- the correct loopback target for the
// reference capture. The VB-CABLE (EARS Bridge's output) carries the RESPONSE Dirac records, not
// the sweep; the sweep plays out Dirac's own output device. Parsed from the settings file's
// DEVICESETUP audioOutputDeviceName attribute (e.g. "Realtek ... (Realtek USB Audio)"). Never
// written. Returns "" when the file is absent/unparseable or on a non-Windows platform.
juce::String readDiracOutputDeviceName();

// Pure parser (cross-platform, testable) behind readDiracOutputDeviceName(): extract the OUTPUT
// device name (audioOutputDeviceName on <DEVICESETUP>) from a settings XML document. "" if absent.
[[nodiscard]] juce::String parseDiracOutputDeviceName (const juce::String& settingsXml);

// True iff readDiracDeviceType() looks like a Windows-Audio / WASAPI shared mode
// (the only mode in which a learn capture succeeds). A pure string classifier so it
// is testable without the file; "" (unknown) returns false (we can't confirm it's
// Windows Audio, so the GUI gives the cautionary hint rather than a false all-clear).
[[nodiscard]] bool diracDeviceTypeIsWindowsAudio (const juce::String& deviceType);

// ---- End-of-sweep auto-stop (the testable core) --------------------------
// The learn capture used to run a FIXED duration (26 s). It now stops early the
// moment Dirac's sweep SEQUENCE ends, so the user doesn't wait out the slack and
// a longer-than-26 s config isn't truncated. The decision is factored out here so
// it's unit-testable without the live WASAPI loop.
//
// Two stages, in order:
//   1. ARM on real activity — require sustained signal above an absolute linear
//      floor for a short while, so we don't "end" during the QUIET BEFORE Dirac
//      starts playing.
//   2. STOP on a trailing silence — once armed, if the loopback goes quiet for a
//      TRAILING SILENCE window longer than Dirac's inter-sweep gap, the sequence is
//      done. We use ~3 s (vs the ~2 s inter-sweep gap the grading path assumes) so
//      the L-then-R sequence is captured WHOLE and we don't stop in the L/R gap.

// On-device-tunable constants (synthetic-tuned defaults; ratify on hardware).
// kArmFloorLinear: absolute linear level a frame must exceed to count as activity
//   (~-46 dBFS — well above the loopback noise floor, below a real sweep).
// kArmSeconds: cumulative time above the floor before we consider the sequence
//   "started" (so a single transient can't arm + immediately trailing-stop).
// kTrailingSilenceSeconds: quiet duration after activity that concludes the run.
//   CONSERVATIVELY longer than the ~2 s Dirac inter-sweep gap so L-then-R stays whole.
constexpr float  kArmFloorLinear         = 0.005f;  // ~-46 dBFS  (on-device-tunable)
constexpr double kArmSeconds             = 0.25;    // sustained activity to arm    (on-device-tunable)
constexpr double kTrailingSilenceSeconds = 3.0;     // > the ~2 s inter-sweep gap   (on-device-tunable)

// PURE decision: given whether sustained activity has been seen, and how long the
// signal has been quiet since the last activity, has the sweep sequence ended?
// Only ends AFTER arming (no activity yet -> never ends early; runs to the cap).
[[nodiscard]] inline bool sweepSequenceEnded (bool activitySeenSoFar,
                                              double trailingSilenceSeconds,
                                              double thresholdSeconds = kTrailingSilenceSeconds) {
    return activitySeenSoFar && trailingSilenceSeconds >= thresholdSeconds;
}

// A small state machine the capture loop drives once per decoded chunk: feed it
// the chunk's peak level and its duration in seconds. It arms after kArmSeconds of
// cumulative above-floor signal, then accumulates trailing silence; ended() is the
// pure decision above. Kept header-only so the unit suite can drive it directly.
struct SweepEndDetector {
    float  armFloor        = kArmFloorLinear;
    double armSeconds      = kArmSeconds;
    double silenceThreshold = kTrailingSilenceSeconds;

    double activeAccum   = 0.0;   // cumulative above-floor time (to arm)
    double trailingQuiet = 0.0;   // quiet time since the last above-floor chunk
    bool   armed         = false; // sustained activity has been seen

    // Drive with one chunk's peak and duration. Returns true once the sequence has
    // ended (armed AND trailing silence past the threshold) — STOP capturing.
    bool update (float chunkPeak, double chunkSeconds) {
        if (chunkPeak >= armFloor) {
            activeAccum += chunkSeconds;
            trailingQuiet = 0.0;
            if (activeAccum >= armSeconds) armed = true;
        } else {
            trailingQuiet += chunkSeconds;
        }
        return ended();
    }
    [[nodiscard]] bool ended() const {
        return sweepSequenceEnded (armed, trailingQuiet, silenceThreshold);
    }
};

// ---- The WASAPI loopback capture (Windows-only; stub elsewhere) ----------
struct LoopbackCaptureResult {
    bool               ok = false;
    juce::String       reason;          // empty iff ok
    bool               cancelled = false;  // true iff the capture was aborted via the cancel token
                                           // (a user-cancel, NOT a real failure — the GUI shows it neutrally)
    std::vector<float> samples;         // mono (downmixed) float samples
    double             rate = 0.0;      // the endpoint mix-format rate actually captured
    int                channels = 0;    // the endpoint mix-format channel count
};

// Loopback-capture the first eRender endpoint whose FriendlyName contains
// `renderDeviceNameSubstring` (case-insensitive), in WASAPI shared mode with
// AUDCLNT_STREAMFLAGS_LOOPBACK — exactly what eb_diag's `loopcap` does, promoted
// into the app. The result's samples are a mono downmix of the endpoint mix
// format. MIXER TRANSPARENCY: if the endpoint mix rate != expectedRate the capture
// is rejected (we would otherwise store a resampled reference). Must run on a
// worker/message thread (off the audio callback). Windows-only; a no-op stub on
// every other platform.
//
// END-OF-SWEEP AUTO-STOP: `seconds` is the MAXIMUM. The capture arms on sustained
// activity, then STOPS as soon as the loopback goes quiet for kTrailingSilenceSeconds
// (a SweepEndDetector drives this) — so a sequence shorter than the cap returns
// early and a longer one isn't truncated as long as it finishes within the cap.
// If no clear end is detected it stops at `seconds` as before.
//
// COOPERATIVE CANCEL: pass a non-null `cancel` and set it (from any thread — it's
// an atomic) to abort the in-flight capture promptly. The capture loop polls it
// every iteration; when set it Stops()+Releases() the WASAPI client cleanly and
// returns { ok=false, cancelled=true, reason="cancelled" } so the caller can show
// a neutral "cancelled" message rather than a capture-failed error. A null cancel
// (the default) runs the full `seconds` capture as before.
LoopbackCaptureResult captureLoopback (const juce::String& renderDeviceNameSubstring,
                                       double seconds,
                                       double expectedRate = 48000.0,
                                       const std::atomic<bool>* cancel = nullptr);

// ---- Per-channel loopback capture (no downmix; Windows-only, stub elsewhere) ----
// captureLoopback downmixes Dirac's render to MONO, which DESTROYS the ~104 dB L/R
// separation the on-device pan check proved Dirac uses (the L measurement sweep plays on
// channel 0 only, the R sweep on channel 1 only). For PER-EAR grading we must keep that
// separation: ref_L = render channel 0, ref_R = render channel 1, no averaging. This is the
// per-channel sibling of captureLoopback — IDENTICAL WASAPI setup (device-find, IMMDevice/
// IAudioClient init, mix-format read, MIXER-TRANSPARENCY rate guard, AUDCLNT_STREAMFLAGS_LOOPBACK,
// the SweepEndDetector arm + auto-stop-on-trailing-silence, the cooperative cancel) but decodes
// each packet into samplesL / samplesR via detail::decodePacketPerChannel instead of a mono fold.
//
// AUTO-STOP keys off the COMBINED activity (the per-block max of |L| and |R|): because Dirac
// hard-pans, one channel is SILENT while the other sweeps, so feeding the detector a single
// channel would trail-stop in the active channel's quiet half and TRUNCATE the L-then-R sequence.
// max(L,R) stays loud across the whole sequence so only the real trailing silence ends it.
//
// A MONO endpoint duplicates its one channel into both samplesL and samplesR. Must run off the
// audio callback. Windows-only; a no-op stub returning { ok=false } on every other platform.
struct StereoLoopbackResult {
    bool               ok = false;
    juce::String       reason;            // empty iff ok
    bool               cancelled = false; // true iff aborted via the cancel token (a user-cancel, not a failure)
    std::vector<float> samplesL, samplesR;// per-channel float samples (ch0 -> L, ch1 -> R; no downmix)
    double             rate = 0.0;        // the endpoint mix-format rate actually captured
    int                channels = 0;      // the endpoint mix-format channel count
};

StereoLoopbackResult captureLoopbackStereo (const juce::String& renderDeviceNameSubstring,
                                            double seconds,
                                            double expectedRate = 48000.0,
                                            const std::atomic<bool>* cancel = nullptr);

// ---- Per-channel pan-check capture (Windows-only; stub elsewhere) ---------
// DIAGNOSTIC ONLY (eb_diag pancheck). captureLoopback downmixes Dirac's render to MONO,
// which is correct for the single-source reference but blind to L-vs-R structure. To decide
// the per-EARCUP reference-model design we need to know empirically whether Dirac plays the
// L measurement sweep on the LEFT channel and the R sweep on the RIGHT (HARD-PANNED, so a
// per-channel ref_L/ref_R is meaningful) or sums both sweeps to MONO across both channels.
//
// This captures a FIXED `seconds` with NO end-of-sweep auto-stop (we need the whole timeline
// including the L-then-R sequence and the gaps) and returns, per ~100 ms block, the RMS in
// dBFS of channel 0 (L) and channel 1 (R) SEPARATELY — never averaged. The caller reads the
// L/R energy-over-time to classify the panning. Mirrors captureLoopback's WASAPI setup exactly
// (same device-find, IMMDevice/IAudioClient init, mix-format read, AUDCLNT_STREAMFLAGS_LOOPBACK
// loopback flag, packet loop). Must run off the audio callback. Windows-only; a no-op stub on
// every other platform.
struct PanCheckResult {
    bool               ok = false;
    juce::String       reason;            // empty iff ok
    double             rate = 0.0;        // the endpoint mix-format rate actually captured
    int                channels = 0;      // the endpoint mix-format channel count
    double             blockSeconds = 0.0;// the per-block bucket size used (e.g. 0.10 s)
    std::vector<float> lRmsDb, rRmsDb;    // per-block RMS in dBFS for ch0 (L) and ch1 (R)
                                          // (if channels < 2, R is filled with L's values)
    bool               monoDevice = false;// true iff channels < 2 (R duplicated from L)
};

// Capture `seconds` of per-channel block-RMS from the render endpoint matching
// `renderDeviceNameSubstring` (empty -> system default render endpoint, like captureLoopback).
// NO auto-stop; runs the whole fixed duration. expectedRate is informational here (we do NOT
// reject on a rate mismatch — the diagnostic wants whatever Dirac is actually playing).
PanCheckResult captureLoopbackPanCheck (const juce::String& renderDeviceNameSubstring,
                                        double seconds,
                                        double expectedRate = 48000.0);

} // namespace eb
