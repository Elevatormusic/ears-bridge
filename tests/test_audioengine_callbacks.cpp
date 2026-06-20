#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include <vector>
#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;

// A unit-impulse FIR makes the per-ear convolution a passthrough so the test can reason about
// the forwarded mono numerically. Settles the async IR load by spinning the capture callback.
static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("AudioEngine callbacks: capture forwards a clean block; render pulls it back") {
    eb::AudioEngine e;
    const int N = 64, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);
    e.setLeftCalFir  (unitImpulse (8));
    e.setRightCalFir (unitImpulse (8));
    e.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f);
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    // Spin the capture callback (async Convolution IR load + gain ramp settle) then pull.
    bool settled = false;
    for (int rep = 0; rep < 3000 && ! settled; ++rep) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
        if (std::abs (outL[N-1] - 0.4f) < 2e-3f) settled = true;   // (0.5+0.3)/2 through the FIFO
        else juce::Thread::sleep (1);
    }
    REQUIRE (settled);
    CHECK (e.cleanCapture());                          // a clean run stays valid
    CHECK (e.health().droppedFrames == 0);
    CHECK_THAT (outR[N-1], WithinAbs (outL[N-1], 1e-6));   // render duplicates mono to L == R
}

TEST_CASE("AudioEngine callbacks: a confirmed rail-run through the real capture callback invalidates") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    // 3 consecutive samples at/above kRailCeiling on L == kRailRunMin -> ClipConfirmed (invalidating).
    std::vector<float> inL { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };
    std::vector<float> inR (inL.size(), 0.0f);
    e.driveCaptureCallback (inL.data(), inR.data(), (int) inL.size());

    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: an isolated single full-scale sample does NOT confirm a clip") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    // A clean full-scale sine touches the rail for ONE sample -> no run -> not a confirmed clip.
    std::vector<float> inL { 0.2f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::vector<float> inR (inL.size(), 0.0f);
    e.driveCaptureCallback (inL.data(), inR.data(), (int) inL.size());

    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a NaN in the raw input invalidates and is not forwarded") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    std::vector<float> inL (N, 0.1f), inR (N, 0.1f);
    inL[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    e.driveCaptureCallback (inL.data(), inR.data(), N);
    e.driveRenderCallback  (outL.data(), outR.data(), N);

    CHECK (eb::any (e.health().flags & eb::HealthFlag::NonFinite));
    CHECK_FALSE (e.cleanCapture());
    for (int i = 0; i < N; ++i) CHECK (std::isfinite (outL[i]));   // no NaN reaches the cable
}
