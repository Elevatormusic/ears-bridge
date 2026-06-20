#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "cal/FirDesigner.h"   // FirMode
namespace eb {

// Immutable snapshot of one calibration "generation": the inputs (rate/mode/taps/hashes/serial),
// the BOTH-ear FIRs designed off-thread, the validator verdict, and a diagnostic. The GUI bumps a
// monotonic id, builds one of these off the message thread, and hands it to
// AudioEngine::applyCalibrationGeneration. The generation token lets the Start gate refuse a stale,
// half-installed, or invalid calibration: the gate is satisfied only when requested == built ==
// applied and the applied generation is valid (see AudioEngine::calibrationApplied()).
struct CalibrationGeneration {
    int    id = 0;                 // monotonic; 0 = none
    double sampleRate = 0.0;
    FirMode mode = FirMode::MinPhaseMagnitude;
    int    taps = 0;
    juce::String leftHash, rightHash, serial;
    juce::AudioBuffer<float> leftFir, rightFir;
    float  sharedHeadroom = 1.0f;  // informational; the graph computes its own from the FIR peaks
    bool   valid = false;          // CalibrationPairValidator verdict
    juce::String diagnostic;       // validation reason / build note (shown in the GUI on invalid)
};

} // namespace eb
