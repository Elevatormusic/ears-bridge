#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {

// A vertical level meter: green->yellow->red gradient fill scaled by the supplied
// linear peak amplitude (0..1+), with a short-decay peak-hold tick and a clip LED.
// The owning component pushes new values from its GUI timer via setLevel(); the meter
// does no audio-thread work itself.
class LevelMeter : public juce::Component {
public:
    explicit LevelMeter (juce::String caption = {});

    // peak: linear amplitude (0 = silence, 1 = full scale). clip: latch the red LED.
    void setLevel (float peakLinear, bool clip);

    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float level = 0.0f;       // smoothed display level (linear)
    float peakHold = 0.0f;    // decaying peak marker (linear)
    bool  clipLatched = false;

    static float linearToFrac (float linear);   // map linear amp -> 0..1 bar fill via dBFS
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace eb
