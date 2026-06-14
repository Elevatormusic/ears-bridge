#include "cal/FirDesigner.h"
#include <cmath>
#include <algorithm>

namespace eb {

// log-frequency linear interpolation of the cal magnitude (dB) at an arbitrary Hz,
// holding endpoint values outside [first,last].
static double interpCalDb (const std::vector<CalPoint>& pts, double hz) {
    if (hz <= pts.front().freqHz) return pts.front().splDb;
    if (hz >= pts.back().freqHz)  return pts.back().splDb;
    // binary search for the bracketing pair
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (pts[mid].freqHz <= hz) lo = mid; else hi = mid;
    }
    const double lf = std::log10 (pts[lo].freqHz), hf = std::log10 (pts[hi].freqHz);
    const double t  = (std::log10 (hz) - lf) / (hf - lf);
    return pts[lo].splDb + t * (pts[hi].splDb - pts[lo].splDb);
}

static double interpCalPhaseDeg (const std::vector<CalPoint>& pts, double hz) {
    if (hz <= pts.front().freqHz) return pts.front().phaseDeg;
    if (hz >= pts.back().freqHz)  return pts.back().phaseDeg;
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) { size_t mid=(lo+hi)/2; if (pts[mid].freqHz<=hz) lo=mid; else hi=mid; }
    const double lf=std::log10(pts[lo].freqHz), hf=std::log10(pts[hi].freqHz);
    const double t=(std::log10(hz)-lf)/(hf-lf);
    return pts[lo].phaseDeg + t*(pts[hi].phaseDeg - pts[lo].phaseDeg);
}

std::vector<float> FirDesigner::targetMagnitudeLinear (const CalFile& cal,
                                                       const FirDesignParams& p,
                                                       int fftSize) {
    const int nBins = fftSize / 2 + 1;
    std::vector<float> mag ((size_t) nBins, 1.0f);
    for (int k = 0; k < nBins; ++k) {
        double hz = (double) k * p.sampleRate / (double) fftSize;
        if (hz < 1.0) hz = 1.0; // avoid log(0) at DC; endpoint-hold covers it
        double db = interpCalDb (cal.points, hz);
        if (p.invert) db = -db;
        if (db > p.maxBoostDb) db = p.maxBoostDb;   // clamp boosts
        mag[(size_t) k] = (float) std::pow (10.0, db / 20.0);
    }
    return mag;
}

juce::AudioBuffer<float> FirDesigner::design (const CalFile& cal, const FirDesignParams& p) {
    // Ensure the design FFT is at least as large as numTaps (guard against misconfig).
    int order = p.designFftOrder;
    while ((1 << order) < p.numTaps) ++order;
    jassert (p.numTaps <= (1 << order));
    const int fftSize = 1 << order;
    const int nBins   = fftSize / 2 + 1;
    auto mag = targetMagnitudeLinear (cal, p, fftSize); // size nBins, linear gain
    juce::ignoreUnused (nBins);

    juce::dsp::FFT fft (order);

    if (p.mode == FirMode::MinPhaseMagnitude) {
        // NOTE: juce::dsp::FFT::perform must run OUT OF PLACE (distinct input and
        // output pointers). Aliasing input==output corrupts the result in this
        // build, so each stage below writes into a fresh buffer. The transform
        // pair stays a consistently normalized round-trip (JUCE 1/N-normalizes the
        // inverse, leaves the forward unscaled) — no extra scaling is applied.

        // 1) log-magnitude over the full spectrum (mirror to negative freqs)
        std::vector<float> logmag ((size_t) fftSize * 2, 0.0f); // interleaved complex
        for (int k = 0; k < fftSize; ++k) {
            int kk = (k <= fftSize/2) ? k : fftSize - k;        // mirror
            float lm = std::log (std::max (1e-7f, mag[(size_t) kk]));
            logmag[(size_t) k * 2]     = lm;                    // real
            logmag[(size_t) k * 2 + 1] = 0.0f;                  // imag
        }
        // 2) real cepstrum = IFFT(logmag)
        std::vector<float> rceps ((size_t) fftSize * 2, 0.0f);
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (logmag.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (rceps.data()), true);
        // 3) fold to minimum-phase cepstrum
        std::vector<float> cep ((size_t) fftSize * 2, 0.0f);
        cep[0] = rceps[0]; cep[1] = 0.0f;
        for (int n = 1; n < fftSize/2; ++n) {
            cep[(size_t) n * 2]     = 2.0f * rceps[(size_t) n * 2];
            cep[(size_t) n * 2 + 1] = 0.0f;
        }
        cep[(size_t)(fftSize/2) * 2] = rceps[(size_t)(fftSize/2) * 2];
        // 4) min-phase log-spectrum = FFT(cep); then exponentiate
        std::vector<float> logspec ((size_t) fftSize * 2, 0.0f);
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (cep.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (logspec.data()), false);
        std::vector<float> spec ((size_t) fftSize * 2, 0.0f);
        for (int k = 0; k < fftSize; ++k) {
            float re = logspec[(size_t) k*2], im = logspec[(size_t) k*2+1];
            float e = std::exp (re);
            spec[(size_t) k*2]   = e * std::cos (im);
            spec[(size_t) k*2+1] = e * std::sin (im);
        }
        // 5) IFFT -> causal min-phase impulse
        std::vector<float> imp ((size_t) fftSize * 2, 0.0f);
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (spec.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (imp.data()), true);

        juce::AudioBuffer<float> ir (1, p.numTaps);
        auto* d = ir.getWritePointer (0);
        for (int i = 0; i < p.numTaps; ++i) d[i] = imp[(size_t) i * 2]; // real part
        // tail taper (last 1/8) to avoid truncation ripple
        const int fade = p.numTaps / 8;
        for (int i = 0; i < fade; ++i) {
            float w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) i / (float) fade));
            d[p.numTaps - 1 - i] *= w;
        }
        return ir;
    }

    {
        std::vector<float> spec ((size_t) fftSize * 2, 0.0f);
        for (int k = 0; k < nBins; ++k) {
            double hz = (double) k * p.sampleRate / (double) fftSize;
            double ph = juce::degreesToRadians (interpCalPhaseDeg (cal.points, std::max (1.0, hz)));
            if (p.invert) ph = -ph;                 // inverse phase too
            float m = mag[(size_t) k];
            spec[(size_t) k*2]   = m * std::cos ((float) ph);
            spec[(size_t) k*2+1] = m * std::sin ((float) ph);
            if (k > 0 && k < fftSize/2) {           // Hermitian mirror
                spec[(size_t)(fftSize-k)*2]   =  m * std::cos ((float) ph);
                spec[(size_t)(fftSize-k)*2+1] = -m * std::sin ((float) ph);
            }
        }
        // IFFT OUT-OF-PLACE — in-place perform() corrupts results in this JUCE/MSVC build (Task 5).
        std::vector<float> imp ((size_t) fftSize * 2, 0.0f);
        fft.perform (reinterpret_cast<juce::dsp::Complex<float>*> (spec.data()),
                     reinterpret_cast<juce::dsp::Complex<float>*> (imp.data()), true);
        // zero-phase IR is centered at 0 (wraps); rotate so the bulk sits inside numTaps
        juce::AudioBuffer<float> ir (1, p.numTaps);
        auto* d = ir.getWritePointer (0);
        const int half = p.numTaps / 2;
        for (int i = 0; i < p.numTaps; ++i) {
            int src = (i - half + fftSize) % fftSize;   // linear-phase center
            d[i] = imp[(size_t) src * 2];
        }
        // symmetric window
        for (int i = 0; i < p.numTaps; ++i) {
            float w = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (p.numTaps - 1)));
            d[i] *= w;
        }
        return ir;
    }
}

} // namespace eb
