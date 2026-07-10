#include "gui/stages/LevelStage.h"
#include "gui/Copy.h"   // P4 T6: typography constants (juce_core only)
#include "gui/Theme.h"

namespace eb {
namespace {
constexpr int kGutter = 16, kContentMaxW = 560;           // the stage-column alignment system (P2.9 T10)
constexpr int kTransportH = 34, kMeterRowH = 28;
// The input-clip warning row. The production copy (~195 chars) needs THREE 12pt lines at the 560px
// column, so the plan's 32px (2 lines) would truncate the one danger surface this stage owns - the
// ledger has the slack (408 total vs the 642 budget), so the row gets the honest 3-line height.
constexpr int kClipHintH = 48;
} // namespace

juce::String LevelStage::levelRunNote (bool blocked, bool latched, const juce::String& machineReason) {
    if (blocked) return machineReason;
    if (! latched) return "Level not confirmed" + kDash + "the grade will tell you";
    return {};
}

LevelStage::LevelStage() {
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle ("Level");
    addAndMakeVisible (header_);
    header_.setRunNoteComponentID ("levelRunNote");        // unique per stage (the T7-defuse rule)
    header_.continueButton().onClick = [this] { if (onContinue) onContinue(); };
    legend_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (legend_);
    applyTheme();
}

void LevelStage::applyTheme() {
    header_.applyTheme();
    legend_.setText ("green target -18 to -12 dBFS", juce::dontSendNotification);
    // textDim, not the plan's textFaint: 11pt is normal-size text under WCAG 1.4.3, and textFaint
    // measures ~2.4:1 (the versionLabel precedent) - the 64-cell contrast gate would fail it.
    legend_.setColour (juce::Label::textColourId, Theme::textDim());
    legend_.setFont (juce::Font (juce::FontOptions (11.0f)));
}

void LevelStage::adopt (juce::TextButton& transport,
                        juce::Label& levelsEyebrow, juce::Label& levelsHint, juce::Label& diracMicGainHint,
                        LevelMeter& meterL, LevelMeter& meterR, LevelMeter& meterOut,
                        juce::Label& inputClipHint) {
    transport_ = &transport;               addAndMakeVisible (transport);
    levelsEyebrow_ = &levelsEyebrow;       addAndMakeVisible (levelsEyebrow);
    levelsHint_ = &levelsHint;             addAndMakeVisible (levelsHint);
    diracMicGainHint_ = &diracMicGainHint; addAndMakeVisible (diracMicGainHint);
    meterL_ = &meterL;                     addAndMakeVisible (meterL);
    meterR_ = &meterR;                     addAndMakeVisible (meterR);
    meterOut_ = &meterOut;                 addAndMakeVisible (meterOut);
    inputClipHint_ = &inputClipHint;       addChildComponent (inputClipHint);   // visible only on a raw clip

    int fo = 1;                                            // transport first, CTA last (§4 focus order)
    transport.setExplicitFocusOrder (fo++);
    header_.continueButton().setExplicitFocusOrder (fo++);
}

void LevelStage::paint (juce::Graphics& g) {
    g.fillAll (Theme::bg());
    if (! cardBounds_.isEmpty())
        Theme::paintCardSurface (g, cardBounds_.toFloat());
}

void LevelStage::resized() {
    auto area = getLocalBounds();
    auto headerArea = area.removeFromTop (StageHeader::kHeight);
    const int colW = juce::jmin (kContentMaxW, juce::jmax (0, area.getWidth() - 2 * kGutter));
    const int x0 = (area.getWidth() - colW) / 2;
    header_.setBounds (headerArea.withX (headerArea.getX() + x0).withWidth (colW));   // one alignment system
    auto rr = juce::Rectangle<int> (area.getX() + x0, area.getY(), colW, area.getHeight());

    if (transport_) transport_->setBounds (rr.removeFromTop (kTransportH).withWidth (200));
    rr.removeFromTop (16);

    // LEVELS card: 12 pad + 14 head + 8 + 3x28 rows + 2x8 gaps + 12 pad = 146.
    const int cardH = 12 + 14 + 8 + 3 * kMeterRowH + 2 * 8 + 12;
    cardBounds_ = rr.removeFromTop (cardH);
    auto lv = cardBounds_.reduced (16, 12);
    {   auto head = lv.removeFromTop (14);
        if (levelsEyebrow_) levelsEyebrow_->setBounds (head.removeFromLeft (70));
        legend_.setBounds (head);
    }
    lv.removeFromTop (8);
    if (meterL_)   { meterL_->setBounds (lv.removeFromTop (kMeterRowH));   lv.removeFromTop (8); }
    if (meterR_)   { meterR_->setBounds (lv.removeFromTop (kMeterRowH));   lv.removeFromTop (8); }
    if (meterOut_) meterOut_->setBounds (lv.removeFromTop (kMeterRowH));

    rr.removeFromTop (8);
    if (levelsHint_)       levelsHint_->setBounds (rr.removeFromTop (16));       // timer-owned guidance line
    rr.removeFromTop (4);
    if (diracMicGainHint_) diracMicGainHint_->setBounds (rr.removeFromTop (32));
    // The ONE conditional row = WorkflowState::ClipWarning (keep the enum in step with this branch).
    if (inputClipHint_ != nullptr) {
        if (inputClipHint_->isVisible()) { rr.removeFromTop (8); inputClipHint_->setBounds (rr.removeFromTop (kClipHintH)); }
        else inputClipHint_->setBounds ({});
    }
}

} // namespace eb
