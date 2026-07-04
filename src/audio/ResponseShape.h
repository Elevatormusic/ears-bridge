#pragma once
#include <vector>

// SP3 - Response-shape anomaly detectors (spec 2026-07-03-response-drift-monitor-design).
// ALL functions here are PURE, DETERMINISTIC, OFFLINE (grade-worker only; never the audio
// thread) and INFO-ONLY consumers: nothing in this module gates, demotes, or invalidates a
// grade. Every threshold constant is PROVISIONAL pending the on-device campaign (#54C) - the
// kIrThresholdsRatified pattern.
//
// The shared analysis pass (spec section 3): the deconvolved IR is TIME-WINDOWED before any
// spectral analysis (-5 ms pre-peak .. +200 ms post-peak, half-Hann tapers) - an unwindowed
// 2^18 FFT keeps stationary noise at full weight; windowing is standard measurement practice
// (REW / Muller-Massarani) and load-bearing for the hum-placement decision (D5a lives on the
// pre-sweep NOISE region, not here). The analysis band is the reference's derived band pulled
// in 1/12 octave each side (insurance against the SP2 eps-crossfade feet).
namespace eb {

struct WindowedSpectrum {
    std::vector<float> power;      // FULL one-sided |X(k)|^2, k = 0..fftSize/2 (windowed IR)
    int    fftSize = 0;
    int    kLo = 0, kHi = 0;       // analysis band bins
    double fs = 48000.0;
    bool   valid = false;
};
// Window the IR around its main peak and transform. bandLo/HiHz = the REFERENCE's derived band
// in Hz (from the SP2 banding, plumbed by Task 5); pulled in 1/12 oct internally. Refuses:
// null/short input (n < 1024), a band narrower than 1/6 octave, an all-zero IR.
[[nodiscard]] WindowedSpectrum windowedBandSpectrum (const float* ir, int n, double fs,
                                                     double bandLoHz, double bandHiHz);

struct BandCurve {
    std::vector<float> freqHz;     // fixed 1/8-oct log-f grid anchored at 20 Hz
    std::vector<float> dB;         // 1/6-oct-smoothed magnitude
    bool valid = false;
};
// The storable/comparable response curve (spec 3.3): 1/6-oct prefix-sum smoothing of ws.power
// over [kLo..kHi] (window clamped INSIDE the band - no out-of-band bleed), sampled at the
// 1/8-oct grid points that fall inside the band (nearest-bin of the smoothed power).
[[nodiscard]] BandCurve computeBandCurve (const WindowedSpectrum& ws);

struct DriftReport {
    bool  valid = false;
    float maxDeltaDb = 0.0f;       // max |smoothed delta| over the common grid
    float worstHz = 0.0f;          // grid frequency of that max
    float hfShelfDb = 0.0f;        // mean delta(6-14 kHz) - mean delta(300 Hz-3 kHz): tinny signature
    float signConsistency = 0.0f;  // majority-sign fraction of significant deltas in 3-16 kHz
    bool  exceedsTolerance = false;// any |smoothed delta| beyond the shaped envelope (<= 14 kHz only)
};
// D1 (spec 4.D1). Delta on the grid intersection (>= 12 common points required), smoothed with a
// 3-point moving average (grid is 1/8 oct; the curve already carries 1/6-oct smoothing -> the
// effective delta smoothing approximates the spec's 1/3 oct). signConsistency counts only points
// with |delta| > 0.5 dB (sign noise on ~0 deltas is meaningless); fewer than 4 such points -> 0.
[[nodiscard]] DriftReport compareCurves (const BandCurve& baseline, const BandCurve& current);

// The spec's frequency-shaped provisional tolerance (research-validated envelope, sqrt2-inflated):
// +/-6 dB below 300 Hz, log-f interpolation 6->3 across 300-500 Hz, +/-3 dB to 4 kHz, +/-5 dB to
// 8 kHz, +/-8 dB to 14 kHz, effectively unlimited (report-only) above 14 kHz.
[[nodiscard]] float driftToleranceDb (float hz);

} // namespace eb
