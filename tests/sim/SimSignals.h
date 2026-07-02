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

} // namespace ebsim
