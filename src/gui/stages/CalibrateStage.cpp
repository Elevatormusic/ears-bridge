#include "gui/stages/CalibrateStage.h"
#include "gui/Theme.h"

namespace eb {

namespace {
void styleFirCaption (juce::Label& l, const juce::String& t) {
    l.setText (t, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, Theme::textDim());
    l.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
}
constexpr int kGutter = 16;
} // namespace

CalibrateStage::CalibrateStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Calibrate");

    styleFirCaption (advancedFirCaption_, "ADVANCED FIR");

    viewport_.setViewedComponent (&content_, false);   // false: content_ is a member
    viewport_.setScrollBarsShown (true, false);         // vertical only
    viewport_.setScrollBarThickness (10);
    addAndMakeVisible (viewport_);

    content_.addAndMakeVisible (advancedFirCaption_);

    continueButton_.getProperties().set ("primary", true);
    continueButton_.setEnabled (false);   // Task 4 wires enablement + navigation
    continueButton_.onClick = [this] { if (onContinue) onContinue(); };
    addAndMakeVisible (continueButton_);
}

void CalibrateStage::applyTheme() {
    styleFirCaption (advancedFirCaption_, "ADVANCED FIR");
}

void CalibrateStage::adopt (juce::Label& calEyebrow,
                            CalSlotComponent& leftCal, CalSlotComponent& rightCal,
                            juce::ToggleButton& complexPhaseToggle,
                            juce::Label& firLenLabel, juce::ComboBox& firLenBox,
                            juce::Label& trimLabel, juce::Slider& trimSlider) {
    calEyebrow_ = &calEyebrow;               content_.addAndMakeVisible (calEyebrow);
    leftCal_ = &leftCal;                     content_.addAndMakeVisible (leftCal);
    rightCal_ = &rightCal;                   content_.addAndMakeVisible (rightCal);
    complexPhaseToggle_ = &complexPhaseToggle; content_.addAndMakeVisible (complexPhaseToggle);
    firLenLabel_ = &firLenLabel;             content_.addAndMakeVisible (firLenLabel);
    firLenBox_ = &firLenBox;                 content_.addAndMakeVisible (firLenBox);
    trimLabel_ = &trimLabel;                 content_.addAndMakeVisible (trimLabel);
    trimSlider_ = &trimSlider;               content_.addAndMakeVisible (trimSlider);
}

void CalibrateStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
}

int CalibrateStage::layoutContent (int width) {
    // Top-down pass in content_'s local space. Cards want the full width (as today's right pane), so no
    // 560 clamp here. Row heights mirror the former resized() right-pane block. Pure layout: no resized().
    auto rr = juce::Rectangle<int> (0, 0, width, 100000).reduced (kGutter, 0);
    rr.removeFromTop (kGutter);

    if (calEyebrow_) calEyebrow_->setBounds (rr.removeFromTop (16));
    rr.removeFromTop (10);

    if (leftCal_)  leftCal_->setBounds (rr.removeFromTop (206));
    rr.removeFromTop (16);
    if (rightCal_) rightCal_->setBounds (rr.removeFromTop (206));
    rr.removeFromTop (20);

    // Advanced FIR: always visible in P1 (the collapse toggle is Phase 2 polish).
    advancedFirCaption_.setBounds (rr.removeFromTop (16));
    rr.removeFromTop (6);
    if (complexPhaseToggle_) complexPhaseToggle_->setBounds (rr.removeFromTop (26));
    rr.removeFromTop (6);
    if (firLenLabel_) firLenLabel_->setBounds (rr.removeFromTop (16));
    rr.removeFromTop (6);
    if (firLenBox_)   firLenBox_->setBounds (rr.removeFromTop (40));
    rr.removeFromTop (8);
    if (trimLabel_)   trimLabel_->setBounds (rr.removeFromTop (16));
    rr.removeFromTop (4);
    if (trimSlider_)  trimSlider_->setBounds (rr.removeFromTop (28));

    return rr.getY() + kGutter;
}

void CalibrateStage::resized() {
    auto area = getLocalBounds();

    // Continue button pinned bottom-right (outside the scrolled content, always reachable).
    auto footer = area.removeFromBottom (34 + kGutter).reduced (kGutter, 0);
    footer.removeFromBottom (kGutter);
    continueButton_.setBounds (footer.removeFromRight (200));

    viewport_.setBounds (area);
    const int contentW = viewport_.getMaximumVisibleWidth();
    const int contentH = layoutContent (contentW);
    content_.setSize (contentW, juce::jmax (contentH, viewport_.getHeight()));
    // One convergence pass if adding content toggled the scrollbar (width changed).
    const int finalW = viewport_.getMaximumVisibleWidth();
    if (finalW != contentW) {
        const int h2 = layoutContent (finalW);
        content_.setSize (finalW, juce::jmax (h2, viewport_.getHeight()));
    }
}

} // namespace eb
