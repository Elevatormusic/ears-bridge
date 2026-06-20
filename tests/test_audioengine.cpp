#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include "cal/FirDesigner.h"
#include <vector>
#include <cmath>
#include <limits>
using Catch::Matchers::WithinAbs;

TEST_CASE("firTapsForRate scales 8192@48k to power-of-two per rate") {
    CHECK (eb::firTapsForRate (48000.0)  == 8192);
    CHECK (eb::firTapsForRate (96000.0)  == 16384);
    CHECK (eb::firTapsForRate (192000.0) == 32768);
    CHECK (eb::firTapsForRate (44100.0)  == 8192);    // 7526 -> next pow2
    CHECK (eb::firTapsForRate (88200.0)  == 16384);
}

static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("AudioEngine headless seam: identity FIR + Average combine averages the ears") {
    eb::AudioEngine eng;
    const int N = 64;
    eng.prepareForTest (48000.0, N);
    eng.setLeftCalFir  (unitImpulse (8));
    eng.setRightCalFir (unitImpulse (8));
    eng.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);
    // Spin a few times via the seam to settle the async IR load + gain ramp.
    bool settled = false;
    for (int rep = 0; rep < 3000 && ! settled; ++rep) {
        eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), N);
        if (std::abs (out[N-1] - 0.4f) < 1e-3f) settled = true;
        else juce::Thread::sleep (1);
    }
    REQUIRE (settled);
    CHECK_THAT (out[N-1], WithinAbs (0.4f, 1e-3));   // (0.5+0.3)/2
}

TEST_CASE("AudioEngine seam: a clipped raw input invalidates the measurement") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> inL { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };  // 3-sample rail run
    std::vector<float> inR (inL.size(), 0.0f);
    std::vector<float> mono (inL.size(), 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), (int) inL.size());
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine seam: a clean input stays valid") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> inL (8, 0.5f), inR (8, 0.3f), mono (8, 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), 8);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine: real R_HPN cal designed at 96k cuts the 4 kHz resonance") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_0000000.txt");
    REQUIRE (f.existsAsFile());
    auto cal = eb::CalFile::parse (f.loadFileAsString());

    eb::AudioEngine eng;
    const int N = 128;
    eng.prepareForTest (96000.0, N);
    eng.setSampleRate (96000.0);            // active rate drives tap count = 16384
    eng.loadRightCal (cal);                 // designs FIR at 96k internally
    eng.setCombineMode (eb::CombineMode::RightOnly);

    // Drive a 4 kHz sine and confirm it is strongly attenuated vs a 200 Hz reference.
    auto rms = [&](double freq) {
        std::vector<float> inL (N, 0.0f), inR (N, 0.0f), out (N, 0.0f);
        double ph = 0.0, d = 2.0 * juce::MathConstants<double>::pi * freq / 96000.0;
        double acc = 0.0; int blocks = 200;
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < N; ++i) { inR[i] = (float) std::sin (ph); ph += d; }
            eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), N);
            if (b > 40) for (int i = 0; i < N; ++i) acc += (double) out[i] * out[i];
            else juce::Thread::sleep (1);   // let IR settle during warm-up blocks
        }
        return std::sqrt (acc / (double) ((blocks - 41) * N));
    };
    double r200 = rms (200.0), r4k = rms (4000.0);
    INFO ("rms200=" << r200 << " rms4k=" << r4k);
    CHECK (r4k < 0.4 * r200);   // ~4 kHz resonance inverted -> strong cut
}

TEST_CASE("AudioEngine seam: a NaN input is sanitized and does NOT poison the FIR for later blocks") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> bad (8, 0.3f); bad[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> r0  (8, 0.0f), mono (8, 0.0f);
    e.processCaptureBlockForTest (bad.data(), r0.data(), mono.data(), 8);
    for (float v : mono) CHECK (std::isfinite (v));               // output never non-finite
    CHECK (eb::any (e.health().flags & eb::HealthFlag::NonFinite));
    CHECK_FALSE (e.cleanCapture());

    // A subsequent CLEAN block must produce finite output (proves the convolution wasn't poisoned).
    std::vector<float> clean (8, 0.4f), r1 (8, 0.0f), mono2 (8, 0.0f);
    e.processCaptureBlockForTest (clean.data(), r1.data(), mono2.data(), 8);
    for (float v : mono2) CHECK (std::isfinite (v));
}

TEST_CASE("AudioEngine::rawRail defaults to unverified before any run") {
    eb::AudioEngine e;
    auto rr = e.rawRail();
    CHECK_FALSE (rr.verified);
    CHECK (rr.requestedRate == 0.0);
    CHECK (rr.mixRate == 0.0);
}
