#include "audio/ResponseShape.h"
#include "audio/Deconvolver.h"     // deriveBandedRegularization: shared -12 dB band-edge derivation (D3)
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

// OUT-OF-PLACE FFTs only (in-place juce::dsp::FFT::perform corrupts on this toolchain - the
// Deconvolver/FirDesigner pattern, mirrored).
namespace eb {
namespace {

int nextPow2R (int v) { int p = 1; while (p < v) p <<= 1; return p; }
int orderForR (int s) { int o = 0; while ((1 << o) < s) ++o; return o; }
constexpr double kPiD = 3.14159265358979323846;

} // namespace

WindowedSpectrum windowedBandSpectrum (const float* ir, int n, double fs,
                                       double bandLoHz, double bandHiHz) {
    WindowedSpectrum out;
    if (ir == nullptr || n < 1024 || fs <= 0.0) return out;
    if (! (bandHiHz > bandLoHz * std::pow (2.0, 1.0 / 6.0)) || bandLoHz <= 0.0) return out;

    // Main peak (the linear impulse sits near index 0; circular indexing handles the pre-peak).
    int peak = 0; float peakMag = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float m = std::abs (ir[i]);
        if (m > peakMag) { peakMag = m; peak = i; }
    }
    if (! (peakMag > 0.0f)) return out;

    // Window: -5 ms .. +200 ms around the peak, half-Hann tapers over 10% each end (spec 3.1).
    const int pre  = (int) std::lround (0.005 * fs);
    const int post = (int) std::lround (0.200 * fs);
    const int len  = std::min (pre + post, n);
    std::vector<float> seg ((size_t) len, 0.0f);
    for (int i = 0; i < len; ++i) {
        const int src = ((peak - pre + i) % n + n) % n;      // circular
        seg[(size_t) i] = ir[src];
    }
    // ASYMMETRIC tapers: the leading taper must end BEFORE the main arrival (segment index
    // `pre`), or the peak gets attenuated relative to later content and every echo reads
    // amplified (Task 2's comb test measured exactly this: a symmetric len/10 taper swallowed
    // the whole 5 ms pre-region and the first ~15 ms post-peak, inflating a -10 dB echo's
    // apparent depth from ~5.7 dB to 27.6 dB at tau=5 ms and INVERTING dominance at 15 ms).
    const int taperIn  = std::max (1, std::min (pre - 32, len / 10));
    const int taperOut = std::max (1, len / 10);
    for (int i = 0; i < taperIn; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPiD * (float) i / (float) taperIn));
        seg[(size_t) i] *= w;
    }
    for (int i = 0; i < taperOut; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPiD * (float) i / (float) taperOut));
        seg[(size_t) (len - 1 - i)] *= w;
    }

    const int fftSize = nextPow2R (len);
    juce::dsp::FFT fft (orderForR (fftSize));
    std::vector<float> in ((size_t) fftSize * 2, 0.0f), sp ((size_t) fftSize * 2, 0.0f);
    for (int i = 0; i < len; ++i) in[(size_t) i * 2] = seg[(size_t) i];
    fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (in.data()),
                 reinterpret_cast<juce::dsp::Complex<float>*> (sp.data()), false);

    const int half = fftSize / 2;
    out.power.assign ((size_t) half + 1, 0.0f);
    for (int k = 0; k <= half; ++k) {
        const float re = sp[(size_t) k * 2], im = sp[(size_t) k * 2 + 1];
        out.power[(size_t) k] = re * re + im * im;
    }
    // Analysis band: reference band pulled in 1/12 oct each side (spec 3.2), clamped sane.
    const double loHz = bandLoHz * std::pow (2.0, 1.0 / 12.0);
    const double hiHz = bandHiHz * std::pow (2.0, -1.0 / 12.0);
    out.kLo = std::max (1, (int) std::lround (loHz * fftSize / fs));
    out.kHi = std::min (half - 1, (int) std::lround (hiHz * fftSize / fs));
    if (out.kHi - out.kLo < 16) return out;                  // still invalid (valid stays false)
    out.fftSize = fftSize;
    out.fs = fs;
    out.valid = true;
    return out;
}

BandCurve computeBandCurve (const WindowedSpectrum& ws) {
    BandCurve out;
    if (! ws.valid) return out;

    // 1/6-oct prefix-sum smoothing over the band only (clamped inside [kLo..kHi]).
    const double kHalfWidth = std::pow (2.0, 1.0 / 12.0) - std::pow (2.0, -1.0 / 12.0);
    const int nb = ws.kHi - ws.kLo + 1;
    std::vector<double> prefix ((size_t) nb + 1, 0.0);
    for (int i = 0; i < nb; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) ws.power[(size_t) (ws.kLo + i)];
    auto smoothedAt = [&] (int k) {                          // k = absolute bin
        const int h  = std::max (1, (int) std::lround ((double) k * kHalfWidth));
        const int lo = std::max (ws.kLo, k - h) - ws.kLo;
        const int hi = std::min (ws.kHi, k + h) - ws.kLo;
        return (prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1);
    };

    // Fixed 1/8-oct grid anchored at 20 Hz, points inside the band.
    const double binHz = ws.fs / (double) ws.fftSize;
    const double fLo = ws.kLo * binHz, fHi = ws.kHi * binHz;
    for (int i = 0; ; ++i) {
        const double f = 20.0 * std::pow (2.0, (double) i / 8.0);
        if (f > fHi) break;
        if (f < fLo) continue;
        const int k = std::clamp ((int) std::lround (f / binHz), ws.kLo, ws.kHi);
        const double p = std::max (smoothedAt (k), 1e-30);
        out.freqHz.push_back ((float) f);
        out.dB.push_back ((float) (10.0 * std::log10 (p)));
    }
    out.valid = out.freqHz.size() >= 12;
    return out;
}

float driftToleranceDb (float hz) {
    if (hz < 300.0f)    return 6.0f;
    if (hz < 500.0f) {                                       // log-f interpolation 6 -> 3
        const float t = (float) (std::log2 (hz / 300.0f) / std::log2 (500.0f / 300.0f));
        return 6.0f + t * (3.0f - 6.0f);
    }
    if (hz <= 4000.0f)  return 3.0f;
    if (hz <= 8000.0f)  return 5.0f;
    if (hz <= 14000.0f) return 8.0f;
    return 1000.0f;                                          // report-only: never part of the verdict
}

DriftReport compareCurves (const BandCurve& baseline, const BandCurve& current) {
    DriftReport r;
    if (! baseline.valid || ! current.valid) return r;

    // Grid intersection (both anchored at 20 Hz -> exact float matches).
    std::vector<float> f, d;
    size_t ib = 0;
    for (size_t ic = 0; ic < current.freqHz.size(); ++ic) {
        while (ib < baseline.freqHz.size() && baseline.freqHz[ib] < current.freqHz[ic] - 0.01f) ++ib;
        if (ib >= baseline.freqHz.size()) break;
        if (std::abs (baseline.freqHz[ib] - current.freqHz[ic]) < 0.01f * current.freqHz[ic]) {
            f.push_back (current.freqHz[ic]);
            d.push_back (current.dB[ic] - baseline.dB[ib]);
        }
    }
    if (f.size() < 12) return r;

    // 3-point moving average of delta (grid 1/8 oct; curves already 1/6-oct smoothed -> the
    // effective smoothing of the delta approximates the spec's 1/3 octave).
    std::vector<float> ds (d.size(), 0.0f);
    for (size_t i = 0; i < d.size(); ++i) {
        const size_t a = i > 0 ? i - 1 : i, b = std::min (i + 1, d.size() - 1);
        ds[i] = (d[a] + d[i] + d[b]) / 3.0f;
    }

    double sumHf = 0.0, sumMid = 0.0; int nHf = 0, nMid = 0, nSig = 0, nPos = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        const float fi = f[i], di = ds[i];
        if (std::abs (di) > r.maxDeltaDb) { r.maxDeltaDb = std::abs (di); r.worstHz = fi; }
        if (fi <= 14000.0f && std::abs (di) > driftToleranceDb (fi)) r.exceedsTolerance = true;
        if (fi >= 6000.0f && fi <= 14000.0f) { sumHf  += di; ++nHf;  }
        if (fi >= 300.0f  && fi <= 3000.0f)  { sumMid += di; ++nMid; }
        if (fi >= 3000.0f && fi <= 16000.0f && std::abs (di) > 0.5f) { ++nSig; if (di > 0) ++nPos; }
    }
    if (nHf > 0 && nMid > 0) r.hfShelfDb = (float) (sumHf / nHf - sumMid / nMid);
    r.signConsistency = nSig >= 4 ? (float) std::max (nPos, nSig - nPos) / (float) nSig : 0.0f;
    r.valid = true;
    return r;
}

CombReport detectComb (const WindowedSpectrum& ws, const float* ir, int n) {
    CombReport out;
    if (! ws.valid || ir == nullptr || n <= 0) return out;

    // --- Primary: band-limited power cepstrum (spec 4.D2). The low-quefrency lifter IS the
    // de-trend; unlike a fractional-octave residual it has UNIFORM tau sensitivity (the rejected
    // ACF formulation had a tau-dependent blind band, worst at short tau).
    const int nb = ws.kHi - ws.kLo + 1;
    std::vector<double> x ((size_t) nb, 0.0);
    double mean = 0.0;
    for (int i = 0; i < nb; ++i) {
        x[(size_t) i] = 0.5 * std::log (std::max ((double) ws.power[(size_t) (ws.kLo + i)], 1e-30));
        mean += x[(size_t) i];
    }
    mean /= nb;
    double winSum = 0.0;
    for (int i = 0; i < nb; ++i) {                            // mean-subtract + Hann taper
        const double w = 0.5 * (1.0 - std::cos (2.0 * kPiD * (double) i / (double) (nb - 1)));
        x[(size_t) i] = (x[(size_t) i] - mean) * w;
        winSum += w;
    }
    const int cN = nextPow2R (nb * 2);
    juce::dsp::FFT cfft (orderForR (cN));
    std::vector<float> cin ((size_t) cN * 2, 0.0f), cout ((size_t) cN * 2, 0.0f);
    for (int i = 0; i < nb; ++i) cin[(size_t) i * 2] = (float) x[(size_t) i];
    cfft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (cin.data()),
                  reinterpret_cast<juce::dsp::Complex<float>*> (cout.data()), false);

    // Quefrency mapping: the transform runs over LINEAR-f bins spaced binHz apart, so cepstral
    // sample q corresponds to tau = q / (cN * binHz) seconds.
    const double binHz = ws.fs / (double) ws.fftSize;
    const double tauPerSample = 1.0 / ((double) cN * binHz);
    const int qLo = std::max (1, (int) std::ceil  (0.0004 / tauPerSample));   // 0.4 ms lifter
    const int qHi = std::min (cN / 2 - 1, (int) std::floor (0.025 / tauPerSample)); // 25 ms
    if (qHi > qLo + 8) {
        std::vector<double> mag ((size_t) (qHi - qLo + 1), 0.0);
        int    qPk = qLo; double mPk = 0.0;
        for (int q = qLo; q <= qHi; ++q) {
            const float re = cout[(size_t) q * 2], im = cout[(size_t) q * 2 + 1];
            const double m = std::sqrt ((double) re * re + (double) im * im);
            mag[(size_t) (q - qLo)] = m;
            if (m > mPk) { mPk = m; qPk = q; }
        }
        std::vector<double> tmp = mag;                        // MAD of the search region
        std::nth_element (tmp.begin(), tmp.begin() + (std::ptrdiff_t) (tmp.size() / 2), tmp.end());
        const double med = tmp[tmp.size() / 2];
        for (auto& v : tmp) v = std::abs (v - med);
        std::nth_element (tmp.begin(), tmp.begin() + (std::ptrdiff_t) (tmp.size() / 2), tmp.end());
        const double mad = std::max (tmp[tmp.size() / 2], 1e-12);
        const double prominence = mPk / mad;
        // DFT amplitude estimate of the log-mag cosine ripple: a ~ 2*|C| / sum(window).
        const double a = std::clamp (2.0 * mPk / std::max (winSum, 1e-9), 0.0, 0.98);
        if (prominence >= 4.0 && a >= 0.1) {                  // -20 dB echo INFO line (spec)
            out.found      = true;
            out.prominence = (float) prominence;
            out.delayMs    = (float) (qPk * tauPerSample * 1000.0);
            out.spacingHz  = (float) (1.0 / (qPk * tauPerSample));
            out.depthDb    = (float) (20.0 * std::log10 ((1.0 + a) / (1.0 - a)));
        }
    }

    // --- Corroboration / long-tau arm: IR-envelope secondary peak, 2..50 ms after the main peak
    // (REW-documented multi-sweep sync loss lands here; legitimate cup acoustics are sub-ms and
    // decay within a few ms). Envelope = moving max over +/-4 samples.
    int peak = 0; float peakMag = 0.0f;
    for (int i = 0; i < n; ++i) { const float m = std::abs (ir[i]); if (m > peakMag) { peakMag = m; peak = i; } }
    if (peakMag > 0.0f) {
        const int from = peak + (int) std::lround (0.002 * ws.fs);
        const int to   = std::min (n - 1, peak + (int) std::lround (0.050 * ws.fs));
        int   sPk = 0; float sMag = 0.0f;
        for (int i = from; i <= to; ++i) {
            float env = 0.0f;
            for (int j = std::max (0, i - 4); j <= std::min (n - 1, i + 4); ++j)
                env = std::max (env, std::abs (ir[j]));
            if (env > sMag) { sMag = env; sPk = i; }
        }
        const float echoDb = 20.0f * std::log10 (std::max (sMag, 1e-12f) / peakMag);
        if (echoDb >= -20.0f && sPk > from) {
            if (! out.found) {
                out.found = true; out.fromEnvelope = true;
                out.delayMs   = (float) ((sPk - peak) * 1000.0 / ws.fs);
                out.spacingHz = (float) (ws.fs / (double) (sPk - peak));
                out.depthDb   = 20.0f * std::log10 ((1.0f + std::pow (10.0f, echoDb / 20.0f))
                                                  / (1.0f - std::pow (10.0f, echoDb / 20.0f)));
            }
        }
    }
    return out;
}

// D3 - Band truncation (spec 4.D3). Named provisional constants (all PROVISIONAL pending #54C).
namespace {
constexpr double kHfPositionOct = 1.0 / 3.0;   // eff hi edge must be >= 1/3 oct INSIDE the ref hi
constexpr double kCliffDropDb    = 30.0;       // sustained drop >= 30 dB within 1/3 oct (>= 90 dB/oct)
constexpr double kPlateauRangeDb = 6.0;        // plateau flatness: max-min <= 6 dB over the next 1/3 oct
constexpr double kLfPositionOct  = 1.0;        // eff lo edge must be >= 1 octave above the ref lo
constexpr double kLfPivotHz      = 150.0;      // ...AND above 150 Hz (provisional pivot; seal loss below)
}

TruncationReport detectTruncation (const WindowedSpectrum& ws, double refLoHz, double refHiHz) {
    TruncationReport out;
    if (! ws.valid) return out;
    const int numBins = ws.fftSize / 2 + 1;

    // Reuse the SAME tilt-whitened -12 dB band-edge derivation as the reference banding, now on the
    // MEASUREMENT's own windowed power (spec 4.D3). An invalid derivation over a valid reference band
    // means "no measurable band" - report valid with both flags false and zero edges (Task 5 words
    // that state separately); a spurious flag there would be worse than silence.
    const auto banded = deriveBandedRegularization (ws.power.data(), numBins);
    out.valid = true;
    if (! banded.valid) return out;

    const double binHz = ws.fs / (double) ws.fftSize;
    const int kLoE = banded.binLo, kHiE = banded.binHi;
    out.effLoHz = (float) (kLoE * binHz);
    out.effHiHz = (float) (kHiE * binHz);

    // 1/6-oct-smoothed dB curve over the full one-sided spectrum (prefix-sum idiom, matched to
    // computeBandCurve's smoothedAt); the cliff/plateau test walks it beyond the effective edge.
    const double kHalfWidth = std::pow (2.0, 1.0 / 12.0) - std::pow (2.0, -1.0 / 12.0);
    std::vector<double> prefix ((size_t) numBins + 1, 0.0);
    for (int k = 0; k < numBins; ++k)
        prefix[(size_t) k + 1] = prefix[(size_t) k] + std::max ((double) ws.power[(size_t) k], 1e-30);
    auto smoothedDb = [&] (int k) {
        const int h  = std::max (1, (int) std::lround ((double) k * kHalfWidth));
        const int lo = std::max (0, k - h);
        const int hi = std::min (numBins - 1, k + h);
        const double p = (prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1);
        return 10.0 * std::log10 (std::max (p, 1e-30));
    };

    // HF flag = BOTH the position condition AND the digital-cliff criterion. A brick-wall SRC/BT
    // cliff drops >= 30 dB in <= 1/3 oct then sits on the noise floor (flat); an acoustic rolloff
    // (12-30 dB/oct) keeps falling and never plateaus - that is the whole discriminator.
    const bool hfPosition = out.effHiHz <= (float) (refHiHz * std::pow (2.0, -kHfPositionOct));
    if (hfPosition && kHiE > 0) {
        const int kDrop = (int) std::lround ((double) kHiE * std::pow (2.0, kHfPositionOct));
        const int kEnd  = (int) std::lround ((double) kHiE * std::pow (2.0, 2.0 * kHfPositionOct));
        // Need >= 1/6 octave of bins beyond the edge still below Nyquist to judge the plateau; if the
        // edge already sits near Nyquist there is no room for a cliff+plateau and we do NOT flag.
        const int kPlateauLo = std::min (kDrop, numBins - 1);
        if (kEnd <= numBins - 1 && kPlateauLo < numBins - 1
            && (double) (numBins - 1 - kEnd) >= 0.0) {
            const double drop = smoothedDb (kHiE) - smoothedDb (kPlateauLo);
            double pMin = 1e300, pMax = -1e300;
            for (int k = kPlateauLo; k <= kEnd; ++k) {
                const double db = smoothedDb (k);
                pMin = std::min (pMin, db); pMax = std::max (pMax, db);
            }
            if (drop >= kCliffDropDb && (pMax - pMin) <= kPlateauRangeDb)
                out.truncatedHi = true;
        }
    }

    // LF flag = POSITION-ONLY (spec 4.D3: slope cannot separate a 2nd-order chain HPF from broken-seal
    // rolloff). Fire only when the eff lo edge is >= 1 octave above the ref lo AND above 150 Hz.
    if (out.effLoHz >= (float) std::max (refLoHz * std::pow (2.0, kLfPositionOct), kLfPivotHz))
        out.truncatedLo = true;

    return out;
}

void extractPeakSegment (const float* ir, int n, float* dst, int dstLen) {
    for (int i = 0; i < dstLen; ++i) dst[i] = 0.0f;
    if (ir == nullptr || n <= 0 || dst == nullptr || dstLen <= 0) return;
    int peak = 0; float peakMag = 0.0f;
    for (int i = 0; i < n; ++i) { const float m = std::abs (ir[i]); if (m > peakMag) { peakMag = m; peak = i; } }
    // Center dstLen samples on the peak, circular (the pre-peak region wraps for a linear IR near 0);
    // zero-padding already applied above for n < dstLen positions that fall outside the source.
    const int start = peak - dstLen / 2;
    for (int i = 0; i < dstLen; ++i) {
        const int src = ((start + i) % n + n) % n;
        dst[i] = ir[src];
    }
}

// D4 shipping verdict (spec 4.D4). Cross-ear polarity via the sign of the normalized L-vs-R IR
// cross-correlation peak (US9560461). Energy-normalize both, scan lags +/-240, key off max |rho|.
namespace { constexpr int kPolarityMaxLag = 240; constexpr float kPolarityRhoMin = 0.4f; }

PolarityReport crossEarPolarity (const float* segL, int nL, const float* segR, int nR) {
    PolarityReport out;
    if (segL == nullptr || segR == nullptr || nL <= 0 || nR <= 0) return out;

    double eL = 0.0, eR = 0.0;
    for (int i = 0; i < nL; ++i) eL += (double) segL[i] * segL[i];
    for (int i = 0; i < nR; ++i) eR += (double) segR[i] * segR[i];
    const double norm = std::sqrt (eL * eR);
    if (! (norm > 1e-30)) return out;

    // Full normalized cross-correlation over lags +/-240; the peak's |rho| decides validity, its
    // sign decides inversion (a negative peak = the two ears measure with opposite polarity).
    float bestRho = 0.0f; double bestAbs = -1.0;
    for (int lag = -kPolarityMaxLag; lag <= kPolarityMaxLag; ++lag) {
        double acc = 0.0;
        const int lo = std::max (0, -lag), hi = std::min (nL, nR - lag);
        for (int i = lo; i < hi; ++i) acc += (double) segL[i] * segR[i + lag];
        const double rho = acc / norm;
        if (std::abs (rho) > bestAbs) { bestAbs = std::abs (rho); bestRho = (float) rho; }
    }
    out.rho      = bestRho;
    out.valid    = std::abs (bestRho) >= kPolarityRhoMin;   // below 0.4 = indeterminate, no verdict
    out.inverted = out.valid && bestRho < 0.0f;
    return out;
}

// D4 per-ear diagnostic (spec 4.D4). Conditioned absolute sign (US8842846): band-limit the IR
// (one-pole HP at 300 Hz then one-pole LP at 3 kHz, forward offline passes) so phase rotation on
// the band-limited mixed-phase IR cannot flip the verdict, then take the sign of the extreme
// onset-weighted sample. Diagnostics-only - never a shipping verdict.
namespace { constexpr double kCondHpHz = 300.0, kCondLpHz = 3000.0, kOnsetTauSec = 0.005; }

int conditionedPolaritySign (const float* ir, int n, double fs) {
    if (ir == nullptr || n <= 0 || fs <= 0.0) return 0;

    // One-pole HP at 300 Hz (y = a*(y_prev + x - x_prev)) then one-pole LP at 3 kHz, forward.
    const double dt   = 1.0 / fs;
    const double rcHp = 1.0 / (2.0 * kPiD * kCondHpHz);
    const double aHp  = rcHp / (rcHp + dt);
    std::vector<double> x ((size_t) n, 0.0);
    double yPrev = 0.0, xPrev = 0.0;
    for (int i = 0; i < n; ++i) {
        const double xin = (double) ir[i];
        const double y = aHp * (yPrev + xin - xPrev);
        x[(size_t) i] = y; yPrev = y; xPrev = xin;
    }
    const double rcLp = 1.0 / (2.0 * kPiD * kCondLpHz);
    const double aLp  = dt / (rcLp + dt);
    double lp = 0.0;
    for (int i = 0; i < n; ++i) { lp += aLp * (x[(size_t) i] - lp); x[(size_t) i] = lp; }

    double xMax = 0.0;
    for (int i = 0; i < n; ++i) xMax = std::max (xMax, std::abs (x[(size_t) i]));
    if (xMax < 1e-6) return 0;

    // Onset = first sample above 0.1*max on the filtered segment; exponential weight decaying from it.
    int i0 = 0;
    for (int i = 0; i < n; ++i) if (std::abs (x[(size_t) i]) > 0.1 * xMax) { i0 = i; break; }
    const double tau = kOnsetTauSec * fs;
    double extreme = 0.0, extAbs = -1.0;
    for (int i = i0; i < n; ++i) {
        const double w = std::exp (-(double) (i - i0) / tau);
        const double v = x[(size_t) i] * w;
        if (std::abs (v) > extAbs) { extAbs = std::abs (v); extreme = v; }
    }
    return extreme > 0.0 ? 1 : (extreme < 0.0 ? -1 : 0);
}

// D5b - Resonance spike (spec 4.D5b). Named provisional constants (PROVISIONAL pending #54C).
namespace {
constexpr double kResProminenceDb = 6.0;               // narrow spike >= 6 dB over the 1/6-oct trend
constexpr double kResMaxWidthOct  = 1.0 / 24.0;        // -3 dB width <= 1/24 oct (Q >~ 35): a resonance
constexpr double kResEdgeGuardOct = 1.0 / 12.0;        // exclude the outer 1/12 oct (edge effects)
}

SpikeReport detectResonance (const WindowedSpectrum& ws) {
    SpikeReport out;
    if (! ws.valid || ws.kHi - ws.kLo < 16) return out;

    // Residual = raw in-band dB minus its own 1/6-oct-smoothed trend (same trend the curve carries).
    const double kHalfWidth = std::pow (2.0, 1.0 / 12.0) - std::pow (2.0, -1.0 / 12.0);
    const int nb = ws.kHi - ws.kLo + 1;
    std::vector<double> rawDb ((size_t) nb, 0.0), prefix ((size_t) nb + 1, 0.0);
    for (int i = 0; i < nb; ++i) {
        const double p = std::max ((double) ws.power[(size_t) (ws.kLo + i)], 1e-30);
        rawDb[(size_t) i] = 10.0 * std::log10 (p);
        prefix[(size_t) i + 1] = prefix[(size_t) i] + p;
    }
    std::vector<double> resid ((size_t) nb, 0.0);
    for (int i = 0; i < nb; ++i) {
        const int k  = ws.kLo + i;
        const int h  = std::max (1, (int) std::lround ((double) k * kHalfWidth));
        const int lo = std::max (ws.kLo, k - h) - ws.kLo;
        const int hi = std::min (ws.kHi, k + h) - ws.kLo;
        const double trend = 10.0 * std::log10 (
            std::max ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1), 1e-30));
        resid[(size_t) i] = rawDb[(size_t) i] - trend;
    }

    // Exclude the outer 1/12 octave each side (edge effects of the variable-width smoother).
    const double binHz = ws.fs / (double) ws.fftSize;
    const int iLo = std::max (0, (int) std::lround (((double) ws.kLo * std::pow (2.0, kResEdgeGuardOct)) - ws.kLo));
    const int iHi = std::min (nb - 1, (int) std::lround (((double) ws.kHi * std::pow (2.0, -kResEdgeGuardOct)) - ws.kLo));
    if (iHi - iLo < 4) return out;

    int iPk = iLo; double resPk = -1e300;
    for (int i = iLo; i <= iHi; ++i) if (resid[(size_t) i] > resPk) { resPk = resid[(size_t) i]; iPk = i; }
    if (resPk < kResProminenceDb) return out;

    // -3 dB width of the residual peak: walk left/right until residual < peak - 3 dB; width in octaves
    // = log2(kR/kL). A high-Q resonance is <= 1/24 oct; a fractional-octave bump is far wider.
    const double thr = resPk - 3.0;
    int iL = iPk, iR = iPk;
    while (iL > iLo && resid[(size_t) (iL - 1)] >= thr) --iL;
    while (iR < iHi && resid[(size_t) (iR + 1)] >= thr) ++iR;
    const int kL = ws.kLo + iL, kR = ws.kLo + iR;
    const double widthOct = std::log2 ((double) std::max (kR, 1) / (double) std::max (kL, 1));

    if (resPk >= kResProminenceDb && widthOct <= kResMaxWidthOct) {
        out.found        = true;
        out.hz           = (float) ((ws.kLo + iPk) * binHz);
        out.prominenceDb = (float) resPk;
    }
    return out;
}

} // namespace eb
