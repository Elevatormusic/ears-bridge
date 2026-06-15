#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {

// A horizontal level meter row: "L"/"R"/"Out" caption, a track with amber + red zones near
// full scale, a green->amber->red fill scaled by the supplied linear peak amplitude, and a
// dB readout. The owning component pushes values from its GUI timer via setLevel(); the meter
// does no audio-thread work itself.
class LevelMeter : public juce::Component {
public:
    explicit LevelMeter (juce::String caption = {});

    // peak: linear amplitude (0 = silence, 1 = full scale). clip: latch the red state.
    void setLevel (float peakLinear, bool clip);

    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float level = 0.0f;          // smoothed display level (linear)
    bool  clipLatched = false;
    juce::String lastDesc;       // last accessible description set (avoids per-frame churn)

    static float linearToFrac (float linear);   // map linear amp -> 0..1 via dBFS window
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace eb
