#include "gui/stages/MeasureStage.h"
#include "gui/Theme.h"

namespace eb {

namespace {
void styleStageEyebrow (juce::Label& l, const juce::String& t) {
    l.setText (t, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, Theme::textDim());
    l.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
}
constexpr int kContentMaxW = 560;
constexpr int kGutter      = 16;
} // namespace

MeasureStage::MeasureStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Measure");
    styleStageEyebrow (eyebrow_, "MEASURE");
    addAndMakeVisible (eyebrow_);

    leadLabel_.setText ("Run the measurement in Dirac Live; EARS Bridge listens and grades each earcup.",
                        juce::dontSendNotification);
    leadLabel_.setColour (juce::Label::textColourId, Theme::textDim());
    leadLabel_.setFont (juce::Font (juce::FontOptions (12.0f)));
    leadLabel_.setJustificationType (juce::Justification::topLeft);
    leadLabel_.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (leadLabel_);
}

void MeasureStage::applyTheme() {
    styleStageEyebrow (eyebrow_, "MEASURE");
    leadLabel_.setColour (juce::Label::textColourId, Theme::textDim());
}

void MeasureStage::adopt (juce::Label& statusLine, juce::Label& statusLineR,
                          GradeMetricDotsView& gradeDotsL, GradeMetricDotsView& gradeDotsR,
                          juce::TextButton& learnRefButton, juce::Label& learnRefResultLabel,
                          juce::ToggleButton& hwDiracToggle) {
    statusLine_ = &statusLine;             addAndMakeVisible (statusLine);
    statusLineR_ = &statusLineR;           addAndMakeVisible (statusLineR);
    gradeDotsL_ = &gradeDotsL;             addAndMakeVisible (gradeDotsL);
    gradeDotsR_ = &gradeDotsR;             addAndMakeVisible (gradeDotsR);
    learnRefButton_ = &learnRefButton;     addAndMakeVisible (learnRefButton);
    learnRefResultLabel_ = &learnRefResultLabel; addAndMakeVisible (learnRefResultLabel);
    hwDiracToggle_ = &hwDiracToggle;       addAndMakeVisible (hwDiracToggle);

    // Explicit top-down focus order within this terminal stage (§4): the Learn-reference action then the
    // hardware-Dirac toggle (no Continue — Measure is terminal).
    int fo = 1;
    learnRefButton.setExplicitFocusOrder (fo++);
    hwDiracToggle.setExplicitFocusOrder  (fo++);

    // In the stage body the two status lines are left-aligned content (they were right-justified in the
    // title bar). This is layout, not new wording — the same labels, the same text.
    statusLine.setJustificationType (juce::Justification::centredLeft);
    statusLineR.setJustificationType (juce::Justification::centredLeft);
}

void MeasureStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
}

void MeasureStage::resized() {
    auto full = getLocalBounds().reduced (kGutter);
    const int colW = juce::jmin (kContentMaxW, full.getWidth());
    auto rr = full.withWidth (colW).withX (full.getX() + (full.getWidth() - colW) / 2);

    eyebrow_.setBounds (rr.removeFromTop (24));
    rr.removeFromTop (8);
    leadLabel_.setBounds (rr.removeFromTop (34));
    rr.removeFromTop (16);

    // Learn-reference flow (moved from Advanced — spec §5.4: this is roadmap #29's original ask).
    if (learnRefButton_)      learnRefButton_->setBounds (rr.removeFromTop (30).removeFromLeft (juce::jmin (260, colW)));
    rr.removeFromTop (4);
    if (learnRefResultLabel_) learnRefResultLabel_->setBounds (rr.removeFromTop (16));
    rr.removeFromTop (20);

    // Per-ear status + quality dots (the same two lines + dots the title bar used to carry).
    if (statusLine_)  statusLine_->setBounds  (rr.removeFromTop (18));
    if (gradeDotsL_)  gradeDotsL_->setBounds  (rr.removeFromTop (16));
    if (gradeDotsR_)  gradeDotsR_->setBounds  (rr.removeFromTop (16));
    if (statusLineR_) statusLineR_->setBounds (rr.removeFromTop (18));
    rr.removeFromTop (20);

    // Hardware-Dirac toggle (moved here — it modifies grading semantics, §5.4).
    if (hwDiracToggle_) hwDiracToggle_->setBounds (rr.removeFromTop (26));
}

} // namespace eb
