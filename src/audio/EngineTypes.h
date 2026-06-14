#pragma once
#include <juce_core/juce_core.h>   // juce::String used by sibling spine headers; Plan 4 precondition
namespace eb {

enum class EarsModel  { Unknown, Ears, EarsPro };
enum class EngineStatus { Stopped, Running, Error };

struct Levels {
    float inL = 0, inR = 0, outMono = 0;
    bool  clipL = false, clipR = false, clipOut = false;
};

struct Health {
    int       xruns = 0;
    long long droppedFrames = 0;
    double    fifoFill = 0.0;
    bool      cleanCapture = true;
    double    captureToRenderRatio = 1.0;
};

} // namespace eb
