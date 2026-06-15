#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "audio/CombineMode.h"
#include <atomic>
namespace eb {
class ProcessingGraph {
public:
    void prepare (double sampleRate, int maxBlockSize);
    void setFir  (int channel, juce::AudioBuffer<float> ir);
    void setCombineMode (CombineMode mode);
    void setOutputGain (float linear);   // applied to the mono output (the "Output trim" control)
    void process (const float* inL, const float* inR, float* outMono, int numSamples);
    void reset();
private:
    juce::dsp::Convolution convL, convR;
    std::atomic<int> combine { (int) CombineMode::TwoPassLeft };
    std::atomic<float> outGain { 1.0f };   // post-combine output gain (1.0 = unity); lock-free for the audio thread
    juce::AudioBuffer<float> scratch;
    double sr = 48000.0; int maxBlock = 0;
};
}
