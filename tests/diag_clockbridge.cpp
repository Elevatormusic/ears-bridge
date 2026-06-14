#include "audio/ClockBridge.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <cstdio>
#include <vector>

// Reproduce the ClockBridge peek/advance scheme with a raw FIFO+interpolator, block by block,
// and print used/needIn per block plus error, to find where multi-call diverges from one-shot.
int main() {
    const double captureRate = 48000.0, renderRate = 48000.0, f0 = 997.0;
    const double ratio = captureRate / renderRate; // 1.0 (no trim, to isolate the SRC plumbing)
    const int capacity = 8192, renderBlock = 256, renderBlocks = 80;

    juce::AbstractFifo fifo { 1 }; fifo.setTotalSize (capacity);
    std::vector<float> ring ((size_t) capacity, 0.0f);
    std::vector<float> srcInput ((size_t) capacity, 0.0f);
    juce::LagrangeInterpolator src; src.reset();

    double phase = 0.0, dPhaseCap = 2.0 * juce::MathConstants<double>::pi * f0 / captureRate;
    auto pushBlock = [&](int n) {
        std::vector<float> tmp (n);
        for (int i = 0; i < n; ++i) { tmp[i] = (float) std::sin (phase); phase += dPhaseCap; }
        int s1,sz1,s2,sz2; fifo.prepareToWrite (n, s1,sz1,s2,sz2);
        if (sz1>0) juce::FloatVectorOperations::copy (ring.data()+s1, tmp.data(), sz1);
        if (sz2>0) juce::FloatVectorOperations::copy (ring.data()+s2, tmp.data()+sz1, sz2);
        fifo.finishedWrite (sz1+sz2);
    };
    // prime
    pushBlock (2048);

    std::vector<float> out (renderBlock, 0.0f);
    double maxErr = 0.0; double rPhase = 0.0, dRen = 2.0*juce::MathConstants<double>::pi*f0/renderRate;
    int outIdx = 0;
    for (int b = 0; b < renderBlocks; ++b) {
        pushBlock (renderBlock); // exact-rate feed
        int needIn = (int) std::ceil (ratio * renderBlock) + 4;
        needIn = juce::jmin (needIn, (int) srcInput.size());
        int avail = fifo.getNumReady();
        int toRead = juce::jmin (needIn, avail);
        int s1,sz1,s2,sz2; fifo.prepareToRead (toRead, s1,sz1,s2,sz2);
        if (sz1>0) juce::FloatVectorOperations::copy (srcInput.data(), ring.data()+s1, sz1);
        if (sz2>0) juce::FloatVectorOperations::copy (srcInput.data()+sz1, ring.data()+s2, sz2);
        int usedIn = src.process (ratio, srcInput.data(), out.data(), renderBlock, toRead, 0);
        fifo.finishedRead (juce::jmin (usedIn, toRead));
        for (int i = 0; i < renderBlock; ++i) {
            double ideal = std::sin (rPhase); rPhase += dRen;
            if (b > 4) maxErr = std::max (maxErr, std::abs ((double) out[i] - ideal));
            ++outIdx;
        }
        if (b < 8 || b % 20 == 0)
            std::printf ("b=%2d needIn=%d avail=%d toRead=%d usedIn=%d ready_after=%d out[0..3]=%.3f %.3f %.3f %.3f\n",
                         b, needIn, avail, toRead, usedIn, fifo.getNumReady(),
                         out[0], out[1], out[2], out[3]);
    }
    std::printf ("multi-call peek/advance: maxErr(no phase corr)=%.5f\n", maxErr);
    return 0;
}
