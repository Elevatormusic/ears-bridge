#pragma once
#include <vector>

// SP3 - Response-shape anomaly detectors (spec 2026-07-03-response-drift-monitor-design).
// ALL functions here are PURE, DETERMINISTIC, OFFLINE (grade-worker only; never the audio
// thread) and INFO-ONLY consumers: nothing in this module gates, demotes, or invalidates a
// grade. Every threshold constant is PROVISIONAL pending the on-device campaign (#54C) - the
// kIrThresholdsRatified pattern.
//
// The shared analysis pass (spec section 3): the deconvolved IR is TIME-WINDOWED before any
// spectral analysis (-5 ms pre-peak .. +200 ms post-peak, half-Hann tapers (leading taper
// capped inside the 5 ms pre-region; trailing 10%)) - an unwindowed
// 2^18 FFT keeps stationary noise at full weight; windowing is standard measurement practice
// (REW / Muller-Massarani) and load-bearing for the hum-placement decision (D5a lives on the
// pre-sweep NOISE region, not here). The analysis band is the reference's derived band pulled
// in 1/12 octave each side (insurance against the SP2 eps-crossfade feet).
namespace eb {

// ---- SP3 shape-anomaly flag bits (Task 5) --------------------------------------------------------
// The packed per-ear bitmask the engine publishes (AudioEngine::publishShapeAnomalies) and the GUI
// note composer (shapeInfoNote) consume. Defined HERE — the pure module both the engine and the pure
// status ladder depend on — so there is a SINGLE source of truth (no duplicated literals). kShapeBaselineSet
// is NOT an anomaly: it records that D1's session baseline has been learned for that ear.
namespace ShapeFlag {
    inline constexpr unsigned kDrift       = 1u;     // D1 session response drift (exceedsTolerance)
    inline constexpr unsigned kComb        = 2u;     // D2 comb / echo
    inline constexpr unsigned kTruncHi     = 4u;     // D3 HF band truncation (SRC/Bluetooth cliff)
    inline constexpr unsigned kTruncLo     = 8u;     // D3 LF band truncation (seal / chain HPF)
    inline constexpr unsigned kPolarity    = 16u;    // D4 cross-ear inverted polarity
    inline constexpr unsigned kHum         = 32u;    // D5a mains hum in the pre-sweep noise
    inline constexpr unsigned kResonance   = 64u;    // D5b narrow resonance spike
    inline constexpr unsigned kSkew        = 128u;   // D6 clock-skew smeared lobe
    inline constexpr unsigned kStep        = 256u;   // D7 mid-sweep level step
    inline constexpr unsigned kBaselineSet = 512u;   // D1 baseline learned this session (NOT an anomaly)
    inline constexpr unsigned kNoBand      = 1024u;  // D3 no measurable band (valid reference, empty measurement edges)

    // Every ANOMALY bit (kBaselineSet is bookkeeping, not a finding). Adding a bit: allocate it at
    // kShapeFlagNext (below), double the sentinel, then classify the new bit here - the completeness
    // static_assert refuses to compile an unclassified bit, and the VerdictCard chip table
    // (gui/StatusLadder.h) static_asserts its chip-mask union against this, so a new detector bit
    // cannot ship silently unchipped (fail-closed, same-file discipline).
    inline constexpr unsigned kAllAnomalyMask = kDrift | kComb | kTruncHi | kTruncLo | kPolarity
                                              | kHum | kResonance | kSkew | kStep | kNoBand;

    // Next free bit - allocate new flags FROM HERE (then double this sentinel). The assert makes the
    // sentinel force the mask: a flag allocated at the sentinel without classifying it above no longer
    // compiles, which in turn forces the chip table (StatusLadder.h asserts chips against the mask).
    inline constexpr unsigned kShapeFlagNext = 2048u;
    static_assert ((kAllAnomalyMask | kBaselineSet) == kShapeFlagNext - 1u,
                   "a ShapeFlag bit exists that is neither in kAllAnomalyMask nor kBaselineSet - every bit must be classified");
}

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

struct CombReport {
    bool  found = false;
    float depthDb = 0.0f;      // peak-to-peak ripple 20*log10((1+a)/(1-a))
    float delayMs = 0.0f, spacingHz = 0.0f;
    float prominence = 0.0f;   // cepstral peak / MAD of the search region
    bool  fromEnvelope = false;// true when only the IR-envelope arm fired (tau >= 2 ms)
};
// D2 (spec 4.D2). Comb / echo detector. Primary arm = band-limited POWER CEPSTRUM over the
// analysis band: the low-quefrency lifter (0.4 ms) IS the de-trend, and unlike the REJECTED
// autocorrelation (ACF) formulation - which carried a tau-dependent blind band worst at short tau
// (the 1 ms echo it missed) - the cepstrum has UNIFORM tau sensitivity across 0.4..25 ms. A
// secondary IR-envelope arm (moving max, 2..50 ms after the main peak) corroborates and reaches
// the long-tau echoes beyond the cepstral window (sets fromEnvelope). PROVISIONAL thresholds
// pending the on-device campaign (#54C): cepstral prominence >= 4x the search-region MAD and
// ripple amplitude a >= 0.1 (== a -20 dB echo); envelope arm fires at a secondary peak >= -20 dB.
[[nodiscard]] CombReport detectComb (const WindowedSpectrum& ws, const float* ir, int n);

struct TruncationReport {
    bool  valid = false, truncatedHi = false, truncatedLo = false;
    float effLoHz = 0.0f, effHiHz = 0.0f;   // measured band edges in Hz (0 = no measurable band)
};
// D3 (spec 4.D3). Band truncation vs the reference band. Reuses the SAME tilt-whitened -12 dB
// band-edge derivation as the reference banding (deriveBandedRegularization) on the measurement's
// own windowed power, then compares effective edges to the reference. HF flag needs BOTH a
// position condition (eff hi edge >= 1/3 oct INSIDE the reference hi) AND a digital-cliff
// criterion (a >=30 dB drop within 1/3 oct followed by a flat plateau) - acoustic rolloffs keep
// falling and never plateau. LF is POSITION-ONLY (slope cannot separate a 2nd-order chain HPF from
// broken-seal rolloff): flag only when the eff lo edge sits >= 1 octave above the reference lo AND
// above the 150 Hz provisional pivot. refLoHz/refHiHz = the reference's derived band in Hz.
[[nodiscard]] TruncationReport detectTruncation (const WindowedSpectrum& ws,
                                                 double refLoHz, double refHiHz);

// D4 per-ear diagnostic (spec 4.D4). Conditioned absolute sign: band-limit (HP 300 Hz -> LP 3 kHz,
// forward one-pole passes), then take the sign of the extreme onset-weighted sample. Raw main-peak
// sign is UNRELIABLE for band-limited mixed-phase IRs (phase rotation flips it - the cal FIR's
// phase sits in the path), so this is diagnostics-only, never a verdict. Returns +1 / -1 / 0.
[[nodiscard]] int conditionedPolaritySign (const float* ir, int n, double fs);

struct PolarityReport { bool valid = false, inverted = false; float rho = 0.0f; };
// D4 shipping verdict (spec 4.D4): cross-ear consistency via the sign of the L-vs-R IR
// cross-correlation peak (US9560461). Energy-normalized full cross-correlation over lags +/-240;
// valid only when the peak |rho| >= 0.4 (below: indeterminate, no verdict); inverted when that
// peak is negative. Run only when both ears graded + matched in the same session (Task 5).
[[nodiscard]] PolarityReport crossEarPolarity (const float* segL, int nL,
                                               const float* segR, int nR);

// Copy dstLen samples of the IR centered on its |ir| peak (circular indexing handles the pre-peak
// region); zero-pads when n < dstLen. Feeds the cross-ear polarity segments.
void extractPeakSegment (const float* ir, int n, float* dst, int dstLen);

struct SpikeReport { bool found = false; float hz = 0.0f, prominenceDb = 0.0f; };
// D5b (spec 4.D5b). Narrow high-Q resonance in the DE-TRENDED residual (raw in-band dB minus its
// own 1/6-oct-smoothed trend). found = prominence >= 6 dB AND the residual peak's -3 dB width <=
// 1/24 octave (Q >~ 35) - a wide bump is tonal balance, not a resonance. Nonlinear pinna rattle is
// out of scope (deferred to high-order Farina harmonics - see spec 4.D5b scope note).
[[nodiscard]] SpikeReport detectResonance (const WindowedSpectrum& ws);

struct HumReport  { bool found=false; float baseHz=0.0f, prominenceDb=0.0f; int lines=0; };
// D5a (spec 4.D5a). Mains hum in the PRE-SWEEP noise region (NOT the windowed IR - the spec's
// placement argument: the correction sweep is a level tone, hum rides the leading silence). Welch
// PSD (8192-sample Hann segments, 50% overlap, averaged power); for each mains base in {50, 60} Hz
// count harmonics n=1..8 whose peak power within +/-0.5% of n*base stands >= 10 dB over the local
// median (a +/-10 Hz window EXCLUDING the +/-1% core). found when a base carries >= 2 such lines;
// the base with more lines wins, prominence = its strongest line. Refuses len < 16384 (too short
// for the Welch average). PROVISIONAL 10 dB threshold pending the on-device campaign (#54C).
[[nodiscard]] HumReport detectMainsHum (const float* noise, int len, double fs);

// D6 (spec 4.D6). Clock-skew lobe width: the REW signature of sample-rate drift between the
// capture and reference clocks is a smeared main lobe (a sharp arrival spreads). -6 dB envelope
// width in samples: env(i) = max|ir| over [i-4..i+4] (the envelope bridges the oscillatory lobe's
// zero crossings - a raw |ir| walk would under-read smeared REAL IRs), walk out from the peak
// both ways while env >= 0.5*peak, then SUBTRACT the moving-max's fixed 2*4-sample dilation so
// the width stays calibrated to true -6 dB widths. Returns -1 on null/empty input. A clean IR
// reads a few samples; a drifted one reads several times wider. PROVISIONAL width line pending #54C.
[[nodiscard]] float mainLobeWidthSamples (const float* ir, int n);

struct StepReport { bool found=false; float stepDb=0.0f; double atSeconds=0.0; };
// D7 (spec 4.D7). Level step - the Muller-Massarani time-variance class (a mid-sweep gain jump,
// e.g. an ASRC re-lock or AGC kick). 1024-sample block RMS of capture and reference; ratio in dB
// over blocks where the reference is active (RMS > 1e-3, ~-60 dBFS); at each block boundary with
// >= 6 active blocks each side, diff = median(ratio ahead) - median(ratio behind); flag the max
// |diff| >= 2.0 dB (a slow ramp spreads the diff under threshold). atSeconds = boundary*1024/fs.
// PROVISIONAL 2 dB step threshold pending the on-device campaign (#54C).
[[nodiscard]] StepReport detectLevelStep (const float* capture, const float* reference, int n, double fs);

} // namespace eb
