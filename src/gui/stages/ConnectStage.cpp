#include "gui/stages/ConnectStage.h"
#include "gui/Theme.h"

namespace eb {

namespace {
// Local eyebrow styling (styleEyebrow is a file-static in MainComponent.cpp; the stages need their own).
void styleStageEyebrow (juce::Label& l, const juce::String& t) {
    l.setText (t, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, Theme::textDim());
    l.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
}
constexpr int kContentMaxW = 560;   // stage content column max width, centred (mirrors the frames' rhythm)
constexpr int kGutter      = 16;
} // namespace

ConnectStage::ConnectStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Connect");
    styleStageEyebrow (eyebrow_, "CONNECT");
    content_.addAndMakeVisible (eyebrow_);

    overrideCaption_.setText ("Not using Dirac?", juce::dontSendNotification);
    overrideCaption_.setColour (juce::Label::textColourId, Theme::textDim());
    overrideCaption_.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
    content_.addAndMakeVisible (overrideCaption_);

    viewport_.setViewedComponent (&content_, false);   // false: content_ is a member
    viewport_.setScrollBarsShown (true, false);         // vertical only
    viewport_.setScrollBarThickness (10);
    addAndMakeVisible (viewport_);

    continueButton_.getProperties().set ("primary", true);
    continueButton_.setEnabled (false);   // Task 4 wires enablement + navigation
    continueButton_.onClick = [this] { if (onContinue) onContinue(); };
    addAndMakeVisible (continueButton_);
}

void ConnectStage::applyTheme() {
    styleStageEyebrow (eyebrow_, "CONNECT");
    overrideCaption_.setColour (juce::Label::textColourId, Theme::textDim());
}

void ConnectStage::adopt (DevicePicker& inputPicker, juce::Label& inputGainHint,
                          juce::Label& combineLabel, juce::ComboBox& combineBox, juce::Label& combineHint,
                          DevicePicker& outputPicker, juce::Label& outputHint,
                          juce::Label& preflightLabel, juce::Label& preflightInfo,
                          juce::Label& diracCableHint, juce::TextButton& diracFixButton,
                          juce::Label& rateLabel, juce::ComboBox& rateBox, juce::Label& rateWarn,
                          juce::Label& bitLabel, juce::ComboBox& bitBox,
                          juce::TextButton& verifyButton, juce::Label& verifyResultLabel,
                          juce::ToggleButton& overrideToggle) {
    inputPicker_ = &inputPicker;             content_.addAndMakeVisible (inputPicker);
    inputGainHint_ = &inputGainHint;         content_.addAndMakeVisible (inputGainHint);
    combineLabel_ = &combineLabel;           content_.addAndMakeVisible (combineLabel);
    combineBox_ = &combineBox;               content_.addAndMakeVisible (combineBox);
    combineHint_ = &combineHint;             content_.addAndMakeVisible (combineHint);
    outputPicker_ = &outputPicker;           content_.addAndMakeVisible (outputPicker);
    outputHint_ = &outputHint;               content_.addAndMakeVisible (outputHint);
    preflightLabel_ = &preflightLabel;       content_.addAndMakeVisible (preflightLabel);
    preflightInfo_ = &preflightInfo;         content_.addAndMakeVisible (preflightInfo);
    diracCableHint_ = &diracCableHint;       content_.addChildComponent (diracCableHint);   // shown only for a standard VB-CABLE
    diracFixButton_ = &diracFixButton;       content_.addChildComponent (diracFixButton);
    rateLabel_ = &rateLabel;                 content_.addAndMakeVisible (rateLabel);
    rateBox_ = &rateBox;                     content_.addAndMakeVisible (rateBox);
    rateWarn_ = &rateWarn;                   content_.addAndMakeVisible (rateWarn);
    bitLabel_ = &bitLabel;                   content_.addAndMakeVisible (bitLabel);
    bitBox_ = &bitBox;                       content_.addAndMakeVisible (bitBox);
    verifyButton_ = &verifyButton;           content_.addAndMakeVisible (verifyButton);       // moved from Advanced — a connection concern
    verifyResultLabel_ = &verifyResultLabel; content_.addAndMakeVisible (verifyResultLabel);
    overrideToggle_ = &overrideToggle;       content_.addAndMakeVisible (overrideToggle);     // the disclosure dies; the toggle lives on

    // Explicit top-down focus order within this stage (§4 / HIG M4). The old rail-wide orders (1..7 set in
    // MainComponent) spanned two stages once re-homed; re-scope them here so Tab walks THIS stage's controls
    // in visual order, then the Continue CTA last (set in the ctor).
    int fo = 1;
    inputPicker.setExplicitFocusOrder    (fo++);
    combineBox.setExplicitFocusOrder     (fo++);
    outputPicker.setExplicitFocusOrder   (fo++);
    diracFixButton.setExplicitFocusOrder (fo++);
    rateBox.setExplicitFocusOrder        (fo++);
    bitBox.setExplicitFocusOrder         (fo++);
    verifyButton.setExplicitFocusOrder   (fo++);
    overrideToggle.setExplicitFocusOrder (fo++);
    continueButton_.setExplicitFocusOrder (fo++);
}

void ConnectStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
}

int ConnectStage::layoutContent (int width) {
    // Centred content column (max 560), mirroring the frames' rhythm; row heights/gaps copied from the
    // former layoutRail() so the re-homed controls keep their exact metrics. Pure layout: no resized().
    const int colW = juce::jmin (kContentMaxW, juce::jmax (0, width - 2 * kGutter));
    const int x0   = (width - colW) / 2;
    auto rr = juce::Rectangle<int> (x0, 0, colW, 100000);
    rr.removeFromTop (kGutter);

    eyebrow_.setBounds (rr.removeFromTop (24));
    rr.removeFromTop (10);

    if (inputPicker_)   inputPicker_->setBounds (rr.removeFromTop (62));
    rr.removeFromTop (4);
    if (inputGainHint_) inputGainHint_->setBounds (rr.removeFromTop (60));
    rr.removeFromTop (12);

    if (combineLabel_) combineLabel_->setBounds (rr.removeFromTop (16));
    rr.removeFromTop (6);
    if (combineBox_)   combineBox_->setBounds (rr.removeFromTop (40));
    rr.removeFromTop (6);
    if (combineHint_)  combineHint_->setBounds (rr.removeFromTop (80));
    rr.removeFromTop (16);

    if (outputPicker_) outputPicker_->setBounds (rr.removeFromTop (62));
    rr.removeFromTop (4);
    if (outputHint_)   outputHint_->setBounds (rr.removeFromTop (30));
    if (preflightLabel_) preflightLabel_->setBounds (rr.removeFromTop (14));
    if (preflightInfo_) {
        if (preflightInfo_->getText().isNotEmpty()) preflightInfo_->setBounds (rr.removeFromTop (14));
        else                                        preflightInfo_->setBounds ({});
    }
    if (diracCableHint_ && diracCableHint_->isVisible()) {
        rr.removeFromTop (6);
        diracCableHint_->setBounds (rr.removeFromTop (48));
        if (diracFixButton_ && diracFixButton_->isVisible()) {
            rr.removeFromTop (4);
            diracFixButton_->setBounds (rr.removeFromTop (30).removeFromLeft (200));
        }
    }
    rr.removeFromTop (12);

    {
        auto rb = rr.removeFromTop (62);
        auto rcol = rb.removeFromLeft (rb.getWidth() / 2 - 8);
        rb.removeFromLeft (16);
        if (rateLabel_) rateLabel_->setBounds (rcol.removeFromTop (16));
        rcol.removeFromTop (6);
        if (rateBox_)   rateBox_->setBounds (rcol.removeFromTop (40));
        if (bitLabel_)  bitLabel_->setBounds (rb.removeFromTop (16));
        rb.removeFromTop (6);
        if (bitBox_)    bitBox_->setBounds (rb.removeFromTop (40));
    }
    if (rateWarn_) rateWarn_->setBounds (rr.removeFromTop (14));
    rr.removeFromTop (12);

    // L/R wiring check (moved here from Advanced — it is a connection concern, §5.1).
    if (verifyButton_)      verifyButton_->setBounds (rr.removeFromTop (30).removeFromLeft (juce::jmin (220, colW)));
    rr.removeFromTop (4);
    if (verifyResultLabel_) verifyResultLabel_->setBounds (rr.removeFromTop (16));
    rr.removeFromTop (16);

    // "Not using Dirac?" escape hatch: caption + the override toggle beneath it.
    overrideCaption_.setBounds (rr.removeFromTop (16));
    rr.removeFromTop (4);
    if (overrideToggle_) overrideToggle_->setBounds (rr.removeFromTop (26));

    return rr.getY() + kGutter;
}

void ConnectStage::resized() {
    auto area = getLocalBounds();

    // Continue button pinned bottom-right (outside the scrolled content, always reachable).
    auto footer = area.removeFromBottom (34 + kGutter).reduced (kGutter, 0);
    footer.removeFromBottom (kGutter);
    continueButton_.setBounds (footer.removeFromRight (200));

    viewport_.setBounds (area);
    const int contentW = viewport_.getMaximumVisibleWidth();
    const int contentH = layoutContent (contentW);
    content_.setSize (contentW, juce::jmax (contentH, viewport_.getHeight()));
    const int finalW = viewport_.getMaximumVisibleWidth();
    if (finalW != contentW) {
        const int h2 = layoutContent (finalW);
        content_.setSize (finalW, juce::jmax (h2, viewport_.getHeight()));
    }
}

} // namespace eb
