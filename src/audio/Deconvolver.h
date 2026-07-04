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
// ON-DEVICE RATIFIED (2026-06-21, Auto per-ear, real Dirac sweep). The old 0.5/0.5
// defaults were tuned on SYNTHETIC near-delta responses and the grade NEVER fired on real
// hardware. A real EARS-mic response is the sweep CONVOLVED with the earcup+coupler impulse
// response, so its cross-correlation against the clean reference is that IR — energy spread
// across the decay, not a delta. Measured populations:
//     case                         coherence   mainLobe
//     real CORRECT sweep            0.97-0.99    0.28      <- must PASS
//     real room AMBIENT             0.74         0.005     <- must FAIL
//     synthetic WRONG sweep         0.909        0.48      <- must FAIL (honesty)
//     synthetic noisy match (6dB)   0.981        0.80      <- must PASS
//     synthetic white noise         0.724        0.019     <- must FAIL
// The decisive fact: the WRONG sweep's mainLobe (0.48) is HIGHER than the real correct
// sweep's (0.28), so mainLobe cannot reject a wrong chirp — but COHERENCE cleanly separates
// the right chirp (>=0.97) from a wrong chirp (0.909) and ambient (0.74). So coherence is the
// PRIMARY discriminator (0.95, in the gap between 0.909 and 0.97), and mainLobe is a LOOSE
// floor (0.08) that just passes the spread real IR (0.28) while rejecting flat noise/ambient
// (0.005-0.019). matched = coherence>=0.95 AND mainLobe>=0.08 classifies every case correctly.
static constexpr float kMatchCoherenceMin = 0.95f;   // primary gate: right chirp (>=0.97) vs wrong (0.909)/ambient (0.74)
static constexpr float kMainLobeMin       = 0.08f;   // loose floor: passes the spread real IR (~0.28), rejects noise (~0.02)

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

// ---- Reference-derived banded regularization (sub-project 2) --------------
// Kirkeby-Nelson frequency-dependent regularization, derived from the reference's own one-sided
// power spectrum (research-validated; spec 2026-07-02-freq-dependent-regularization-design):
//   1) fractional-octave smoothing (1/6-oct half-width, two cascaded prefix-sum passes),
//   2) tilt-whitening Pw(k) = Ps(k)*k (cancels the ESS 1/f power tilt so LF and HF get EQUAL
//      classification margin - a raw threshold would spend ~30 dB of margin on the tilt),
//   3) in-band <=> whitened power within 12 dB of the plateau MEDIAN (ripple-robust), band =
//      the longest contiguous run (isolated noise bins never extend it),
//   4) eps_in = A*1e-6 (division stays the pure inverse in-band - preserves today's behavior),
//      eps_out = A*10 (out-of-band content ATTENUATED, never boosted ~1/eps like the flat reg),
//      raised-cosine crossfade of log10(eps) over 1/2 octave each side (protects the
//      negative-time THD region from transition ringing).
// Everything is in bin-index space (f proportional to k): no sample rate, no assumed sweep range
// (a 100 Hz-8 kHz reference self-derives a narrow band), scale-invariant by construction (eps
// scales with the reference power - fixes the latent absolute-eps defect).
// `power` = |REF(k)|^2 for k = 0..numBins-1 (numBins = fftSize/2 + 1). valid=false on a
// degenerate spectrum (all-zero / too few bins / run under 64 bins) - the caller falls back to
// the legacy flat eps, so a broken learn never behaves WORSE than today.
struct BandedRegularization {
    std::vector<float> epsilon;   // per-bin eps, one-sided (numBins entries); empty unless valid
    int  binLo = 0, binHi = 0;    // the derived in-band run (inclusive)
    bool valid = false;
};
[[nodiscard]] BandedRegularization deriveBandedRegularization (const float* power, int numBins);

// Sentinel for deconvolve's `regularization`: derive banded eps(k) from the reference (Task 2
// wires the default; a degenerate reference falls back to the legacy flat 1e-3).
static constexpr float kAutoRegularization = -1.0f;

// ---- Regularized frequency-domain deconvolution --------------------------
// IR = IFFT( FFT(resp) * conj(FFT(ref)) / (|FFT(ref)|^2 + eps(k)) ).
// Default (kAutoRegularization): eps(k) is the reference-derived BANDED regularization above -
// in-band the pure inverse (A*1e-6), out-of-band firm attenuation (A*10) instead of the flat
// reg's ~1000x noise boost (the SIM F1 corruption), with the DC bin zeroed. A degenerate
// reference falls back to the legacy flat 1e-3. Pass an explicit positive value for the legacy
// flat-eps behavior (characterization tests / escape hatch).
// Out-of-place FFT. `n` is the working length; the inputs are read up to n
// samples and the result is returned at the zero-padded power-of-two length
// (>= n), with the linear-impulse main lobe near index 0. Callers that want
// exactly n samples can resize the result.
std::vector<float> deconvolve (const float* ref, const float* resp, int n,
                               float regularization = kAutoRegularization);

// SP3 out-param overload (PLUMBING ONLY — identical math to the 4-arg above; that signature
// forwards here with usedBanding == nullptr). When `usedBanding` is non-null it receives a COPY of
// the reference-derived BandedRegularization the deconvolution actually used (valid only on the
// sentinel default; a degenerate reference or an explicit positive eps leaves it invalid). The SP3
// analysis pass reads usedBanding->binLo/binHi to derive the reference's analysis band in Hz
// (bin * sampleRate / fftSize) — the SAME band the SP2 regularization keyed off, so the detectors
// and the correction agree. No behavioural change to the returned IR.
std::vector<float> deconvolve (const float* ref, const float* resp, int n,
                               float regularization, BandedRegularization* usedBanding);

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
