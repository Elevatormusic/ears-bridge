#include "gui/stages/LevelStage.h"
#include "gui/Theme.h"

namespace eb {

namespace {
void styleStageEyebrow (juce::Label& l, const juce::String& t) {
    l.setText (t, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, Theme::accentText());   // P2.9: accent anchor (4.5:1-safe both themes)
    l.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
}
constexpr int kContentMaxW = 560;
constexpr int kGutter      = 16;
} // namespace

LevelStage::LevelStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Level");
    styleStageEyebrow (eyebrow_, "LEVEL");
    addAndMakeVisible (eyebrow_);

    continueButton_.getProperties().set ("primary", true);
    continueButton_.setEnabled (false);   // Task 4 wires enablement + navigation
    continueButton_.onClick = [this] { if (onContinue) onContinue(); };
    addAndMakeVisible (continueButton_);
}

void LevelStage::applyTheme() {
    styleStageEyebrow (eyebrow_, "LEVEL");
}

void LevelStage::adopt (juce::Label& levelsEyebrow, juce::Label& levelsHint, juce::Label& diracMicGainHint,
                        LevelMeter& meterL, LevelMeter& meterR, LevelMeter& meterOut,
                        juce::Label& inputClipHint) {
    levelsEyebrow_ = &levelsEyebrow;     addAndMakeVisible (levelsEyebrow);
    levelsHint_ = &levelsHint;           addAndMakeVisible (levelsHint);
    diracMicGainHint_ = &diracMicGainHint; addAndMakeVisible (diracMicGainHint);
    meterL_ = &meterL;                   addAndMakeVisible (meterL);
    meterR_ = &meterR;                   addAndMakeVisible (meterR);
    meterOut_ = &meterOut;               addAndMakeVisible (meterOut);
    inputClipHint_ = &inputClipHint;     addChildComponent (inputClipHint);   // hidden until a raw-input clip

    // The meters are non-interactive; the only focusable control this stage owns is the Continue CTA (§4).
    continueButton_.setExplicitFocusOrder (1);
}

void LevelStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
    if (! cardBounds_.isEmpty())
        Theme::paintCardSurface (g, cardBounds_.toFloat());    // shared recipe flows through (content untouched)
}

void LevelStage::resized() {
    auto full = getLocalBounds().reduced (kGutter);
    const int colW = juce::jmin (kContentMaxW, full.getWidth());
    auto pp = full.withWidth (colW).withX (full.getX() + (full.getWidth() - colW) / 2);

    eyebrow_.setBounds (pp.removeFromTop (24));
    pp.removeFromTop (10);

    // Continue at the bottom-right, reserved first.
    auto footer = pp.removeFromBottom (34);
    continueButton_.setBounds (footer.removeFromRight (200));
    pp.removeFromBottom (16);

    // Reserve the bottom captions from the bottom so the card above can't clip them (mirrors the old
    // right-pane order).
    if (inputClipHint_ && inputClipHint_->isVisible()) {
        inputClipHint_->setBounds (pp.removeFromBottom (50));
        pp.removeFromBottom (8);
    }
    if (diracMicGainHint_) { diracMicGainHint_->setBounds (pp.removeFromBottom (34)); pp.removeFromBottom (8); }

    // Levels card fills the remaining space (cap it so a tall window doesn't stretch the meters absurdly).
    cardBounds_ = pp.removeFromTop (juce::jmin (pp.getHeight(), 160));
    auto lv = cardBounds_.reduced (16, 12);
    {
        auto top = lv.removeFromTop (14);
        if (levelsEyebrow_) levelsEyebrow_->setBounds (top.removeFromLeft (60));
        top.removeFromLeft (10);
        if (levelsHint_)    levelsHint_->setBounds (top);
    }
    lv.removeFromTop (8);
    const int mh = juce::jmax (8, lv.getHeight() / 3);
    if (meterL_)   meterL_->setBounds   (lv.removeFromTop (mh));
    if (meterR_)   meterR_->setBounds   (lv.removeFromTop (mh));
    if (meterOut_) meterOut_->setBounds (lv.removeFromTop (mh));
}

} // namespace eb
