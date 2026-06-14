#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/ProcessingGraph.h"
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"
#include <juce_dsp/juce_dsp.h>
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

TEST_CASE("Real R_HPN cal cuts the ~4 kHz EARS resonance after convolution") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_0000000.txt");
    REQUIRE(f.existsAsFile());
    auto cal = eb::CalFile::parse (f.loadFileAsString());

    eb::FirDesignParams p; p.sampleRate = 48000.0; p.numTaps = 8192;
    p.mode = eb::FirMode::MinPhaseMagnitude; p.invert = true; p.maxBoostDb = 12.0;
    auto ir = eb::FirDesigner::design (cal, p);

    const int order = 16, fftSize = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
    for (int i = 0; i < juce::jmin (ir.getNumSamples(), fftSize); ++i)
        buf[(size_t) i] = ir.getSample (0, i);
    fft.performRealOnlyForwardTransform (buf.data());
    int bin = (int) std::lround (4000.0 * fftSize / p.sampleRate);
    float re = buf[(size_t) bin*2], im = buf[(size_t) bin*2+1];
    double db = 20.0 * std::log10 (std::max (1e-9f, std::sqrt (re*re + im*im)));
    CHECK(db < -10.0);   // inverse of a large positive bump = a strong cut
}
