#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

// ==================================================================================================
// Synthetic-measurement-rig signal layer (spec: docs/superpowers/specs/2026-07-02-synthetic-
// measurement-rig-design.md). PURE + DETERMINISTIC: builds the "virtual Dirac session" — the
// on-device-confirmed render program [stereo level tone][gap][L sweep][gap][R sweep][gap][trailing
// L sweep], hard-panned (the idle channel is DIGITAL silence) — plus composable impairment
// transforms applied to the mic-side copy. No JUCE, no engine deps: the rig's signal layer must be
// reasoned about (and unit-tested) in isolation from the pipeline it feeds.
//
// Research-validated decisions encoded here (see the spec's ▸RV notes):
//  - ESS is the plain Farina form with 2 ms raised-cosine fades — NOT Novak-synchronized (the
//    production reference is Dirac's arbitrary sweep; the gate was on-device-tuned against it).
//  - sweepSeconds >= 3.2: production's learn validates each TRIMMED sweep with minSeconds = 3.0
//    (MainComponent: validateReferenceCapture(trim, rate, 3.0)) — shorter sweeps would be rejected
//    by the very path the rig mirrors.
//  - contentPpm models the CONTENT-vs-capture clock skew (Dirac's playback clock vs the EARS clock)
//    by scaling the sweep duration/phase via EXACT phase math — that skew is acoustic and passes
//    through no resampler in production, so none is used here.
// ==================================================================================================
namespace ebsim {

struct StereoTimeline {
    std::vector<float> L, R;
    double fs = 48000.0;
};

struct SessionSpec {
    double fs           = 48000.0;
    double f1           = 20.0;
    double f2           = 20000.0;
    double sweepSeconds = 3.2;     // >= 3.2 (the production learn floor; see the header comment)
    double gapSeconds   = 0.5;
    double toneSeconds  = 1.0;     // the leading stereo level tone Dirac plays before the sweeps
    float  sweepPeak    = 0.5f;    // ~-6 dBFS drive (validateReferenceCapture wants [-20, -3] dBFS)
    float  tonePeak     = 0.01f;   // ~-40 dBFS, both channels — must be IGNORED by the schedule learner
};

struct SessionTruth {
    int toneStart = 0, toneLen = 0;
    struct Seg { int start = 0, len = 0, ear = 0; };   // ear 0 = L, 1 = R
    std::vector<Seg> segments;                          // ground truth, in time order: L, R, trailing L
};

// The plain Farina exponential sine sweep with 2 ms raised-cosine fade-in/out (the repo's test
// idiom). `seconds` is scaled by the caller for content drift; peak sets the drive.
inline std::vector<float> makeEss (double seconds, double fs, double f1, double f2, float peak) {
    const int n = std::max (1, (int) std::llround (seconds * fs));
    std::vector<float> x ((size_t) n, 0.0f);
    const double T  = (double) n / fs;
    const double pi = 3.14159265358979323846;
    const double w1 = 2.0 * pi * f1;
    const double w2 = 2.0 * pi * f2;
    const double K  = std::log (w2 / w1);
    const double A  = w1 * T / K;
    for (int i = 0; i < n; ++i) {
        const double t   = (double) i / fs;
        const double phi = A * (std::exp ((t / T) * K) - 1.0);
        x[(size_t) i] = peak * (float) std::sin (phi);
    }
    const int fade = std::min (n / 4, (int) std::lround (0.002 * fs));   // 2 ms raised-cosine
    for (int i = 0; i < fade; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) pi * (float) i / (float) fade));
        x[(size_t) i]           *= w;
        x[(size_t) (n - 1 - i)] *= w;
    }
    return x;
}

// Build the virtual Dirac session. contentPpm scales the SWEEP duration by (1 + ppm*1e-6) — the
// exact-phase-math model of the playback-vs-capture clock skew. Gaps/tone are unscaled (their
// exact lengths are not clock-critical; the schedule learner measures them loosely).
inline StereoTimeline makeDiracSession (const SessionSpec& spec, SessionTruth& truth,
                                        double contentPpm = 0.0) {
    truth = {};
    StereoTimeline tl; tl.fs = spec.fs;

    const double scale     = 1.0 + contentPpm * 1e-6;
    const auto   sweep     = makeEss (spec.sweepSeconds * scale, spec.fs, spec.f1, spec.f2, spec.sweepPeak);
    const int    gapLen    = (int) std::llround (spec.gapSeconds  * spec.fs);
    const int    toneLen   = (int) std::llround (spec.toneSeconds * spec.fs);
    const int    sweepLen  = (int) sweep.size();
    const int    total     = toneLen + gapLen + 3 * sweepLen + 2 * gapLen + gapLen;   // + trailing gap

    tl.L.assign ((size_t) total, 0.0f);
    tl.R.assign ((size_t) total, 0.0f);

    int pos = 0;
    // The leading STEREO level tone (~1 kHz) on BOTH channels — active but NOT hard-panned, so the
    // production schedule learner must ignore it (|Ldb - Rdb| < panMinDb).
    {
        const double pi = 3.14159265358979323846;
        for (int i = 0; i < toneLen; ++i) {
            const float v = spec.tonePeak * (float) std::sin (2.0 * pi * 1000.0 * (double) i / spec.fs);
            tl.L[(size_t) (pos + i)] = v;
            tl.R[(size_t) (pos + i)] = v;
        }
        truth.toneStart = pos; truth.toneLen = toneLen;
        pos += toneLen + gapLen;
    }
    // Hard-panned sweeps: L, R, trailing L (the confirmed Dirac order). The idle channel stays
    // DIGITAL silence — the ~104 dB loopback isolation measured on-device.
    const int order[3] = { 0, 1, 0 };
    for (int s = 0; s < 3; ++s) {
        auto& active = (order[s] == 0) ? tl.L : tl.R;
        std::copy (sweep.begin(), sweep.end(), active.begin() + pos);
        truth.segments.push_back ({ pos, sweepLen, order[s] });
        pos += sweepLen;
        if (s < 2) pos += gapLen;
    }
    return tl;
}

// ==================================================================================================
// Impairment transforms — pure, composable, applied to the MIC-side copy of the session. Each maps
// to a real failure mode the pipeline must grade honestly (spec S2–S8).
// ==================================================================================================

// Per-ear headphone/coupler IR: main tap + a decaying early cluster inside ~2.5 ms, scaled to unity
// peak. ▸RV: the point is to land the match-gate's mainLobeConcentration in the 0.2–0.4 band the
// gate was on-device-tuned against (real spread IRs), away from the unrealistic-delta corner.
inline void convolveIr (StereoTimeline& tl) {
    static constexpr struct { int at; float g; } taps[] = {
        { 0, 1.00f }, { 7, 0.55f }, { 16, 0.40f }, { 28, 0.28f }, { 45, 0.18f }, { 120, 0.10f } };
    auto conv = [] (std::vector<float>& x) {
        std::vector<float> y (x.size(), 0.0f);
        float norm = 0.0f; for (auto& t : taps) norm += t.g;      // unity-ish peak preservation
        for (auto& t : taps) {
            const float g = t.g / norm;
            for (size_t i = (size_t) t.at; i < x.size(); ++i) y[i] += g * x[i - (size_t) t.at];
        }
        x.swap (y);
    };
    conv (tl.L); conv (tl.R);
}

// Deterministic room floor: the repo's seeded-LCG idiom (uniform in [-amp, amp]).
inline void addSeededNoise (StereoTimeline& tl, float amp, unsigned seed) {
    auto add = [&seed, amp] (std::vector<float>& x) {
        for (auto& v : x) {
            seed = seed * 1664525u + 1013904223u;
            v += amp * ((float) (seed >> 9) / (float) (1u << 23) - 0.5f) * 2.0f;
        }
    };
    add (tl.L); add (tl.R);
}

// Flat open-back leak: each ear receives the OTHER ear's original signal at leakDb.
inline void mixCrosstalk (StereoTimeline& tl, float leakDb) {
    const float g = std::pow (10.0f, leakDb / 20.0f);
    const auto srcL = tl.L, srcR = tl.R;               // originals (no double-leak)
    for (size_t i = 0; i < tl.L.size(); ++i) {
        tl.L[i] += g * srcR[i];
        tl.R[i] += g * srcL[i];
    }
}

// ▸RV robustness variant: the SAME leak level but 4–8 kHz-emphasised (open-back leak peaks there) —
// an RBJ bandpass biquad (constant-peak-gain) centred at 5.66 kHz spanning the band, so the leak's
// in-band level matches leakDb while low/high frequencies leak much less.
inline void mixCrosstalkShaped (StereoTimeline& tl, float leakDb) {
    const double fs = tl.fs, f0 = std::sqrt (4000.0 * 8000.0);
    const double w0 = 2.0 * 3.14159265358979323846 * f0 / fs;
    const double bw = std::log2 (8000.0 / 4000.0);
    const double alpha = std::sin (w0) * std::sinh (std::log (2.0) / 2.0 * bw * w0 / std::sin (w0));
    // RBJ "constant 0 dB peak gain" bandpass.
    const double b0 = alpha, b1 = 0.0, b2 = -alpha, a0 = 1.0 + alpha, a1 = -2.0 * std::cos (w0), a2 = 1.0 - alpha;
    const float g = std::pow (10.0f, leakDb / 20.0f);
    auto bp = [&] (const std::vector<float>& x) {
        std::vector<float> y (x.size(), 0.0f);
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double yi = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1; x1 = x[i]; y2 = y1; y1 = yi;
            y[i] = (float) yi;
        }
        return y;
    };
    const auto leakToR = bp (tl.L);
    const auto leakToL = bp (tl.R);
    for (size_t i = 0; i < tl.L.size(); ++i) {
        tl.L[i] += g * leakToL[i];
        tl.R[i] += g * leakToR[i];
    }
}

// Memoryless amp distortion: y = x + a2 x^2 + a3 x^3. ▸RV (FFT-verified at full-scale drive A=1):
// ~5% THD -> a2 = 0.10; ~10% -> a2 = 0.22 (H2 = a2 A^2 / 2, H3 = a3 A^3 / 4). THD scales with the
// drive, so scenarios PIN the amplitude. Harmonics past 24 kHz alias deliberately — so does the
// real 48 kHz chain; do not band-limit.
inline void applyDistortion (StereoTimeline& tl, float a2, float a3) {
    auto d = [a2, a3] (std::vector<float>& x) {
        for (auto& v : x) v = v + a2 * v * v + a3 * v * v * v;
    };
    d (tl.L); d (tl.R);
}

inline void applyGainDb (StereoTimeline& tl, float dB) {
    const float g = std::pow (10.0f, dB / 20.0f);
    for (auto& v : tl.L) v *= g;
    for (auto& v : tl.R) v *= g;
}

// Hard rail clamp (drives the ClipConfirmed rail-run detector honestly).
inline void clipHard (StereoTimeline& tl) {
    for (auto& v : tl.L) v = std::clamp (v, -1.0f, 1.0f);
    for (auto& v : tl.R) v = std::clamp (v, -1.0f, 1.0f);
}

// One-pole low-pass: the stand-in for the HISTORICAL corruption class (HF loss in the capture
// path — the ratio-creep / interpolator-droop family). y += a (x - y).
inline void applyHfDroop (StereoTimeline& tl, float cutoffHz) {
    const float a = 1.0f - std::exp (-2.0f * 3.14159265f * cutoffHz / (float) tl.fs);
    auto lp = [a] (std::vector<float>& x) {
        float y = 0.0f;
        for (auto& v : x) { y += a * (v - y); v = y; }
    };
    lp (tl.L); lp (tl.R);
}

} // namespace ebsim
