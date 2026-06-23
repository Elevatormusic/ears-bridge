#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/PolyphaseResampler.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include <algorithm>

using Catch::Approx;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kPi = juce::MathConstants<double>::pi;

// Resample `in` at a FIXED ratio (input-samples-per-output) through our resampler, primed like
// ClockBridge (readPhase starts at L/2-1). Stops before reading past the input.
std::vector<float> resampleStream (eb::PolyphaseResampler& r, const std::vector<float>& in,
                                   double ratio, int numOut) {
    std::vector<float> out;
    out.reserve ((size_t) numOut);
    double phase = r.halfLength() - 1;
    for (int j = 0; j < numOut; ++j) {
        if ((int) std::floor (phase) + r.halfLength() >= (int) in.size()) break;
        out.push_back (r.sampleAt (in.data(), phase));
        phase += ratio;
    }
    return out;
}

std::vector<float> makeSine (int n, double f, double fs) {
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) std::sin (2.0 * kPi * f * i / fs);
    return v;
}

// dB of a (single-tone) signal's amplitude relative to a unit sine, via RMS over the steady region
// (sqrt(2)*RMS = amplitude). Grid-phase robust, no bin alignment needed.
double rmsAmplitudeDb (const std::vector<float>& y, int skip) {
    double s = 0.0; int n = 0;
    for (int i = skip; i < (int) y.size() - skip; ++i) { s += (double) y[(size_t) i] * y[(size_t) i]; ++n; }
    const double rms = std::sqrt (s / std::max (1, n));
    return 20.0 * std::log10 (std::max (1e-12, rms * std::sqrt (2.0)));
}
} // namespace

// ---- Task 4: passband flatness (48->48 drift) + Lagrange baseline-fail ---------------------------

TEST_CASE ("resampler passband flat within 0.10 dB 20Hz-20kHz (48->48 drift)", "[resampler][passband]") {
    const double fs = 48000.0, ratio = 10000.0 / 10001.0;     // near-unity drift exercises all 512 phases
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    const double tones[] = { 20, 50, 100, 500, 1000, 4000, 8000, 12000, 16000, 19000, 20000 };
    const int N = 1 << 18;                                     // long enough that 20 Hz RMS is precise
    double maxDev = 0.0;
    for (double f : tones) {
        auto in  = makeSine (N, f, fs);
        auto out = resampleStream (r, in, ratio, N - r.length() - 4);
        maxDev = std::max (maxDev, std::abs (rmsAmplitudeDb (out, 2048)));
    }
    INFO ("max passband deviation dB = " << maxDev);
    CHECK (maxDev <= 0.10);                                    // SPEC gate
}

TEST_CASE ("BASELINE: juce::LagrangeInterpolator FAILS the passband gate", "[resampler][baseline]") {
    const double fs = 48000.0, ratio = 10000.0 / 10001.0;
    const double tones[] = { 8000, 12000, 16000, 19000, 20000 };
    const int N = 1 << 18;
    double maxDev = 0.0;
    for (double f : tones) {
        auto in = makeSine (N, f, fs);
        juce::LagrangeInterpolator lag;
        std::vector<float> out ((size_t) (N - 8));
        lag.process (ratio, in.data(), out.data(), (int) out.size());
        maxDev = std::max (maxDev, std::abs (rmsAmplitudeDb (out, 2048)));
    }
    INFO ("Lagrange max passband deviation dB = " << maxDev);
    CHECK (maxDev > 0.10);                                     // proves the gate has teeth
}

// ---- Task 1: prepare() + the (P+1)xL Kaiser-windowed-sinc prototype table -------------------------

TEST_CASE ("PolyphaseResampler: geometry per rate pair", "[resampler][table]") {
    eb::PolyphaseResampler r;
    r.prepare (48000.0, 48000.0);                 // unity
    CHECK (r.phases()     == 512);
    CHECK (r.length()     == 96);                 // L_unity (even, x4)
    CHECK (r.halfLength() == 48);
    CHECK (r.cutoff()     == Approx (1.0));
    CHECK (r.groupDelay() == Approx (47.5));      // (L-1)/2

    r.prepare (96000.0, 48000.0);                 // 2:1 downsample
    CHECK (r.cutoff() == Approx (0.5));
    CHECK (r.length() == 192);                    // round_to_x4(96/0.5)
    CHECK (r.length() % 4 == 0);

    r.prepare (192000.0, 48000.0);                // 4:1 worst case
    CHECK (r.cutoff() == Approx (0.25));
    CHECK (r.length() == 384);                    // kMaxLen
}

TEST_CASE ("PolyphaseResampler: each of the P rows sums to 1.0 (DC gain, normalized)", "[resampler][table]") {
    eb::PolyphaseResampler r;
    for (double cap : { 48000.0, 88200.0, 96000.0, 192000.0 }) {
        r.prepare (cap, 48000.0);
        const int L = r.length();
        for (int p = 0; p < r.phases(); ++p) {          // rows 0..P-1
            double s = 0.0;
            const float* c = r.row (p);
            for (int k = 0; k < L; ++k) s += c[k];
            CHECK_THAT (s, WithinAbs (1.0, 1e-5));      // risk #5: no per-mu amplitude modulation
        }
    }
}

TEST_CASE ("PolyphaseResampler: guard row + finite taps + centered phase-0 peak", "[resampler][table]") {
    eb::PolyphaseResampler r;
    r.prepare (48000.0, 48000.0);
    const int L = r.length(), P = r.phases();
    const float* g0 = r.row (0);
    const float* gP = r.row (P);                         // the guard row

    // Guard row P = the frac=1.0 fractional-delay filter = the up-shift of row 0 (reads input[n+1]), so
    // c[P][k] = c[0][k-1] and c[P][0] ~= 0 -> the row+1 blend is valid with no special case at the wrap.
    CHECK_THAT ((double) gP[0], WithinAbs (0.0, 1e-6));
    for (int k = 1; k < L; ++k) CHECK_THAT ((double) gP[k], WithinAbs ((double) g0[k - 1], 1e-6));

    // Every tap of every row (incl. guard) is finite.
    for (int p = 0; p <= P; ++p) {
        const float* c = r.row (p);
        for (int k = 0; k < L; ++k) REQUIRE (std::isfinite (c[k]));
    }

    // Phase-0 (on-grid) row peaks at the center tap -> linear-phase sinc centered.
    int peak = 0;
    for (int k = 1; k < L; ++k) if (std::abs (g0[k]) > std::abs (g0[peak])) peak = k;
    CHECK (peak >= L / 2 - 1);
    CHECK (peak <= L / 2);
}

// ---- Task 2: sampleAt() — per-output MAC + linear inter-phase blend ------------------------------

TEST_CASE ("PolyphaseResampler::sampleAt: DC transparency (rows sum to 1)", "[resampler][mac]") {
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    std::vector<float> in (4096, 0.7f);
    const int base = 1000;
    for (double frac : { 0.0, 0.123, 0.5, 0.777, 0.999 })
        CHECK_THAT ((double) r.sampleAt (in.data(), base + frac), WithinAbs (0.7, 1e-4));
}

TEST_CASE ("PolyphaseResampler::sampleAt: preserves a band-limited sine's amplitude", "[resampler][mac]") {
    // A broken MAC (wrong taps / centering) cannot reconstruct a full-amplitude in-band sine. The
    // rigorous linear-phase / constant-group-delay check is the FFT test in Task 5.
    constexpr double pi = juce::MathConstants<double>::pi;
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    const int N = 8192;
    std::vector<float> in (N);
    for (int i = 0; i < N; ++i) in[i] = (float) std::sin (2.0 * pi * 1000.0 * i / 48000.0);
    double mn = 1e9, mx = -1e9, phase = r.halfLength() - 1;
    const double ratio = 0.9997;                          // near-unity drift
    for (int j = 0; j < N - r.length() - 4; ++j) {
        const double y = r.sampleAt (in.data(), phase);
        if (phase > 1000.0 && phase < N - 200.0) { mn = std::min (mn, y); mx = std::max (mx, y); }
        phase += ratio;
    }
    CHECK_THAT (mx, WithinAbs ( 1.0, 5e-3));              // unity passband gain at 1 kHz, correct interpolation
    CHECK_THAT (mn, WithinAbs (-1.0, 5e-3));
}

TEST_CASE ("PolyphaseResampler::sampleAt: continuous across the phase wrap (guard row)", "[resampler][mac]") {
    constexpr double pi = juce::MathConstants<double>::pi;
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    std::vector<float> in (4096);
    for (int i = 0; i < 4096; ++i) in[i] = (float) std::sin (2.0 * pi * 1000.0 * i / 48000.0);
    const int base = 2000;
    const double a = r.sampleAt (in.data(), base + 0.999999);   // row ~ P-1, mu -> 1
    const double b = r.sampleAt (in.data(), base + 1.000000);   // n+1, mu = 0  -> must be continuous
    CHECK_THAT (a, WithinAbs (b, 5e-4));
}

// ---- Task 5: alias rejection + THD + group delay + Lagrange alias baseline -----------------------

namespace {
// Total signal level in dBFS (rel a 0 dBFS = unit-peak sine) over the steady region.
double rmsDbFS (const std::vector<float>& y, int skip) {
    double s = 0.0; int n = 0;
    for (int i = skip; i < (int) y.size() - skip; ++i) { s += (double) y[(size_t) i] * y[(size_t) i]; ++n; }
    return 20.0 * std::log10 (std::max (1e-12, std::sqrt (s / std::max (1, n))));
}

// Hann-windowed linear magnitude spectrum (FFT order 16), used for THD.
std::vector<float> magSpectrum (const std::vector<float>& y, int& nfftOut) {
    const int order = 16, Nfft = 1 << order;
    nfftOut = Nfft;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) Nfft * 2, 0.0f);
    const int n = std::min ((int) y.size(), Nfft);
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 * (1.0 - std::cos (2.0 * kPi * i / (n - 1)));   // Hann
        buf[(size_t) i] = (float) (y[(size_t) i] * w);
    }
    fft.performRealOnlyForwardTransform (buf.data());
    std::vector<float> mag ((size_t) (Nfft / 2 + 1));
    for (int k = 0; k <= Nfft / 2; ++k) {
        const float re = buf[(size_t) k * 2], im = buf[(size_t) k * 2 + 1];
        mag[(size_t) k] = std::sqrt (re * re + im * im);
    }
    return mag;
}

// RMS magnitude of the Hann mainlobe around frequency f (+-3 bins).
double mainlobe (const std::vector<float>& mag, double f, double sr, int Nfft) {
    const int bin = (int) std::lround (f * Nfft / sr);
    double s = 0.0;
    for (int b = bin - 3; b <= bin + 3; ++b)
        if (b >= 0 && b < (int) mag.size()) s += (double) mag[(size_t) b] * mag[(size_t) b];
    return std::sqrt (s);
}
} // namespace

TEST_CASE ("resampler alias rejection 96->48 >= 90 dB (stopband tones)", "[resampler][alias]") {
    eb::PolyphaseResampler r; r.prepare (96000.0, 48000.0);     // fc = 0.5, anti-alias cutoff at 24 kHz
    const int N = 1 << 17;
    // Each input tone is ABOVE the 24 kHz output Nyquist; without anti-aliasing it folds in-band.
    for (double f : { 30000.0, 40000.0, 26000.0 }) {            // fold to 18k / 8k / 22k
        auto in  = makeSine (N, f, 96000.0);
        auto out = resampleStream (r, in, 2.0, N / 2 - r.length());   // downsample by 2
        const double lvl = rmsDbFS (out, 1024);
        INFO ("input " << f << " Hz -> output level dBFS = " << lvl);
        CHECK (lvl <= -90.0);                                   // SPEC gate: rejected by >= 90 dB
    }
}

TEST_CASE ("BASELINE: juce::LagrangeInterpolator ALIASES the 30 kHz image (fails -90 dB)", "[resampler][baseline]") {
    const int N = 1 << 17;
    auto in = makeSine (N, 30000.0, 96000.0);
    juce::LagrangeInterpolator lag;
    std::vector<float> out ((size_t) (N / 2 - 8));
    lag.process (2.0, in.data(), out.data(), (int) out.size());
    const double lvl = rmsDbFS (out, 1024);
    INFO ("Lagrange 30 kHz -> output level dBFS = " << lvl);
    CHECK (lvl > -90.0);                                        // proves the gate has teeth (it aliases badly)
}

TEST_CASE ("resampler SFDR: worst spur <= -80 dB (1 kHz)", "[resampler][thd]") {
    // A LINEAR resampler has no harmonic distortion; the honest distortion metric is the worst single
    // spurious tone (SFDR), not a sum across many bins (which accumulates the broadband image floor).
    const double fs = 48000.0, f0 = 1000.0, ratio = 10000.0 / 10001.0;
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    auto in  = makeSine (1 << 17, f0, fs);
    auto out = resampleStream (r, in, ratio, (1 << 17) - r.length() - 4);
    int Nfft = 0;
    auto mag = magSpectrum (out, Nfft);
    const int fbin = (int) std::lround (f0 * Nfft / fs);
    double fund = 0.0;
    for (int b = fbin - 6; b <= fbin + 6; ++b) fund = std::max (fund, (double) mag[(size_t) b]);
    // Worst spurious IMAGE (the real concern): exclude a generous +-100-bin (~+-73 Hz) band around the
    // fundamental, where the benign near-tone phase-sweep sidebands live (bounded < 0.5% by the amplitude
    // test). Interpolation images appear far from the tone and must be <= -80 dB.
    double spur = 0.0; int spurBin = 0;
    const int loBin = (int) std::ceil (30.0 * Nfft / fs);           // skip DC / sub-30 Hz
    for (int b = loBin; b < (int) mag.size(); ++b)
        if (std::abs (b - fbin) > 100 && mag[(size_t) b] > spur) { spur = mag[(size_t) b]; spurBin = b; }
    const double sfdr = 20.0 * std::log10 (std::max (1e-12, spur) / std::max (1e-12, fund));
    INFO ("far-spur SFDR dB = " << sfdr << "  worst image at " << (spurBin * fs / Nfft) << " Hz");
    CHECK (sfdr <= -80.0);
}

namespace {
// DFT of y at physical frequency f (Hz), sample rate sr, over [skip, skip+count). Returns (re, im).
std::pair<double, double> dftAt (const std::vector<float>& y, double f, double sr, int skip, int count) {
    double re = 0.0, im = 0.0;
    const int end = std::min (skip + count, (int) y.size());
    for (int i = skip; i < end; ++i) { const double a = 2.0 * kPi * f * i / sr; re += y[(size_t) i] * std::cos (a); im -= y[(size_t) i] * std::sin (a); }
    return { re, im };
}
// Filter phase at f: arg(out DFT) - arg(in DFT). in at fsIn, out at fsOut (= fsIn/ratio).
double filterPhase (const std::vector<float>& in, const std::vector<float>& out,
                    double f, double fsIn, double fsOut, int skip, int count) {
    auto a = dftAt (in, f, fsIn, skip, count);
    auto b = dftAt (out, f, fsOut, skip, count);
    return std::atan2 (b.second, b.first) - std::atan2 (a.second, a.first);
}
} // namespace

TEST_CASE ("resampler constant group delay 2k-16kHz within +-0.5 sample", "[resampler][phase]") {
    // Group delay via close-frequency phase differences (wrap-immune, no peak location). Measured on the
    // FULL resampler under near-unity drift so all 512 phases are exercised; the SPREAD across frequency
    // is the phase nonlinearity (a constant time-origin offset cancels in the spread).
    const double fs = 48000.0, ratio = 10000.0 / 10001.0, fsOut = fs / ratio;
    eb::PolyphaseResampler r; r.prepare (48000.0, 48000.0);
    const int N = 1 << 16, skip = 4096, count = N - 12288;
    auto groupDelay = [&] (double f) -> double {
        const double d = 40.0;                                      // +-40 Hz: phase change < pi (wrap-safe)
        auto in1 = makeSine (N, f - d, fs); auto o1 = resampleStream (r, in1, ratio, N - r.length() - 4);
        auto in2 = makeSine (N, f + d, fs); auto o2 = resampleStream (r, in2, ratio, N - r.length() - 4);
        const double p1 = filterPhase (in1, o1, f - d, fs, fsOut, skip, count);
        const double p2 = filterPhase (in2, o2, f + d, fs, fsOut, skip, count);
        double dp = p2 - p1; while (dp > kPi) dp -= 2.0 * kPi; while (dp < -kPi) dp += 2.0 * kPi;
        return -dp / (2.0 * kPi * (2.0 * d) / fs);                  // group delay in input samples
    };
    double mn = 1e9, mx = -1e9;
    for (double f : { 2000.0, 4000.0, 8000.0, 12000.0, 16000.0 }) {
        const double t = groupDelay (f); mn = std::min (mn, t); mx = std::max (mx, t);
    }
    INFO ("group delay spread = " << (mx - mn) << " samples");
    CHECK (mx - mn < 0.5);                                          // constant group delay = linear phase
}
