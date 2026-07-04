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
    const int taper = std::max (1, len / 10);
    for (int i = 0; i < taper; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPiD * (float) i / (float) taper));
        seg[(size_t) i]              *= w;
        seg[(size_t) (len - 1 - i)]  *= w;
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

} // namespace eb
