#include "audio/ProcessingGraph.h"
#include <cmath>

namespace eb {

void ProcessingGraph::prepare (double sampleRate, int maxBlockSize) {
    sr = sampleRate; maxBlock = maxBlockSize;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
    convL.prepare (spec); convR.prepare (spec);
    scratch.setSize (2, maxBlockSize);
    envL_ = envR_ = 0.0f; activeEar_ = 0;   // fresh AutoPerEar state
}

void ProcessingGraph::setFir (int channel, juce::AudioBuffer<float> ir) {
    auto& conv = (channel == 0) ? convL : convR;
    conv.loadImpulseResponse (std::move (ir), sr,
        juce::dsp::Convolution::Stereo::no,
        juce::dsp::Convolution::Trim::no,
        juce::dsp::Convolution::Normalise::no);
}

void ProcessingGraph::setCombineMode (CombineMode mode) {
    combine.store ((int) mode);
}

void ProcessingGraph::setOutputGain (float linear) {
    outGain.store (juce::jlimit (0.0f, 4.0f, linear));   // clamp; 0 = mute, 1 = unity
}

void ProcessingGraph::process (const float* inL, const float* inR,
                               float* outMono, int numSamples) {
    auto* l = scratch.getWritePointer (0);
    auto* r = scratch.getWritePointer (1);
    juce::FloatVectorOperations::copy (l, inL, numSamples);
    juce::FloatVectorOperations::copy (r, inR, numSamples);

    { juce::dsp::AudioBlock<float> b (&l, 1, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (b); convL.process (ctx); }
    { juce::dsp::AudioBlock<float> b (&r, 1, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (b); convR.process (ctx); }

    switch ((CombineMode) combine.load()) {
        case CombineMode::TwoPassLeft:
            juce::FloatVectorOperations::copy (outMono, l, numSamples); break;
        case CombineMode::TwoPassRight:
            juce::FloatVectorOperations::copy (outMono, r, numSamples); break;
        case CombineMode::Sum:
            juce::FloatVectorOperations::copy (outMono, l, numSamples);
            juce::FloatVectorOperations::add  (outMono, r, numSamples); break;
        case CombineMode::Average:
            for (int i = 0; i < numSamples; ++i) outMono[i] = 0.5f * (l[i] + r[i]); break;
        case CombineMode::AutoPerEar: {
            // Per-ear measurement: Dirac drives ONE earcup at a time during its L/R sweep, so follow
            // whichever earcup is currently sounding (the louder RAW mic = the directly-driven ear)
            // and output ONLY that ear's calibrated mic. Each sweep is then a clean single arrival
            // with no open-back leakage summed in. Detect on the RAW input (inL/inR) so per-ear FIR
            // gain differences don't bias the choice; output the post-FIR l/r.
            float pkL = 0.0f, pkR = 0.0f;
            for (int i = 0; i < numSamples; ++i) { pkL = juce::jmax (pkL, std::abs (inL[i])); pkR = juce::jmax (pkR, std::abs (inR[i])); }
            const float rel = std::exp (-(float) numSamples / (float) (sr * 0.08));   // ~80 ms peak-envelope release
            envL_ = juce::jmax (pkL, envL_ * rel);   // fast attack, slow release -> quick onset detection
            envR_ = juce::jmax (pkR, envR_ * rel);
            const int prev = activeEar_;
            // Only reconsider when something is clearly above the noise floor; otherwise HOLD through
            // the inter-sweep silence. Switch only when the OTHER ear is >= 2x (6 dB) louder, so
            // open-back leakage into the far mic can't flip the choice mid-sweep.
            if (envL_ > 0.0032f || envR_ > 0.0032f) {            // ~ -50 dBFS gate
                if (activeEar_ == 0) { if (envR_ > envL_ * 2.0f) activeEar_ = 1; }
                else                 { if (envL_ > envR_ * 2.0f) activeEar_ = 0; }
            }
            const float* cur = (activeEar_ == 0) ? l : r;
            if (activeEar_ != prev) {                            // crossfade old->new across this block (anti-click)
                const float* old = (prev == 0) ? l : r;
                const float inv = 1.0f / (float) juce::jmax (1, numSamples);
                for (int i = 0; i < numSamples; ++i) { const float t = (float) i * inv; outMono[i] = old[i] * (1.0f - t) + cur[i] * t; }
            } else {
                juce::FloatVectorOperations::copy (outMono, cur, numSamples);
            }
            break;
        }
    }

    // Post-combine output trim (the GUI's "Output trim (dB)" slider). Unity by default; skip the
    // multiply at unity to keep the common path free of a needless pass.
    const float g = outGain.load();
    if (g != 1.0f) juce::FloatVectorOperations::multiply (outMono, g, numSamples);
}

void ProcessingGraph::reset() { convL.reset(); convR.reset(); envL_ = envR_ = 0.0f; activeEar_ = 0; }

} // namespace eb
