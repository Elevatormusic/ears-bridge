#include "gui/stages/StageHeader.h"
#include "gui/Theme.h"

namespace eb {

StageHeader::StageHeader (const juce::String& eyebrow, const juce::String& title,
                          const juce::String& sub, const juce::String& ctaText) {
    eyebrow_.setText (eyebrow, juce::dontSendNotification);
    title_.setText (title, juce::dontSendNotification);
    sub_.setText (sub, juce::dontSendNotification);
    sub_.setJustificationType (juce::Justification::topLeft);
    sub_.setMinimumHorizontalScale (1.0f);                    // wrap, never squish
    runNote_.setJustificationType (juce::Justification::centredRight);
    // ComponentID is owner-set (setRunNoteComponentID): this class is SHARED across stages, so a
    // hard-coded "calRunNote" here would give every stage the SAME (Calibrate-prefixed) id.
    runNote_.setMinimumHorizontalScale (1.0f);                 // never squash - let the probe catch true overflow
    cta_.setButtonText (ctaText);
    cta_.getProperties().set ("primary", true);
    cta_.setEnabled (false);                                  // owner wires enablement to Done-ness
    applyTheme();
    for (auto* l : { &eyebrow_, &title_, &sub_, &runNote_ }) addAndMakeVisible (*l);
    addAndMakeVisible (cta_);
}

void StageHeader::applyTheme() {
    // Eyebrow: textDim, not accent - light-mode accent text sits under the 4.5:1 gate floor;
    // the accessible accent-text token is P4 (M3) work.
    eyebrow_.setColour (juce::Label::textColourId, Theme::textDim());
    eyebrow_.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
    title_.setColour (juce::Label::textColourId, Theme::text());
    title_.setFont (juce::Font (juce::FontOptions (22.0f).withStyle ("Bold")));   // macOS 27 Title1 (22 w700, lh 26)
    sub_.setColour (juce::Label::textColourId, Theme::textDim());
    sub_.setFont (juce::Font (juce::FontOptions (13.0f)));
    runNote_.setColour (juce::Label::textColourId, Theme::textDim());
    runNote_.setFont (juce::Font (juce::FontOptions (11.0f)));
}

void StageHeader::setRunNote (const juce::String& s) {
    if (runNote_.getText() != s)
        runNote_.setText (s, juce::dontSendNotification);
}

void StageHeader::resized() {
    auto r = getLocalBounds();
    r.removeFromTop (12);                 // T10: frame's 26px yielded to the fold budget (ledgered deviation)
    auto cluster = r.removeFromRight (kCtaW);
    cta_.setBounds (cluster.removeFromTop (kCtaBtnH).removeFromRight (kCtaBtnW));
    cluster.removeFromTop (6);
    runNote_.setBounds (cluster.removeFromTop (kRunNoteH));
    r.removeFromRight (16);
    eyebrow_.setBounds (r.removeFromTop (16));
    r.removeFromTop (4);
    title_.setBounds (r.removeFromTop (26));   // Title1 leading
    r.removeFromTop (4);
    sub_.setBounds (r.removeFromTop (34));     // 2 lines of Body 13 (lh 16) + border
}

} // namespace eb
