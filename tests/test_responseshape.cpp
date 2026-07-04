// SP3 (response-drift monitor): the shared analysis pass + the D1 drift view.
// Spec: docs/superpowers/specs/2026-07-03-response-drift-monitor-design.md sections 3-4.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>
#include "audio/ResponseShape.h"

static constexpr double kPiR = 3.14159265358979323846;

// Band-limited synthetic IR: inverse-FFT-free construction — a windowed sinc bandpass
// (20 Hz..cut) convolved with planted taps. Good enough for curve/window tests.
static std::vector<float> bandIr (int n, double fs, double cutHz, int at = 400, float amp = 1.0f) {
    std::vector<float> ir ((size_t) n, 0.0f);
    const int half = 160;                                   // sinc kernel half-width
    const double fc = cutHz / fs;
    for (int i = -half; i <= half; ++i) {
        const double x = (double) i;
        const double s = (i == 0) ? 2.0 * fc : std::sin (2.0 * kPiR * fc * x) / (kPiR * x);
        const double w = 0.5 * (1.0 + std::cos (kPiR * x / (double) half));   // Hann
        const int j = at + i;
        if (j >= 0 && j < n) ir[(size_t) j] += amp * (float) (s * w);
    }
    return ir;
}

TEST_CASE("windowedBandSpectrum windows around the peak and reports the analysis band") {
    const int n = 1 << 16;
    auto ir = bandIr (n, 48000.0, 20000.0);
    auto ws = eb::windowedBandSpectrum (ir.data(), n, 48000.0, 20.0, 20000.0);
    REQUIRE (ws.valid);
    REQUIRE ((int) ws.power.size() == ws.fftSize / 2 + 1);
    // Analysis band pulled in 1/12 oct from [20, 20000]:
    const double loHz = 20.0 * std::pow (2.0, 1.0 / 12.0), hiHz = 20000.0 * std::pow (2.0, -1.0 / 12.0);
    CHECK (std::abs (ws.kLo - (int) std::lround (loHz * ws.fftSize / 48000.0)) <= 1);
    CHECK (std::abs (ws.kHi - (int) std::lround (hiHz * ws.fftSize / 48000.0)) <= 1);
    // In-band power is orders of magnitude above the out-of-band floor for this bandpass IR.
    double inBand = 0.0, above = 0.0; int cAbove = 0;
    for (int k = ws.kLo; k <= ws.kHi; ++k) inBand = std::max (inBand, (double) ws.power[(size_t) k]);
    for (int k = ws.fftSize / 2 - 200; k <= ws.fftSize / 2; ++k) { above += ws.power[(size_t) k]; ++cAbove; }
    CHECK (inBand > 1e4 * (above / std::max (1, cAbove)));
    // Guards: degenerate inputs refuse.
    CHECK_FALSE (eb::windowedBandSpectrum (nullptr, n, 48000.0, 20.0, 20000.0).valid);
    CHECK_FALSE (eb::windowedBandSpectrum (ir.data(), 512, 48000.0, 20.0, 20000.0).valid);
    CHECK_FALSE (eb::windowedBandSpectrum (ir.data(), n, 48000.0, 5000.0, 5100.0).valid); // < 1/6 oct
    std::vector<float> zeros ((size_t) n, 0.0f);
    CHECK_FALSE (eb::windowedBandSpectrum (zeros.data(), n, 48000.0, 20.0, 20000.0).valid);
}

TEST_CASE("computeBandCurve resamples to the 1/8-oct grid; flat-band IR yields a flat-ish curve") {
    const int n = 1 << 16;
    auto ir = bandIr (n, 48000.0, 20000.0);
    auto ws = eb::windowedBandSpectrum (ir.data(), n, 48000.0, 20.0, 20000.0);
    REQUIRE (ws.valid);
    auto c = eb::computeBandCurve (ws);
    REQUIRE (c.valid);
    REQUIRE (c.freqHz.size() == c.dB.size());
    CHECK (c.freqHz.size() >= 60);                          // ~80 pts over ~9.7 octaves at 1/8 oct
    // Grid is anchored at 20 Hz: every f = 20 * 2^(i/8) for integer i.
    for (float f : c.freqHz) {
        const double i = std::log2 ((double) f / 20.0) * 8.0;
        CHECK (std::abs (i - std::lround (i)) < 1e-3);
    }
    // A windowed-sinc bandpass is flat mid-band: 200 Hz..8 kHz within a few dB of its median.
    std::vector<float> mid;
    for (size_t i = 0; i < c.freqHz.size(); ++i)
        if (c.freqHz[i] > 200.0f && c.freqHz[i] < 8000.0f) mid.push_back (c.dB[i]);
    REQUIRE (mid.size() > 10);
    float lo = mid[0], hi = mid[0];
    for (float v : mid) { lo = std::min (lo, v); hi = std::max (hi, v); }
    CHECK (hi - lo < 6.0f);
}

TEST_CASE("compareCurves: three drift statistics behave on constructed deltas") {
    // Build two curves on the same grid directly (the struct is open) - baseline flat 0 dB,
    // current = an HF shelf: 0 dB below 3 kHz, then -8 dB above 6 kHz (linear ramp between).
    eb::BandCurve base, cur;
    for (int i = 0; ; ++i) {
        const float f = 20.0f * std::pow (2.0f, (float) i / 8.0f);
        if (f > 19000.0f) break;
        base.freqHz.push_back (f); base.dB.push_back (0.0f);
        float d = 0.0f;
        if (f >= 6000.0f) d = -8.0f;
        else if (f > 3000.0f) d = -8.0f * (std::log2 (f / 3000.0f) / std::log2 (2.0f));
        cur.freqHz.push_back (f); cur.dB.push_back (d);
    }
    base.valid = cur.valid = true;
    auto r = eb::compareCurves (base, cur);
    REQUIRE (r.valid);
    CHECK (r.maxDeltaDb > 6.0f);                            // the -8 dB shelf (1/3-oct smoothing dents it)
    CHECK (r.worstHz > 5000.0f);
    CHECK (r.hfShelfDb < -5.0f);                            // mean(6-14k) - mean(300-3k) ~ -8
    CHECK (r.signConsistency > 0.85f);                      // systematic: one sign everywhere it matters
    CHECK (r.exceedsTolerance);                             // -8 dB at 6-8 kHz vs the +/-5 band
    // Ripple case: +/-2 dB alternating above 3 kHz -> low consistency, inside tolerance.
    eb::BandCurve rip = base;
    for (size_t i = 0; i < rip.freqHz.size(); ++i)
        if (rip.freqHz[i] > 3000.0f) rip.dB[i] = ((i % 2) ? 2.0f : -2.0f);
    auto r2 = eb::compareCurves (base, rip);
    REQUIRE (r2.valid);
    CHECK (r2.signConsistency < 0.75f);
    CHECK_FALSE (r2.exceedsTolerance);                      // 2 dB ripple (further dented by smoothing)
    // Identical curves: null result.
    auto r3 = eb::compareCurves (base, base);
    REQUIRE (r3.valid);
    CHECK (r3.maxDeltaDb < 0.01f);
    CHECK_FALSE (r3.exceedsTolerance);
    // Mismatched grids refuse.
    eb::BandCurve off = base; off.freqHz.erase (off.freqHz.begin()); off.dB.erase (off.dB.begin());
    CHECK (eb::compareCurves (base, off).valid);            // intersection still >= 12 points -> valid
    eb::BandCurve tiny; tiny.valid = true;
    CHECK_FALSE (eb::compareCurves (base, tiny).valid);
}

TEST_CASE("driftToleranceDb is the spec envelope") {
    CHECK (eb::driftToleranceDb (100.0f)   == 6.0f);
    CHECK (eb::driftToleranceDb (1000.0f)  == 3.0f);
    CHECK (eb::driftToleranceDb (6000.0f)  == 5.0f);
    CHECK (eb::driftToleranceDb (10000.0f) == 8.0f);
    CHECK (eb::driftToleranceDb (16000.0f) > 100.0f);       // report-only above 14 kHz
    const float mid = eb::driftToleranceDb (400.0f);        // 300..500: log-f interpolation 6 -> 3
    CHECK (mid < 6.0f); CHECK (mid > 3.0f);
}
