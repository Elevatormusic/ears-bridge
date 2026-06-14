#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/ProcessingGraph.h"
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

// Spin the graph until juce::dsp::Convolution finishes its ASYNC IR load + gain
// ramp, so steady-state DC-through-identity equals the input. Brief sleeps give the
// Convolution background loader thread wall-clock time. Returns false if never settles.
static bool warmUp (eb::ProcessingGraph& g, const std::vector<float>& inL,
                    const std::vector<float>& inR, std::vector<float>& out, int N) {
    g.setCombineMode (eb::CombineMode::TwoPassLeft);
    for (int rep = 0; rep < 3000; ++rep) {
        g.process (inL.data(), inR.data(), out.data(), N);
        if (std::abs (out[N - 1] - inL[0]) < 1.0e-4f) return true;
        juce::Thread::sleep (1);
    }
    return false;
}

TEST_CASE("ProcessingGraph combine modes with identity FIRs") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);
    REQUIRE (warmUp (g, inL, inR, out, N)); // wait out the async IR load + gain ramp

    SECTION("Average") {
        g.setCombineMode (eb::CombineMode::Average);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.4f, 1e-4)); // (0.5+0.3)/2
    }
    SECTION("Sum") {
        g.setCombineMode (eb::CombineMode::Sum);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.8f, 1e-4));
    }
    SECTION("TwoPassLeft") {
        g.setCombineMode (eb::CombineMode::TwoPassLeft);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.5f, 1e-4));
    }
    SECTION("TwoPassRight") {
        g.setCombineMode (eb::CombineMode::TwoPassRight);
        g.process (inL.data(), inR.data(), out.data(), N);
        CHECK_THAT(out[N-1], WithinAbs(0.3f, 1e-4));
    }
}
