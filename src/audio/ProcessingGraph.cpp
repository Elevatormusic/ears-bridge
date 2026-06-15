#include "audio/ProcessingGraph.h"

namespace eb {

void ProcessingGraph::prepare (double sampleRate, int maxBlockSize) {
    sr = sampleRate; maxBlock = maxBlockSize;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
    convL.prepare (spec); convR.prepare (spec);
    scratch.setSize (2, maxBlockSize);
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
    }

    // Post-combine output trim (the GUI's "Output trim (dB)" slider). Unity by default; skip the
    // multiply at unity to keep the common path free of a needless pass.
    const float g = outGain.load();
    if (g != 1.0f) juce::FloatVectorOperations::multiply (outMono, g, numSamples);
}

void ProcessingGraph::reset() { convL.reset(); convR.reset(); }

} // namespace eb
