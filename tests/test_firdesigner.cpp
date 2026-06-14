#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"

using Catch::Matchers::WithinAbs;

// cal curve: flat 0 dB at 1k, +20 dB at 4k -> inverted target = 0 dB, -20 dB
static eb::CalFile twoPointCal() {
    eb::CalFile c;
    c.points = { {20.0, 0.0, 0.0}, {1000.0, 0.0, 0.0},
                 {4000.0, 20.0, 0.0}, {20000.0, 0.0, 0.0} };
    return c;
}

static double binToHz (int bin, int fftSize, double sr) {
    return (double) bin * sr / (double) fftSize;
}
static int hzToBin (double hz, int fftSize, double sr) {
    return (int) std::lround (hz * (double) fftSize / sr);
}

TEST_CASE("FirDesigner target magnitude is the inverted, interpolated cal curve") {
    eb::FirDesignParams p; p.sampleRate = 48000.0; p.invert = true; p.maxBoostDb = 12.0;
    const int fftSize = 65536;
    auto mag = eb::FirDesigner::targetMagnitudeLinear (twoPointCal(), p, fftSize);
    REQUIRE((int) mag.size() == fftSize/2 + 1);

    auto dbAt = [&](double hz) {
        int bin = hzToBin (hz, fftSize, p.sampleRate);
        return 20.0 * std::log10 (std::max (1e-9f, mag[(size_t) bin]));
    };
    CHECK_THAT(dbAt(1000.0), WithinAbs(0.0, 0.5));     // inverse of 0 dB
    CHECK_THAT(dbAt(4000.0), WithinAbs(-20.0, 0.5));   // inverse of +20 dB
}

TEST_CASE("FirDesigner clamps excessive boost") {
    // cal -30 dB at 100 Hz -> inverted +30 dB, clamped to +12 dB
    eb::CalFile c; c.points = { {20.0,-30.0,0.0}, {100.0,-30.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.invert = true; p.maxBoostDb = 12.0;
    const int fftSize = 65536;
    auto mag = eb::FirDesigner::targetMagnitudeLinear (c, p, fftSize);
    int bin = hzToBin (100.0, fftSize, p.sampleRate);
    double db = 20.0 * std::log10 (std::max (1e-9f, mag[(size_t) bin]));
    CHECK(db <= 12.0 + 0.5);
    CHECK(db >= 12.0 - 0.5);
}

#include <juce_dsp/juce_dsp.h>

// Measure the magnitude (dB) of an impulse response at a given frequency,
// via zero-padded FFT.
static double irMagnitudeDb (const juce::AudioBuffer<float>& ir, double hz, double sr) {
    const int order = 16, fftSize = 1 << order; // 65536
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
    const int n = juce::jmin (ir.getNumSamples(), fftSize);
    for (int i = 0; i < n; ++i) buf[(size_t) i] = ir.getSample (0, i);
    fft.performRealOnlyForwardTransform (buf.data());
    int bin = (int) std::lround (hz * (double) fftSize / sr);
    float re = buf[(size_t) bin * 2], im = buf[(size_t) bin * 2 + 1];
    return 20.0 * std::log10 (std::max (1e-9f, std::sqrt (re*re + im*im)));
}

TEST_CASE("Min-phase FIR magnitude matches the inverted cal within 1 dB") {
    eb::CalFile c; c.points = { {20.0,0.0,0.0}, {1000.0,0.0,0.0},
                                {4000.0,20.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.sampleRate = 48000.0; p.numTaps = 8192;
    p.mode = eb::FirMode::MinPhaseMagnitude; p.invert = true; p.maxBoostDb = 12.0;
    auto ir = eb::FirDesigner::design (c, p);
    REQUIRE(ir.getNumSamples() == 8192);
    CHECK_THAT(irMagnitudeDb (ir, 1000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(0.0, 1.0));
    CHECK_THAT(irMagnitudeDb (ir, 4000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(-20.0, 1.0));
}

TEST_CASE("Min-phase FIR is causal (energy concentrated near the front)") {
    eb::CalFile c; c.points = { {20.0,0.0,0.0}, {4000.0,20.0,0.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.numTaps = 8192; p.mode = eb::FirMode::MinPhaseMagnitude;
    auto ir = eb::FirDesigner::design (c, p);
    double total = 0, firstQuarter = 0;
    for (int i = 0; i < ir.getNumSamples(); ++i) {
        double e = (double) ir.getSample(0,i) * ir.getSample(0,i);
        total += e; if (i < ir.getNumSamples()/4) firstQuarter += e;
    }
    INFO("causality ratio = " << (firstQuarter / total));
    CHECK(firstQuarter / total > 0.9); // min-phase: front-loaded
}

TEST_CASE("Complex-mode FIR matches inverted-cal magnitude within 1 dB") {
    eb::CalFile c; c.points = { {20.0,0.0,0.0}, {1000.0,0.0,0.0},
                                {4000.0,20.0,45.0}, {20000.0,0.0,0.0} };
    eb::FirDesignParams p; p.numTaps = 8192;
    p.mode = eb::FirMode::ComplexWithPhase; p.invert = true; p.maxBoostDb = 12.0;
    auto ir = eb::FirDesigner::design (c, p);
    REQUIRE(ir.getNumSamples() == 8192);
    CHECK_THAT(irMagnitudeDb (ir, 1000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(0.0, 1.0));
    CHECK_THAT(irMagnitudeDb (ir, 4000.0, p.sampleRate),
               Catch::Matchers::WithinAbs(-20.0, 1.0));
}
