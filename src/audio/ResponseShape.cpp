#include "audio/ResponseShape.h"
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

} // namespace eb
