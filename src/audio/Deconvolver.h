#pragma once
#include <vector>

// Reference-Based Measurement Monitor — Task 1: the deconvolver + the
// `referenceMatches` match-gate (the linchpin).
//
// Offline DSP only (run once per completed sweep, off the audio thread). All
// FFTs are performed OUT OF PLACE — in-place juce::dsp::FFT::perform corrupts
// results in this toolchain (see src/cal/FirDesigner.cpp).
namespace eb {

// ---- Match-gate cutoffs --------------------------------------------------
// PROVISIONAL — on-device ratification needed. These defaults are chosen so the
// synthetic tests cleanly separate a matching sweep (and a noisy-but-matching
// one) from a wrong sweep / white noise; the real cutoffs are tuned on hardware
// during the learn/measure campaign (carried in the plan's on-device-ratification
// list alongside the IR-quality thresholds).
static constexpr float kMatchCoherenceMin = 0.5f;   // min cross-correlation peak prominence
static constexpr float kMainLobeMin       = 0.5f;   // min fraction of cross-correlation energy in the main lobe

// ---- Cross-correlation alignment -----------------------------------------
struct AlignResult {
    int   delaySamples = 0;     // lag of the cross-correlation peak (bulk delay of resp vs ref)
    float coherence    = 0.0f;  // matched-filter main-lobe energy concentration in [0,1]
};

// FFT-based cross-correlation of resp against ref. Returns the lag of the peak
// (the bulk delay) and a coherence score in [0,1] = the fraction of the
// cross-correlation energy that lands in the main lobe (within a short window of
// the peak). High for the same sweep (clean OR noisy), low for a different sweep
// or noise. Out-of-place FFT, zero-padded to a power of two >= max(refLen, respLen).
AlignResult crossCorrelateAlign (const float* ref, int refLen,
                                 const float* resp, int respLen);

// ---- Regularized frequency-domain deconvolution --------------------------
// IR = IFFT( FFT(resp) * conj(FFT(ref)) / (|FFT(ref)|^2 + regularization) ).
// Out-of-place FFT. `n` is the working length; the inputs are read up to n
// samples and the result is returned at the zero-padded power-of-two length
// (>= n), with the linear-impulse main lobe near index 0. Callers that want
// exactly n samples can resize the result.
std::vector<float> deconvolve (const float* ref, const float* resp, int n,
                               float regularization = 1e-3f);

// ---- The match-gate ------------------------------------------------------
struct MatchVerdict {
    bool  matched               = false;
    float coherence             = 0.0f;  // cross-correlation peak prominence: peak vs background RMS (0..1)
    float mainLobeConcentration = 0.0f;  // fraction of cross-correlation energy in the main lobe (0..1)
};

// Decide whether `resp` is the acoustic response to *this* `ref` sweep (vs a
// wrong/stale reference). Both metrics are computed on the matched-filter
// cross-correlation of resp against ref (NOT the regularized IR — see the .cpp:
// heavy additive noise destroys the IR main lobe, but a noisy-but-matching sweep
// must still pass, so the gate keys off the noise-robust cross-correlation):
//   coherence             = peak prominence (peak vs the background RMS)
//   mainLobeConcentration = fraction of correlation energy in the main lobe
// A true match (clean OR noisy) concentrates energy in a compact lobe that
// towers over the floor; a wrong reference delocalizes it; noise has no lobe.
//   matched = coherence >= minCoherence && mainLobeConcentration >= kMainLobeMin
MatchVerdict referenceMatches (const float* ref, const float* resp, int n,
                               float minCoherence = kMatchCoherenceMin);

} // namespace eb
