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

TEST_CASE("detectComb finds echoes across the tau window (cepstrum), incl. the 1 ms ACF blind spot") {
    const int n = 1 << 16; const double fs = 48000.0;
    auto mk = [&] (double tauMs, float a) {
        auto ir = bandIr (n, fs, 20000.0);                   // main arrival at 400
        const int d = (int) std::lround (tauMs * 1e-3 * fs);
        auto echo = bandIr (n, fs, 20000.0, 400 + d, a);     // band-limited delayed copy
        for (int i = 0; i < n; ++i) ir[(size_t) i] += echo[(size_t) i];
        return ir;
    };
    for (const double tauMs : { 1.0, 5.0, 15.0 }) {          // 1 ms = the rejected-ACF blind spot
        auto ir = mk (tauMs, 0.316f);                        // -10 dB echo
        auto ws = eb::windowedBandSpectrum (ir.data(), n, fs, 20.0, 20000.0);
        REQUIRE (ws.valid);
        auto c = eb::detectComb (ws, ir.data(), n);
        INFO ("tau=" << tauMs << " found=" << c.found << " delay=" << c.delayMs
              << " depth=" << c.depthDb << " prom=" << c.prominence);
        CHECK (c.found);
        CHECK (std::abs (c.delayMs - tauMs) < 0.2 + 0.05 * tauMs);
        CHECK (c.depthDb > 2.5f);                            // a=0.316 -> pp ~5.7 dB; allow estimator slack
        CHECK (c.depthDb < 12.0f);
    }
}

TEST_CASE("detectComb stays silent on clean, sub-lifter, and below-threshold cases") {
    const int n = 1 << 16; const double fs = 48000.0;
    auto clean = bandIr (n, fs, 20000.0);
    auto wsC = eb::windowedBandSpectrum (clean.data(), n, fs, 20.0, 20000.0);
    REQUIRE (wsC.valid);
    CHECK_FALSE (eb::detectComb (wsC, clean.data(), n).found);
    // 0.2 ms is a legitimate intra-cup reflection: below the 0.4 ms lifter -> silent.
    auto cup = clean;
    { auto e = bandIr (n, fs, 20000.0, 400 + (int) std::lround (0.0002 * fs), 0.4f);
      for (int i = 0; i < n; ++i) cup[(size_t) i] += e[(size_t) i]; }
    auto wsCup = eb::windowedBandSpectrum (cup.data(), n, fs, 20.0, 20000.0);
    REQUIRE (wsCup.valid);
    CHECK_FALSE (eb::detectComb (wsCup, cup.data(), n).found);
    // -32 dB echo: well under the -20 dB INFO line -> silent.
    auto faint = clean;
    { auto e = bandIr (n, fs, 20000.0, 400 + (int) std::lround (0.005 * fs), 0.025f);
      for (int i = 0; i < n; ++i) faint[(size_t) i] += e[(size_t) i]; }
    auto wsF = eb::windowedBandSpectrum (faint.data(), n, fs, 20.0, 20000.0);
    REQUIRE (wsF.valid);
    CHECK_FALSE (eb::detectComb (wsF, faint.data(), n).found);
}

TEST_CASE("detectComb envelope arm catches a 35 ms echo beyond the cepstral window") {
    const int n = 1 << 17; const double fs = 48000.0;
    auto ir = bandIr (n, fs, 20000.0);
    const int d = (int) std::lround (0.035 * fs);
    auto e = bandIr (n, fs, 20000.0, 400 + d, 0.25f);        // -12 dB echo at 35 ms
    for (int i = 0; i < n; ++i) ir[(size_t) i] += e[(size_t) i];
    auto ws = eb::windowedBandSpectrum (ir.data(), n, fs, 20.0, 20000.0);
    REQUIRE (ws.valid);
    auto c = eb::detectComb (ws, ir.data(), n);
    CHECK (c.found);
    CHECK (c.fromEnvelope);
    CHECK (std::abs (c.delayMs - 35.0) < 2.0);
}

// SP3 Task 3: D3 truncation + D4 polarity + D5b resonance spike.
static eb::WindowedSpectrum synthSpectrum (int fftSize, double fs, double loHz, double hiHz,
                                           double cliffHz = 0.0, double rolloffDbOct = 0.0,
                                           double floorRel = 1e-12) {
    eb::WindowedSpectrum ws;
    ws.fftSize = fftSize; ws.fs = fs;
    const int half = fftSize / 2;
    ws.power.assign ((size_t) half + 1, (float) floorRel);
    const double binHz = fs / fftSize;
    const int kLo = std::max (1, (int) (loHz / binHz)), kHi = std::min (half - 1, (int) (hiHz / binHz));
    for (int k = kLo; k <= kHi; ++k) {
        double p = (double) kLo / k;                          // the ESS 1/f tilt
        if (cliffHz > 0.0 && k * binHz > cliffHz) p = floorRel;               // digital cliff
        if (rolloffDbOct > 0.0 && k * binHz > cliffHz && cliffHz > 0.0) { /* unused combo */ }
        ws.power[(size_t) k] = (float) p;
    }
    if (rolloffDbOct > 0.0 && cliffHz > 0.0) {                // acoustic rolloff variant: replace
        for (int k = kLo; k <= kHi; ++k) {
            const double f = k * binHz;
            double p = (double) kLo / k;
            if (f > cliffHz) p *= std::pow (10.0, -rolloffDbOct * std::log2 (f / cliffHz) / 10.0);
            ws.power[(size_t) k] = (float) std::max (p, floorRel);
        }
    }
    ws.kLo = std::max (1, (int) std::lround (loHz * std::pow (2.0, 1.0/12.0) / binHz));
    ws.kHi = std::min (half - 1, (int) std::lround (hiHz * std::pow (2.0, -1.0/12.0) / binHz));
    ws.valid = true;
    return ws;
}

TEST_CASE("detectTruncation: digital cliff fires; acoustic rolloff and clean band do not") {
    const double fs = 48000.0; const int fftSize = 32768;
    auto cliff = synthSpectrum (fftSize, fs, 20.0, 20000.0, 12000.0);          // brick wall at 12k
    auto r1 = eb::detectTruncation (cliff, 20.0, 20000.0);
    REQUIRE (r1.valid);
    CHECK (r1.truncatedHi);
    CHECK (r1.effHiHz > 12000.0 / 1.3); CHECK (r1.effHiHz < 12000.0 * 1.3);
    auto roll = synthSpectrum (fftSize, fs, 20.0, 20000.0, 8000.0, 24.0);      // 24 dB/oct from 8k
    auto r2 = eb::detectTruncation (roll, 20.0, 20000.0);
    REQUIRE (r2.valid);
    CHECK_FALSE (r2.truncatedHi);                             // steep-but-falling acoustics: no plateau
    auto clean = synthSpectrum (fftSize, fs, 20.0, 20000.0);
    CHECK_FALSE (eb::detectTruncation (clean, 20.0, 20000.0).truncatedHi);
    // LF: content starts at 400 Hz vs a 20 Hz reference -> position-only flag (>= 1 oct AND > 150).
    auto hp = synthSpectrum (fftSize, fs, 400.0, 20000.0);
    auto r3 = eb::detectTruncation (hp, 20.0, 20000.0);
    CHECK (r3.truncatedLo);
    // 60 Hz start (seal-loss territory, below the 150 Hz pivot): NOT flagged.
    auto seal = synthSpectrum (fftSize, fs, 60.0, 20000.0);
    CHECK_FALSE (eb::detectTruncation (seal, 20.0, 20000.0).truncatedLo);
}

TEST_CASE("polarity: cross-ear inversion is caught; indeterminate stays silent") {
    const int n = 1 << 15; const double fs = 48000.0;
    auto irL = bandIr (n, fs, 18000.0);
    auto irR = irL;                                           // matched drivers
    std::vector<float> segL (2048, 0.0f), segR (2048, 0.0f);
    eb::extractPeakSegment (irL.data(), n, segL.data(), 2048);
    eb::extractPeakSegment (irR.data(), n, segR.data(), 2048);
    auto ok = eb::crossEarPolarity (segL.data(), 2048, segR.data(), 2048);
    REQUIRE (ok.valid);
    CHECK_FALSE (ok.inverted);
    CHECK (ok.rho > 0.9f);
    for (auto& v : irR) v = -v;                               // wiring/cal inversion
    eb::extractPeakSegment (irR.data(), n, segR.data(), 2048);
    auto bad = eb::crossEarPolarity (segL.data(), 2048, segR.data(), 2048);
    REQUIRE (bad.valid);
    CHECK (bad.inverted);
    // Uncorrelated noise: indeterminate -> no verdict (|rho| < 0.4).
    std::vector<float> nz (2048, 0.0f);
    unsigned s = 7u; for (auto& v : nz) { s = s * 1664525u + 1013904223u; v = ((float)(s >> 9) / (float)(1u << 23)) - 0.5f; }
    CHECK_FALSE (eb::crossEarPolarity (segL.data(), 2048, nz.data(), 2048).valid);
    // Conditioned per-ear sign (diagnostic): flips with the IR.
    const int sPos = eb::conditionedPolaritySign (irL.data(), n, fs);
    std::vector<float> neg = irL; for (auto& v : neg) v = -v;
    CHECK (sPos != 0);
    CHECK (eb::conditionedPolaritySign (neg.data(), n, fs) == -sPos);
}

TEST_CASE("detectResonance: narrow high-Q spike fires; wide bump does not") {
    const double fs = 48000.0; const int fftSize = 32768;
    auto base = synthSpectrum (fftSize, fs, 20.0, 20000.0);
    // Narrow spike at ~2.3 kHz: +10 dB over ~1/40 octave TOTAL. Plan-bug fix (post-restart
    // adoption): the original expression kc*(2^(1/80)-2^(-1/80)) is the bin span of a FULL
    // 1/40-oct interval, but it was used as the HALF-width - planting a ~1/20-oct plateau that
    // the detector CORRECTLY rejected against the spec's <= 1/24-oct width gate. Halve it so
    // the planted spike is genuinely inside the gate; the detector was right all along.
    auto spiky = base;
    { const double binHz = fs / fftSize; const int kc = (int) (2300.0 / binHz);
      const int w = std::max (1, (int) (kc * (std::pow (2.0, 1.0/80.0) - std::pow (2.0, -1.0/80.0)) / 2.0));
      for (int k = kc - w; k <= kc + w; ++k) spiky.power[(size_t) k] *= 10.0f; }
    auto r = eb::detectResonance (spiky);
    CHECK (r.found);
    CHECK (r.hz > 2000.0f); CHECK (r.hz < 2700.0f);
    CHECK (r.prominenceDb > 6.0f);
    // A 1/3-octave-wide bump is tonal balance, not a resonance.
    auto bump = base;
    { const double binHz = fs / fftSize; const int kc = (int) (2300.0 / binHz);
      const int w = (int) (kc * (std::pow (2.0, 1.0/6.0) - std::pow (2.0, -1.0/6.0)));
      for (int k = kc - w; k <= kc + w; ++k) bump.power[(size_t) k] *= 10.0f; }
    CHECK_FALSE (eb::detectResonance (bump).found);
    CHECK_FALSE (eb::detectResonance (base).found);
}
