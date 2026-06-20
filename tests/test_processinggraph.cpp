#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/ProcessingGraph.h"
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;

static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

// A scaled impulse is a frequency-flat FIR with |H(f)| == gain everywhere, so it exercises the
// auto-headroom path with an exactly-known peak gain.
static juce::AudioBuffer<float> scaledImpulse (int taps, float gain) {
    auto b = unitImpulse (taps); b.applyGain (gain); return b;
}

// Spin the graph until BOTH juce::dsp::Convolution instances finish their ASYNC IR
// load + gain ramp, so steady-state DC-through-identity equals the input on each
// channel. Brief sleeps give the Convolution background-loader threads wall-clock
// time; under CPU contention those threads can be starved, so the budget is generous
// and we return as soon as the values converge rather than after a fixed rep count.
//
// We wait for BOTH channels deliberately: convL (read via LeftOnly) and convR (read
// via RightOnly) load and ramp independently, so keying "settled" on L alone could
// return while R is still mid-ramp -- which then shows up as drift in the Average/Sum/
// RightOnly assertions below. lVal/rVal/reps are reported so a REQUIRE failure shows
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
        g.setCombineMode (eb::CombineMode::LeftOnly);
        g.process (inL.data(), inR.data(), out.data(), N);
        lVal = out[N - 1];
        g.setCombineMode (eb::CombineMode::RightOnly);
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
    SECTION("LeftOnly") {
        g.setCombineMode (eb::CombineMode::LeftOnly);
        g.process (inL.data(), inR.data(), out.data(), N);
        INFO ("LeftOnly out[N-1]=" << out[N - 1] << " want 0.5");
        CHECK_THAT(out[N-1], WithinAbs(0.5f, 1e-3));
    }
    SECTION("RightOnly") {
        g.setCombineMode (eb::CombineMode::RightOnly);
        g.process (inL.data(), inR.data(), out.data(), N);
        INFO ("RightOnly out[N-1]=" << out[N - 1] << " want 0.3");
        CHECK_THAT(out[N-1], WithinAbs(0.3f, 1e-3));
    }
}

TEST_CASE("ProcessingGraph output gain scales the mono output (the Output-trim control)") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));
    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);
    auto wu = warmUp (g, inL, inR, out, N, inL[0], inR[0]);
    REQUIRE (wu.settled);

    g.setCombineMode (eb::CombineMode::LeftOnly);   // pass inL (0.5) straight through

    g.setOutputGain (1.0f);                             // unity -> 0.5
    g.process (inL.data(), inR.data(), out.data(), N);
    CHECK_THAT (out[N-1], WithinAbs (0.5f, 1e-3));

    g.setOutputGain (0.5f);                             // -6 dB -> 0.25
    g.process (inL.data(), inR.data(), out.data(), N);
    CHECK_THAT (out[N-1], WithinAbs (0.25f, 1e-3));

    g.setOutputGain (0.0f);                             // mute -> 0
    g.process (inL.data(), inR.data(), out.data(), N);
    CHECK_THAT (out[N-1], WithinAbs (0.0f, 1e-4));
}

TEST_CASE("ProcessingGraph AutoPerEar follows whichever earcup is sounding (Dirac per-ear)") {
    const int N = 512;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));
    std::vector<float> loud (N, 0.3f), silent (N, 0.0f), out (N, 0.0f);

    auto wu = warmUp (g, loud, silent, out, N, loud[0], silent[0]);   // settle the convolutions
    REQUIRE (wu.settled);

    g.setCombineMode (eb::CombineMode::AutoPerEar);

    // LEFT earcup sweeping (L loud, R silent): output is the LEFT mic, NOT the silent right.
    for (int i = 0; i < 15; ++i) g.process (loud.data(), silent.data(), out.data(), N);
    INFO ("left active out=" << out[N-1]);
    CHECK_THAT (out[N-1], WithinAbs (0.3f, 1e-2));
    CHECK (g.activeEar() == 0);   // published for the GUI "capturing Left/Right" indicator

    // Inter-sweep silence: the held choice should not flip.
    for (int i = 0; i < 15; ++i) g.process (silent.data(), silent.data(), out.data(), N);
    CHECK (g.activeEar() == 0);   // held through the gap, not reset to a default

    // RIGHT earcup sweeping: it switches to the RIGHT mic.
    for (int i = 0; i < 15; ++i) g.process (silent.data(), loud.data(), out.data(), N);
    INFO ("right active out=" << out[N-1]);
    CHECK_THAT (out[N-1], WithinAbs (0.3f, 1e-2));
    CHECK (g.activeEar() == 1);

    // And follows back to LEFT for the validation repeat.
    for (int i = 0; i < 15; ++i) g.process (loud.data(), silent.data(), out.data(), N);
    CHECK_THAT (out[N-1], WithinAbs (0.3f, 1e-2));
    CHECK (g.activeEar() == 0);
}

TEST_CASE("ProcessingGraph auto headroom bounds the output and preserves L/R balance") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    const float gL = 4.0f, gR = 1.0f;                 // L cal boosts +12 dB, R is unity
    g.setFir (0, scaledImpulse (8, gL));
    g.setFir (1, scaledImpulse (8, gR));

    const float headroom = 1.0f / juce::jmax (gL, gR);   // non-Sum modes bound by the louder ear
    std::vector<float> inL (N, 1.0f), inR (N, 1.0f), out (N, 0.0f);   // 0 dBFS input

    auto wu = warmUp (g, inL, inR, out, N, gL * headroom, gR * headroom);
    INFO ("headroom warmUp reps=" << wu.reps << " L=" << wu.lVal << " R=" << wu.rVal);
    REQUIRE (wu.settled);

    // (a) The +12 dB FIR alone would put a 0 dBFS input at 4.0 (hard clip); the makeup pulls it back
    //     to exactly the input level and never above it.
    g.setCombineMode (eb::CombineMode::LeftOnly);
    g.process (inL.data(), inR.data(), out.data(), N);
    const float outL = out[N - 1];
    CHECK (outL <= 1.0f + 1e-3f);
    CHECK_THAT (outL, WithinAbs (1.0f, 1e-2f));

    g.setCombineMode (eb::CombineMode::RightOnly);
    g.process (inL.data(), inR.data(), out.data(), N);
    const float outR = out[N - 1];

    // (b) The SAME makeup is applied to both ears, so the measured L/R ratio is untouched.
    CHECK_THAT (outL / outR, WithinAbs (gL / gR, 1e-2f));
}

TEST_CASE("ProcessingGraph clamps the mono output so it can never exceed full scale") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, unitImpulse (8)); g.setFir (1, unitImpulse (8));   // unity -> headroom 1.0
    std::vector<float> inL (N, 0.8f), inR (N, 0.8f), out (N, 0.0f);
    auto wu = warmUp (g, inL, inR, out, N, inL[0], inR[0]);
    REQUIRE (wu.settled);

    // Sum (+6 dB, intentionally uncompensated) would put 0.8 + 0.8 = 1.6 over full scale; the final
    // hard clamp keeps the cable from ever seeing a sample past 1.0.
    g.setCombineMode (eb::CombineMode::Sum);
    g.process (inL.data(), inR.data(), out.data(), N);
    CHECK (out[N - 1] <= 1.0f + 1e-4f);
    CHECK_THAT (out[N - 1], WithinAbs (1.0f, 1e-3f));
}

TEST_CASE("ProcessingGraph clearFir restores unity passthrough and resets the auto-headroom") {
    const int N = 64;
    eb::ProcessingGraph g; g.prepare (48000.0, N);
    g.setFir (0, scaledImpulse (8, 4.0f)); g.setFir (1, scaledImpulse (8, 4.0f));   // +12 dB -> headroom 0.25
    std::vector<float> inL (N, 1.0f), inR (N, 1.0f), out (N, 0.0f);
    auto wu = warmUp (g, inL, inR, out, N, 1.0f, 1.0f);   // 1.0 * 4 * 0.25 = 1.0
    REQUIRE (wu.settled);

    g.clearFir (0); g.clearFir (1);                       // back to unity FIRs -> headroom 1.0
    auto wu2 = warmUp (g, inL, inR, out, N, 1.0f, 1.0f);  // 1.0 * 1 * 1 = 1.0 (passthrough)
    REQUIRE (wu2.settled);
    g.setCombineMode (eb::CombineMode::LeftOnly);
    g.process (inL.data(), inR.data(), out.data(), N);
    CHECK_THAT (out[N - 1], WithinAbs (1.0f, 1e-2f));     // input passes through unchanged
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

TEST_CASE("ProcessingGraph default combine mode is AutoPerEar (the Dirac per-ear mode)") {
    eb::ProcessingGraph g;
    g.prepare (48000.0, 512);
    std::vector<float> inL (512, 0.0f), inR (512, 0.9f), out (512, 0.0f);   // right ear has signal
    for (int i = 0; i < 64; ++i) g.process (inL.data(), inR.data(), out.data(), 512);
    // AutoPerEar follows whichever ear is sounding -> right (activeEar 1). The old LeftOnly default
    // never runs the per-ear switch, so activeEar stays 0. This fails before the default fix.
    CHECK (g.activeEar() == 1);
}

TEST_CASE("ProcessingGraph::process sanitizes non-finite input and reports it") {
    eb::ProcessingGraph g; g.prepare (48000.0, 8);
    std::vector<float> l (8, 0.2f), r (8, 0.0f), out (8, 0.0f);
    l[2] = std::numeric_limits<float>::infinity();
    const bool bad = g.process (l.data(), r.data(), out.data(), 8);
    CHECK (bad);
    for (float v : out) CHECK (std::isfinite (v));
    // A following clean block stays finite (convolution not poisoned).
    std::vector<float> l2 (8, 0.2f), out2 (8, 0.0f);
    CHECK_FALSE (g.process (l2.data(), r.data(), out2.data(), 8));
    for (float v : out2) CHECK (std::isfinite (v));
}
