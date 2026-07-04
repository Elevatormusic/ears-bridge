#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/LevelMeter.h"

// LevelStage — the "Level" wizard stage (P1 Task 3, spec §5.3). Hosts the three meters + the gain-
// staging captions re-homed from the right pane. It paints its own Levels card backdrop (the backdrop
// moved off MainComponent::paint into the stage). The engine transport (Start/Stop) STAYS in the title
// bar in P1 — moving it here is Phase 3. Continue = "Continue to measure".

namespace eb {

class LevelStage : public juce::Component {
public:
    LevelStage();

    void adopt (juce::Label& levelsEyebrow, juce::Label& levelsHint, juce::Label& diracMicGainHint,
                LevelMeter& meterL, LevelMeter& meterR, LevelMeter& meterOut,
                juce::Label& inputClipHint);

    juce::TextButton& continueButton() { return continueButton_; }
    std::function<void()> onContinue;

    void applyTheme();   // re-colour the stage's own labels on a live light/dark flip

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::Label eyebrow_;   // "LEVEL"
    juce::Rectangle<int> cardBounds_;   // the Levels card backdrop (drawn in paint)

    juce::Label*  levelsEyebrow_ = nullptr;
    juce::Label*  levelsHint_ = nullptr;
    juce::Label*  diracMicGainHint_ = nullptr;
    LevelMeter*   meterL_ = nullptr;
    LevelMeter*   meterR_ = nullptr;
    LevelMeter*   meterOut_ = nullptr;
    juce::Label*  inputClipHint_ = nullptr;

    juce::TextButton continueButton_ { "Continue to measure" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelStage)
};

} // namespace eb
