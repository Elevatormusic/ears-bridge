#pragma once
#include <juce_core/juce_core.h>
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

// ---- Pure validation (the testable core) ---------------------------------
struct ReferenceValidation {
    bool         ok = false;
    juce::String reason;   // empty iff ok
};

// Decide whether `samples` (n mono samples at `rate`) is a clean, full,
// single-source Dirac sweep suitable to use as the measurement reference.
// First failure wins; `reason` names the failing rule. Checks, in order:
//   1. LENGTH  — at least minSeconds of audio (the L-then-R sequence is ~25 s),
//                else "capture too short …".
//   2. LEVEL   — peak within [minPeakDb, maxPeakDb] dBFS, and NO clipping run
//                (kClipRunSamples consecutive samples at/near full scale).
//   3. SINGLE-SOURCE — the clean-single-sweep self-test: referenceMatches
//                (samples, samples) must recover a compact, dominant
//                autocorrelation lobe. A second source (a tone mixed onto the
//                sweep) or noise-only delocalizes it and FAILS.
ReferenceValidation validateReferenceCapture (const float* samples, int n, double rate,
                                              double minSeconds = 25.0,
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

// ---- The WASAPI loopback capture (Windows-only; stub elsewhere) ----------
struct LoopbackCaptureResult {
    bool               ok = false;
    juce::String       reason;          // empty iff ok
    std::vector<float> samples;         // mono (downmixed) float samples
    double             rate = 0.0;      // the endpoint mix-format rate actually captured
    int                channels = 0;    // the endpoint mix-format channel count
};

// Loopback-capture the first eRender endpoint whose FriendlyName contains
// `renderDeviceNameSubstring` (case-insensitive) for `seconds`, in WASAPI shared
// mode with AUDCLNT_STREAMFLAGS_LOOPBACK — exactly what eb_diag's `loopcap` does,
// promoted into the app. The result's samples are a mono downmix of the endpoint
// mix format. MIXER TRANSPARENCY: if the endpoint mix rate != expectedRate the
// capture is rejected (we would otherwise store a resampled reference). Must run
// on a worker/message thread (off the audio callback). Windows-only; a no-op
// stub on every other platform.
LoopbackCaptureResult captureLoopback (const juce::String& renderDeviceNameSubstring,
                                       double seconds,
                                       double expectedRate = 48000.0);

} // namespace eb
