#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "cal/CalFile.h"
#include <vector>
namespace eb {
enum class FirMode { MinPhaseMagnitude, ComplexWithPhase };
struct FirDesignParams {
    double sampleRate = 48000.0;
    int    numTaps    = 8192;
    int    designFftOrder = 16;
    FirMode mode      = FirMode::MinPhaseMagnitude;
    bool   invert     = true;
    double maxBoostDb = 12.0;
    double bandLowHz  = 10.0;
    double bandHighHz = 20000.0;
};
class FirDesigner {
public:
    static juce::AudioBuffer<float> design (const CalFile& cal, const FirDesignParams& p);
};
}
