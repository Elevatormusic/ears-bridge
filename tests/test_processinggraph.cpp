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

// Spin the graph until BOTH juce::dsp::Convolution instances finish their ASYNC IR
// load + gain ramp, so steady-state DC-through-identity equals the input on each
// channel. Brief sleeps give the Convolution background-loader threads wall-clock
// time; under CPU contention those threads can be starved, so the budget is generous
// and we return as soon as the values converge rather than after a fixed rep count.
//
// We wait for BOTH channels deliberately: convL (read via TwoPassLeft) and convR (read
// via TwoPassRight) load and ramp independently, so keying "settled" on L alone could
// return while R is still mid-ramp -- which then shows up as drift in the Average/Sum/
// TwoPassRight assertions below. lVal/rVal/reps are reported so a REQUIRE failure shows
// how far each channel got (i.e. distinguishes "never settled" from a later value drift).
struct WarmUpResult { bool settled; float lVal; float rVal; int reps; };

static WarmUpResult warmUp (eb::ProcessingGraph& g, const std::vector<float>& inL,
                            const std::vector<float>& inR, std::vector<float>& out,
                            int N, float targetL, float targetR) {
    constexpr float convergeTol = 1.0e-4f; // tighter than the post-settle assertion tol
    constexpr int   maxReps     = 10000;   // ~10 s of 1 ms sleeps; ample under contention
    constexpr int   stableReps  = 4;       // consecutive blocks both channels must hold
    float lVal = 0.0f, rVal = 0.0f;
    int stable = 0;
    for (int rep = 0; rep < maxReps; ++rep) {
        g.setCombineMode (eb::CombineMode::TwoPassLeft);
        g.process (inL.data(), inR.data(), out.data(), N);
        lVal = out[N - 1];
        g.setCombineMode (eb::CombineMode::TwoPassRight);
        g.process (inL.data(), inR.data(), out.data(), N);
        rVal = out[N - 1];

        if (std::abs (lVal - targetL) < convergeTol && std::abs (rVal - targetR) < convergeTol) {
            if (++stable >= stableReps) return { true, lVal, rVal, rep + 1 };
        } else {
            stable = 0;
        }
        juce::Thread::sleep (1);
    }
    return { false, lVal, rVal, maxReps };
}

TEST_CASE("ProcessingGraph combine modes with identity FIRs") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);

    // Wait out the async IR load + gain ramp on both channels before asserting.
    auto wu = warmUp (g, inL, inR, out, N, inL[0], inR[0]);
    INFO ("warmUp settled=" << wu.settled << " reps=" << wu.reps
          << "  L=" << wu.lVal << " (want " << inL[0] << ")"
          << "  R=" << wu.rVal << " (want " << inR[0] << ")");
    REQUIRE (wu.settled); // failure here => convolutions never settled (not value drift)

    // At unity gain steady state is ~exact; 1e-3 still leaves margin over warmUp's 1e-4
    // convergence, so a CHECK failure here means real combine-math drift, not warm-up.
    SECTION("Average") {
        g.setCombineMode (eb::CombineMode::Average);
        g.process (inL.data(), inR.data(), out.data(), N);
        INFO ("Average out[N-1]=" << out[N - 1] << " want 0.4");
        CHECK_THAT(out[N-1], WithinAbs(0.4f, 1e-3)); // (0.5+0.3)/2
    }
    SECTION("Sum") {
        g.setCombineMode (eb::CombineMode::Sum);
        g.process (inL.data(), inR.data(), out.data(), N);
        INFO ("Sum out[N-1]=" << out[N - 1] << " want 0.8");
        CHECK_THAT(out[N-1], WithinAbs(0.8f, 1e-3));
    }
    SECTION("TwoPassLeft") {
        g.setCombineMode (eb::CombineMode::TwoPassLeft);
        g.process (inL.data(), inR.data(), out.data(), N);
        INFO ("TwoPassLeft out[N-1]=" << out[N - 1] << " want 0.5");
        CHECK_THAT(out[N-1], WithinAbs(0.5f, 1e-3));
    }
    SECTION("TwoPassRight") {
        g.setCombineMode (eb::CombineMode::TwoPassRight);
        g.process (inL.data(), inR.data(), out.data(), N);
        INFO ("TwoPassRight out[N-1]=" << out[N - 1] << " want 0.3");
        CHECK_THAT(out[N-1], WithinAbs(0.3f, 1e-3));
    }
}

TEST_CASE("Real R_HPN cal cuts the ~4 kHz EARS resonance after convolution") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_8604350.txt");
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
