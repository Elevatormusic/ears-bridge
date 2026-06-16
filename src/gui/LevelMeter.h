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

    // Draw the green "aim here" target band (capture meters only). The Out meter leaves it off and
    // keeps just the clip-danger zones.
    void setShowTargetBand (bool on) noexcept { showTargetBand = on; }

    // Mark this meter as the earcup currently being captured in AutoPerEar mode: an accent "live" dot
    // (a shape, not colour alone) + accent label. Repaints only on change.
    void setActive (bool on) { if (on != active_) { active_ = on; repaint(); } }

    void paint (juce::Graphics&) override;

    bool isClipLatched() const noexcept { return clipLatched; }   // exposed for unit tests

    // The clip indicator holds for ~1.5 s after the LAST clip, then auto-releases (re-armed on
    // every new clip). At the 30 Hz GUI tick that is ~45 ticks.
    static constexpr int kClipHoldTicks = 45;

    // The healthy capture window shown as the green target band on input meters (matches the engine's
    // kGoodLevelLinear "level low" guidance, which fires below this band).
    static constexpr float kTargetLoDb = -18.0f;
    static constexpr float kTargetHiDb = -12.0f;

private:
    juce::String label;
    bool  showTargetBand = false;   // capture meters draw the -18..-12 dBFS green target band
    bool  active_ = false;          // AutoPerEar: this is the earcup currently fed to Dirac
    float level = 0.0f;          // bar level (linear, fast attack / slow release)
    float smoothDb = -120.0f;    // slowly-smoothed dB for the READOUT so the number is readable
    bool  clipLatched = false;
    int   clipHold = 0;          // ticks remaining before the clip latch auto-releases
    juce::String lastDesc;       // last accessible description set (avoids per-frame churn)

    static float linearToFrac (float linear);   // map linear amp -> 0..1 via dBFS window
    static float dbToFrac     (float db);       // map dBFS -> 0..1 via the same display window
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace eb
