#include "audio/Deconvolver.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

// Reference-Based Measurement Monitor — Task 1 implementation.
//
// All transforms run OUT OF PLACE (distinct in/out pointers). In-place
// juce::dsp::FFT::perform corrupts results in this JUCE/MSVC toolchain — see the
// out-of-place pattern in src/cal/FirDesigner.cpp, mirrored here exactly. JUCE
// 1/N-normalizes the inverse and leaves the forward unscaled; the round-trips
// below rely on that consistent convention (no extra scaling applied).

namespace eb {

namespace {

// Smallest power of two >= v (v >= 1).
int nextPow2 (int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// fftOrder such that (1 << order) == size.
int orderForSize (int size) {
    int order = 0;
    while ((1 << order) < size) ++order;
    return order;
}

// Pack n real samples (zero-padded) into an interleaved complex buffer of
// length 2*fftSize, then forward-transform OUT OF PLACE into `out`.
void forwardReal (juce::dsp::FFT& fft, int fftSize,
                  const float* src, int n,
                  std::vector<float>& out /* size 2*fftSize */) {
    std::vector<float> in ((size_t) fftSize * 2, 0.0f);
    const int m = std::min (n, fftSize);
    for (int i = 0; i < m; ++i) in[(size_t) i * 2] = src[i];   // real part; imag stays 0
    fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (in.data()),
                 reinterpret_cast<juce::dsp::Complex<float>*> (out.data()), false);
}

} // namespace

// ---------------------------------------------------------------------------
// Reference-derived banded regularization (sub-project 2). See the header for
// the scheme; every constant here is the research-validated spec value.
// ---------------------------------------------------------------------------
BandedRegularization deriveBandedRegularization (const float* power, int numBins) {
    BandedRegularization out;
    if (power == nullptr || numBins < 16) return out;

    // 1) Fractional-octave smoothing: variable-window moving average whose half-width is
    //    ~1/6 octave at every bin (h = k*(2^(1/12) - 2^(-1/12))), evaluated over a prefix sum so
    //    each bin costs O(1); two cascaded passes (~triangular kernel) tame the ripple.
    const double kHalfWidth = std::pow (2.0, 1.0 / 12.0) - std::pow (2.0, -1.0 / 12.0);
    auto smooth = [] (const std::vector<double>& src, double halfWidth) {
        const int nb = (int) src.size();
        std::vector<double> prefix ((size_t) nb + 1, 0.0);
        for (int i = 0; i < nb; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + src[(size_t) i];
        std::vector<double> dst ((size_t) nb, 0.0);
        for (int k = 0; k < nb; ++k) {
            const int h  = std::max (1, (int) std::lround ((double) k * halfWidth));
            const int lo = std::max (0, k - h);
            const int hi = std::min (nb - 1, k + h);
            dst[(size_t) k] = (prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1);
        }
        return dst;
    };
    std::vector<double> P ((size_t) numBins, 0.0);
    for (int k = 0; k < numBins; ++k) P[(size_t) k] = (double) power[(size_t) k];
    const std::vector<double> Ps = smooth (smooth (P, kHalfWidth), kHalfWidth);

    // 2) Tilt-whiten: Pw(k) = Ps(k)*k cancels the ESS 1/f power tilt (research Q2) so the in-band
    //    region is flat to within ripple and both edges get the SAME margin. DC/Nyquist excluded.
    std::vector<double> Pw ((size_t) numBins, 0.0);
    for (int k = 1; k < numBins - 1; ++k) Pw[(size_t) k] = Ps[(size_t) k] * (double) k;

    // 3) Plateau level = MEDIAN of the whitened bins within 6 dB of the whitened max (median,
    //    not max: ripple-robust).
    double maxPw = 0.0;
    for (int k = 1; k < numBins - 1; ++k) maxPw = std::max (maxPw, Pw[(size_t) k]);
    if (! (maxPw > 0.0) || ! std::isfinite (maxPw)) return out;
    std::vector<double> plateau;
    const double plateauFloor = maxPw * 0.25118864;              // -6 dB
    for (int k = 1; k < numBins - 1; ++k)
        if (Pw[(size_t) k] >= plateauFloor) plateau.push_back (Pw[(size_t) k]);
    if (plateau.empty()) return out;
    const size_t midIdx = plateau.size() / 2;
    std::nth_element (plateau.begin(), plateau.begin() + (std::ptrdiff_t) midIdx, plateau.end());
    const double refLevel = plateau[midIdx];
    if (! (refLevel > 0.0)) return out;

    // 4) Classify: in-band <=> Pw within 12 dB of the plateau median; band = the LONGEST
    //    contiguous run (mirrors measuredSweepLength's longest-run pattern).
    const double thr = refLevel * 0.063095734;                   // -12 dB
    int bestLo = -1, bestHi = -2, runLo = -1;
    for (int k = 1; k < numBins - 1; ++k) {
        if (Pw[(size_t) k] >= thr) {
            if (runLo < 0) runLo = k;
            if (k - runLo > bestHi - bestLo) { bestLo = runLo; bestHi = k; }
        } else {
            runLo = -1;
        }
    }
    constexpr int kMinRunBins = 64;                              // an impulse is not a sweep
    if (bestLo < 0 || bestHi - bestLo + 1 < kMinRunBins) return out;

    // 4b) Edge refinement on the RAW whitened spectrum. The variable-window smoothing smears the
    //     band edges OUTWARD by up to ~1/3 octave, and the whitening (*k) amplifies exactly the
    //     smeared HIGH-side skirt - measured on the synthetic spectrum: with f2 only ~1/4 octave
    //     below Nyquist the smeared run reached the rail, handing near-Nyquist noise the FULL
    //     inverse (a reopened F1 zone above f2). The smoothed run guarantees contiguity; the true
    //     edge is the OUTERMOST raw bin still above threshold inside it. Shrinking from the run
    //     ends inward can never cross an interior ripple notch, and raw edge ripple only moves
    //     the result by a few bins (conservative: a hair narrower band = a hair more suppression).
    std::vector<double> PwRaw ((size_t) numBins, 0.0);
    for (int k = 1; k < numBins - 1; ++k) PwRaw[(size_t) k] = P[(size_t) k] * (double) k;
    while (bestHi > bestLo && PwRaw[(size_t) bestHi] < thr) --bestHi;
    while (bestLo < bestHi && PwRaw[(size_t) bestLo] < thr) ++bestLo;
    if (bestHi - bestLo + 1 < kMinRunBins) return out;

    // 5) Anchor = max UN-whitened smoothed power over the run: eps scales with the reference's
    //    real power (scale-invariant; fixes the latent absolute-eps defect).
    double anchor = 0.0;
    for (int k = bestLo; k <= bestHi; ++k) anchor = std::max (anchor, Ps[(size_t) k]);
    if (! (anchor > 1.0e-12) || ! std::isfinite (anchor)) return out;   // spec 3.7 dust floor: a
    // real learn's |REF|^2 sits orders of magnitude above this even at absurdly low capture
    // levels - never derive a "band" from numerical dust (scale-invariance is for signals).

    // 6) eps in the log domain: eps_in = A*1e-6, eps_out = A*10, raised-cosine crossfade of
    //    log10(eps) over 1/2 octave (sqrt 2) each side (research Q4/Q5). Log-space interpolation
    //    because eps spans 7 decades; exponent clamped so no denormals reach the division.
    const double logIn  = std::log10 (anchor) - 6.0;
    const double logOut = std::log10 (anchor) + 1.0;
    const double kSqrt2 = std::sqrt (2.0);
    const double lnS2   = std::log (kSqrt2);
    const float  epsOut = (float) std::pow (10.0, logOut);
    out.epsilon.assign ((size_t) numBins, epsOut);               // rails (DC/Nyquist) stay eps_out
    auto raisedCos = [] (double t) { return 0.5 * (1.0 - std::cos (3.14159265358979323846 * t)); };
    for (int k = 1; k < numBins - 1; ++k) {
        double w = 0.0;                                          // weight toward eps_in
        if (k >= bestLo && k <= bestHi) {
            w = 1.0;
        } else if (k < bestLo) {
            // bestLo <= 2 leaves no integer bins inside the half-octave foot -> the crossfade
            // degenerates to a hard step: monotone, bounds-safe, unreachable for real sweeps.
            const double lo = (double) bestLo / kSqrt2;
            if ((double) k > lo) w = raisedCos (std::log ((double) k / lo) / lnS2);
        } else {
            const double hi = (double) bestHi * kSqrt2;
            if ((double) k < hi) w = raisedCos (1.0 - std::log ((double) k / (double) bestHi) / lnS2);
        }
        const double logEps = logOut + w * (logIn - logOut);
        out.epsilon[(size_t) k] = (float) std::pow (10.0, std::max (logEps, -37.0));
    }
    out.binLo = bestLo;
    out.binHi = bestHi;
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------------------
// Regularized frequency-domain deconvolution
//   IR = IFFT( FFT(resp) * conj(FFT(ref)) / (|FFT(ref)|^2 + reg) )
// Out-of-place forward and inverse. Returns the time-domain IR at the padded
// power-of-two length (>= n), main lobe near index 0.
// ---------------------------------------------------------------------------
std::vector<float> deconvolve (const float* ref, const float* resp, int n, float regularization) {
    if (n <= 0 || ref == nullptr || resp == nullptr) return {};

    const int fftSize = nextPow2 (n);
    const int order   = orderForSize (fftSize);
    juce::dsp::FFT fft (order);

    std::vector<float> Ref ((size_t) fftSize * 2, 0.0f);
    std::vector<float> Resp ((size_t) fftSize * 2, 0.0f);
    forwardReal (fft, fftSize, ref,  n, Ref);
    forwardReal (fft, fftSize, resp, n, Resp);

    // Banded eps(k) derived from the reference itself on the sentinel default (research-validated
    // Kirkeby-Nelson; see deriveBandedRegularization). A degenerate reference (no plausible band)
    // falls back to the legacy flat 1e-3 - an already-broken learn never behaves WORSE than
    // today. An explicit positive eps is the legacy escape hatch.
    const int half = fftSize / 2;
    BandedRegularization banded;
    if (regularization < 0.0f) {
        std::vector<float> P ((size_t) half + 1, 0.0f);
        for (int k = 0; k <= half; ++k) {
            const float re = Ref[(size_t) k * 2], im = Ref[(size_t) k * 2 + 1];
            P[(size_t) k] = re * re + im * im;
        }
        banded = deriveBandedRegularization (P.data(), half + 1);
    }
    const float flatEps = regularization >= 0.0f ? regularization : 1.0e-3f;

    // H = Resp * conj(Ref) / (|Ref|^2 + eps(k))
    std::vector<float> H ((size_t) fftSize * 2, 0.0f);
    for (int k = 0; k < fftSize; ++k) {
        const float rRe = Ref[(size_t) k * 2],  rIm = Ref[(size_t) k * 2 + 1];
        const float yRe = Resp[(size_t) k * 2], yIm = Resp[(size_t) k * 2 + 1];
        const int   kb  = k <= half ? k : fftSize - k;           // conjugate-symmetric mirror bin
        const float e   = banded.valid ? banded.epsilon[(size_t) kb] : flatEps;
        const float denom = std::max (rRe * rRe + rIm * rIm + e, 1.0e-20f);
        // Resp * conj(Ref) = (yRe + j yIm)(rRe - j rIm)
        const float numRe = yRe * rRe + yIm * rIm;
        const float numIm = yIm * rRe - yRe * rIm;
        H[(size_t) k * 2]     = numRe / denom;
        H[(size_t) k * 2 + 1] = numIm / denom;
    }
    if (banded.valid) { H[0] = 0.0f; H[1] = 0.0f; }              // DC carries no IR information

    std::vector<float> imp ((size_t) fftSize * 2, 0.0f);
    fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (H.data()),
                 reinterpret_cast<juce::dsp::Complex<float>*> (imp.data()), true); // inverse, OUT OF PLACE

    std::vector<float> ir ((size_t) fftSize, 0.0f);
    for (int i = 0; i < fftSize; ++i) ir[(size_t) i] = imp[(size_t) i * 2]; // real part
    return ir;
}

// ---------------------------------------------------------------------------
// FFT cross-correlation alignment.
// xcorr(resp, ref)[lag] = IFFT( FFT(resp) * conj(FFT(ref)) ). The peak lag is
// the bulk delay of resp relative to ref. Coherence here = the matched-filter
// main-lobe energy concentration (fraction of correlation energy within
// +/- kLobeHalfWidth of the peak), the noise-robust same-sweep discriminator.
// ---------------------------------------------------------------------------
// Window half-width (samples) for the cross-correlation main-lobe concentration.
// ~2.7 ms at 48 kHz. Chosen from the synthetic sweep across widths: it is wide
// enough to capture a matching sweep's autocorrelation lobe (>= 0.81 of the
// correlation energy for a clean/noisy match) yet narrow enough that a wrong
// sweep's time-smeared correlation (~0.48) and broadband noise (~0.02) stay
// below the kMainLobeMin gate. (Widening to ~192 lets a wrong-sweep creep toward
// 0.51 and erodes the margin; 128 maximizes the match/mismatch separation.)
static constexpr int kLobeHalfWidth = 128;

AlignResult crossCorrelateAlign (const float* ref, int refLen,
                                 const float* resp, int respLen) {
    AlignResult out;
    if (ref == nullptr || resp == nullptr || refLen <= 0 || respLen <= 0) return out;

    const int maxLen = std::max (refLen, respLen);
    const int fftSize = nextPow2 (maxLen);
    const int order   = orderForSize (fftSize);
    juce::dsp::FFT fft (order);

    std::vector<float> Ref ((size_t) fftSize * 2, 0.0f);
    std::vector<float> Resp ((size_t) fftSize * 2, 0.0f);
    forwardReal (fft, fftSize, ref,  refLen,  Ref);
    forwardReal (fft, fftSize, resp, respLen, Resp);

    // Cross-power spectrum: Resp * conj(Ref). The inverse FFT of this is the
    // matched-filter cross-correlation of resp against ref.
    std::vector<float> X ((size_t) fftSize * 2, 0.0f);
    for (int k = 0; k < fftSize; ++k) {
        const float rRe = Ref[(size_t) k * 2],  rIm = Ref[(size_t) k * 2 + 1];
        const float yRe = Resp[(size_t) k * 2], yIm = Resp[(size_t) k * 2 + 1];
        X[(size_t) k * 2]     = yRe * rRe + yIm * rIm;   // Re(Resp * conj(Ref))
        X[(size_t) k * 2 + 1] = yIm * rRe - yRe * rIm;   // Im
    }

    std::vector<float> corr ((size_t) fftSize * 2, 0.0f);
    fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (X.data()),
                 reinterpret_cast<juce::dsp::Complex<float>*> (corr.data()), true); // inverse, OUT OF PLACE

    // Magnitudes of the (circular) correlation; index = positive lag (resp later
    // than ref). A lag near fftSize wraps to a small negative delay.
    std::vector<float> mag ((size_t) fftSize, 0.0f);
    int peakIdx = 0;
    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < fftSize; ++i) {
        const float re = corr[(size_t) i * 2], im = corr[(size_t) i * 2 + 1];
        const float m = std::sqrt (re * re + im * im);
        mag[(size_t) i] = m;
        sumSq += (double) m * m;
        if (m > peak) { peak = m; peakIdx = i; }
    }

    // Delay: map the upper half of the circular index range to negative lags.
    int delay = peakIdx;
    if (delay > fftSize / 2) delay -= fftSize;
    out.delaySamples = delay;

    // Coherence = main-lobe energy concentration of the matched-filter
    // cross-correlation: the fraction of the total correlation energy that lands
    // within +/- kLobeHalfWidth of the peak. This is the load-bearing
    // discriminator (proven by the synthetic harness):
    //   - same sweep (clean OR noisy): the matched filter compresses the sweep
    //     into a sharp lobe, so a large fraction of the energy sits in the window
    //     (~0.66 in tests) — additive noise spreads energy elsewhere but the lobe
    //     stays dominant, so a noisy-but-matching sweep still scores high;
    //   - a DIFFERENT sweep: the two chirps follow different time-frequency
    //     trajectories, so the correlation is smeared in time -> the lobe holds
    //     much less of the energy (~0.40);
    //   - white noise: no lobe at all (~0.02).
    if (sumSq <= 0.0) {
        out.coherence = 0.0f;
    } else {
        double lobeSq = 0.0;
        for (int d = -kLobeHalfWidth; d <= kLobeHalfWidth; ++d) {
            const int j = ((peakIdx + d) % fftSize + fftSize) % fftSize; // circular
            lobeSq += (double) mag[(size_t) j] * mag[(size_t) j];
        }
        out.coherence = std::clamp ((float) (lobeSq / sumSq), 0.0f, 1.0f);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The match-gate.
//
// Both metrics are computed on the matched-filter cross-correlation of resp
// against ref (the inverse FFT of Resp*conj(Ref)) — NOT on the regularized IR.
// This is a deliberate, load-bearing choice: a noisy-but-matching sweep MUST
// pass the gate (the gate asks "is this the right sweep?", Task 2 grades the
// noise), but heavy additive noise destroys the *regularized-IR* main lobe (the
// IR's argmax wanders into the noise floor — proven in the synthetic harness:
// IR main-lobe fraction collapses to ~0.03 at 6 dB SNR, indistinguishable from
// white noise). The cross-correlation main lobe is noise-robust: the matched
// filter compresses the matching sweep into a sharp lobe whose dominance
// survives heavy noise, yet still delocalizes for a *different* sweep. So the
// regularized IR is produced by deconvolve() for Task 2's quality grading, but
// the GATE keys off the cross-correlation, which separates all three cases:
//   coherence (peak prominence) AND mainLobeConcentration (lobe energy fraction)
//   - matching sweep (clean or 6 dB SNR): both HIGH
//   - different sweep:                     concentration LOW (smeared chirp)
//   - white noise:                         both LOW (no lobe)
//
// matched = coherence >= minCoherence && mainLobeConcentration >= kMainLobeMin.
// ---------------------------------------------------------------------------
MatchVerdict referenceMatches (const float* ref, const float* resp, int n, float minCoherence) {
    MatchVerdict v;
    if (ref == nullptr || resp == nullptr || n <= 0) return v;

    const int fftSize = nextPow2 (n);
    const int order   = orderForSize (fftSize);
    juce::dsp::FFT fft (order);

    std::vector<float> Ref ((size_t) fftSize * 2, 0.0f);
    std::vector<float> Resp ((size_t) fftSize * 2, 0.0f);
    forwardReal (fft, fftSize, ref,  n, Ref);
    forwardReal (fft, fftSize, resp, n, Resp);

    std::vector<float> X ((size_t) fftSize * 2, 0.0f);
    for (int k = 0; k < fftSize; ++k) {
        const float rRe = Ref[(size_t) k * 2],  rIm = Ref[(size_t) k * 2 + 1];
        const float yRe = Resp[(size_t) k * 2], yIm = Resp[(size_t) k * 2 + 1];
        X[(size_t) k * 2]     = yRe * rRe + yIm * rIm;
        X[(size_t) k * 2 + 1] = yIm * rRe - yRe * rIm;
    }
    std::vector<float> corr ((size_t) fftSize * 2, 0.0f);
    fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (X.data()),
                 reinterpret_cast<juce::dsp::Complex<float>*> (corr.data()), true); // inverse, OUT OF PLACE

    std::vector<float> mag ((size_t) fftSize, 0.0f);
    int peakIdx = 0;
    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < fftSize; ++i) {
        const float re = corr[(size_t) i * 2], im = corr[(size_t) i * 2 + 1];
        const float m = std::sqrt (re * re + im * im);
        mag[(size_t) i] = m;
        sumSq += (double) m * m;
        if (m > peak) { peak = m; peakIdx = i; }
    }
    if (sumSq <= 0.0 || peak <= 0.0f) return v;   // all-zero input -> not matched

    // mainLobeConcentration: fraction of the cross-correlation ENERGY within
    // +/- kLobeHalfWidth of the peak (the main lobe). HIGH only when resp really
    // is this sweep (clean or noisy); LOW for a wrong sweep or noise.
    double lobeSq = 0.0;
    for (int d = -kLobeHalfWidth; d <= kLobeHalfWidth; ++d) {
        const int j = ((peakIdx + d) % fftSize + fftSize) % fftSize;   // circular
        lobeSq += (double) mag[(size_t) j] * mag[(size_t) j];
    }
    v.mainLobeConcentration = std::clamp ((float) (lobeSq / sumSq), 0.0f, 1.0f);

    // coherence: peak PROMINENCE = peak vs the RMS of the correlation outside the
    // lobe window, squashed into [0,1] as 1 - 1/ratio. A complementary check to
    // the concentration: it confirms the lobe towers over the background floor.
    double restSq = 0.0;
    int restCount = 0;
    for (int i = 0; i < fftSize; ++i) {
        int dd = std::abs (i - peakIdx);
        dd = std::min (dd, fftSize - dd);                              // circular distance
        if (dd <= kLobeHalfWidth) continue;
        restSq += (double) mag[(size_t) i] * mag[(size_t) i];
        ++restCount;
    }
    const float restRms = (restCount > 0) ? (float) std::sqrt (restSq / restCount) : 0.0f;
    if (restRms <= 0.0f) {
        v.coherence = 1.0f;
    } else {
        const float ratio = peak / restRms;
        v.coherence = std::clamp (1.0f - 1.0f / std::max (1.0f, ratio), 0.0f, 1.0f);
    }

    v.matched = (v.coherence >= minCoherence) && (v.mainLobeConcentration >= kMainLobeMin);
    return v;
}

} // namespace eb
