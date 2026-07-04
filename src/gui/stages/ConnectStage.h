#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/DevicePicker.h"

// ConnectStage — the "Connect" wizard stage (P1 Task 3). Hosts the device/format controls re-homed
// from the old left rail (spec §5.1). It OWNS only its own header labels + the "Not using Dirac?"
// caption + the disabled Continue button; every functional control is a MainComponent member adopted
// in via adopt() (construct-once/reparent-once). The Connect column is TALL (it holds most of the old
// rail), so — like CalibrateStage — the content lives in a Viewport and scrolls rather than collapsing
// its bottom controls to zero height at the minimum window (spec §4).

namespace eb {

class ConnectStage : public juce::Component {
public:
    ConnectStage();

    // Reparent the Connect controls in (construction only). References stay owned by MainComponent.
    void adopt (DevicePicker& inputPicker, juce::Label& inputGainHint,
                juce::Label& combineLabel, juce::ComboBox& combineBox, juce::Label& combineHint,
                DevicePicker& outputPicker, juce::Label& outputHint,
                juce::Label& preflightLabel, juce::Label& preflightInfo,
                juce::Label& diracCableHint, juce::TextButton& diracFixButton,
                juce::Label& rateLabel, juce::ComboBox& rateBox, juce::Label& rateWarn,
                juce::Label& bitLabel, juce::ComboBox& bitBox,
                juce::TextButton& verifyButton, juce::Label& verifyResultLabel,
                juce::ToggleButton& overrideToggle);

    juce::TextButton& continueButton() { return continueButton_; }
    std::function<void()> onContinue;

    // Re-apply theme-dependent colours to the stage's OWN labels on a live light/dark flip (MainComponent
    // calls this from applyTextColours; the adopted controls re-colour through their own applyTheme()).
    void applyTheme();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Content : juce::Component { void resized() override {} };   // laid out explicitly by the parent
    juce::Viewport viewport_;
    Content        content_;

    juce::Label eyebrow_;          // "CONNECT"
    juce::Label overrideCaption_;  // "Not using Dirac?"

    // Adopted (owned by MainComponent) — held as pointers for layout only.
    DevicePicker*    inputPicker_ = nullptr;
    juce::Label*     inputGainHint_ = nullptr;
    juce::Label*     combineLabel_ = nullptr;
    juce::ComboBox*  combineBox_ = nullptr;
    juce::Label*     combineHint_ = nullptr;
    DevicePicker*    outputPicker_ = nullptr;
    juce::Label*     outputHint_ = nullptr;
    juce::Label*     preflightLabel_ = nullptr;
    juce::Label*     preflightInfo_ = nullptr;
    juce::Label*     diracCableHint_ = nullptr;
    juce::TextButton* diracFixButton_ = nullptr;
    juce::Label*     rateLabel_ = nullptr;
    juce::ComboBox*  rateBox_ = nullptr;
    juce::Label*     rateWarn_ = nullptr;
    juce::Label*     bitLabel_ = nullptr;
    juce::ComboBox*  bitBox_ = nullptr;
    juce::TextButton* verifyButton_ = nullptr;
    juce::Label*     verifyResultLabel_ = nullptr;
    juce::ToggleButton* overrideToggle_ = nullptr;

    juce::TextButton continueButton_ { "Continue to calibration" };

    int layoutContent (int width);   // top-down pass inside content_; returns the total height

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConnectStage)
};

} // namespace eb
