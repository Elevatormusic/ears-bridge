#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/CalSlotComponent.h"

// CalibrateStage — the "Calibrate" wizard stage (P1 Task 3, spec §5.2). Hosts the two cal slots + the
// Advanced-FIR controls re-homed from the rail. Cards want the FULL stage width (as today's right pane),
// so this stage does NOT clamp to the 560 content column. Content can exceed the stage height at the
// minimum window, so the stage owns a Viewport (§4: probe geometry already clips through Viewports).
// The FIR disclosure is ALWAYS visible in P1 — the collapse toggle is Phase 2 polish.

namespace eb {

class CalibrateStage : public juce::Component {
public:
    CalibrateStage();

    // Reparent the Calibrate controls in (construction only). calEyebrow is repurposed as this stage's
    // title row (owned by MainComponent, re-styled by applyTextColours as today).
    void adopt (juce::Label& calEyebrow,
                CalSlotComponent& leftCal, CalSlotComponent& rightCal,
                juce::ToggleButton& complexPhaseToggle,
                juce::Label& firLenLabel, juce::ComboBox& firLenBox,
                juce::Label& trimLabel, juce::Slider& trimSlider);

    juce::TextButton& continueButton() { return continueButton_; }
    std::function<void()> onContinue;

    void applyTheme();   // re-colour the stage's own labels on a live light/dark flip

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    // The scrolled content (holds every child); sized to its full height so the Viewport can scroll it.
    struct Content : juce::Component {
        std::function<int(int)> layout;   // set by CalibrateStage; returns the content height for `width`
        void resized() override {}        // laid out explicitly by the parent (avoids resized() recursion)
    };
    juce::Viewport viewport_;
    Content        content_;

    juce::Label advancedFirCaption_;   // "Advanced FIR" caption the stage owns

    juce::Label*        calEyebrow_ = nullptr;
    CalSlotComponent*   leftCal_ = nullptr;
    CalSlotComponent*   rightCal_ = nullptr;
    juce::ToggleButton* complexPhaseToggle_ = nullptr;
    juce::Label*        firLenLabel_ = nullptr;
    juce::ComboBox*     firLenBox_ = nullptr;
    juce::Label*        trimLabel_ = nullptr;
    juce::Slider*       trimSlider_ = nullptr;

    juce::TextButton continueButton_ { "Continue to level" };

    int layoutContent (int width);   // top-down pass inside content_; returns the total height

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalibrateStage)
};

} // namespace eb
