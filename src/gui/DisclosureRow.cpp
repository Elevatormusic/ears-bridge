#include "gui/DisclosureRow.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

DisclosureRow::DisclosureRow (const juce::String& title) : juce::Button (title) {
    setButtonText (title);
    setTitle (title);                    // a11y name; expanded state rides the toggle state
    setClickingTogglesState (true);
    setWantsKeyboardFocus (true);
}

void DisclosureRow::setOpen (bool open) {
    if (locked_ && ! open) return;                            // a locked row may not close
    if (getToggleState() == open) return;
    setToggleState (open, juce::dontSendNotification);
    if (onOpenChanged) onOpenChanged (open);
}

void DisclosureRow::clicked() {
    // Button already flipped the toggle. Enforce the lock (revert silently), then report.
    if (locked_ && ! getToggleState()) {
        setToggleState (true, juce::dontSendNotification);
        return;
    }
    if (onOpenChanged) onOpenChanged (getToggleState());
}

void DisclosureRow::clickForTest() {
    setToggleState (! getToggleState(), juce::dontSendNotification);
    clicked();
}

void DisclosureRow::setLocked (bool locked) {
    locked_ = locked;
    if (locked_) setOpen (true);
}

void DisclosureRow::setSummary (const juce::String& s) {
    if (summary_ == s) return;
    summary_ = s;
    repaint();
}

// Single source for the title column width. `content` is the row space AFTER the chevron gutter
// and gap have been removed; this carves off the title column (mutating `content` to the summary
// remainder) and returns it. The column is sized to the MEASURED title (+ a little breathing room),
// capped at 200px, so a short title no longer starves the summary the way a fat fixed reservation
// did. Both paintButton and summaryAvailableWidth() route through here so their math can't drift.
juce::Rectangle<int> DisclosureRow::layoutTitleColumn (juce::Rectangle<int>& content) const {
    const juce::Font titleFont (juce::FontOptions (12.5f).withStyle ("Bold"));
    const int measured = (int) std::ceil (juce::GlyphArrangement::getStringWidth (titleFont, getButtonText())) + 6;
    const int titleW = juce::jmin (content.getWidth(), juce::jmin (measured, 200));
    return content.removeFromLeft (titleW);
}

int DisclosureRow::summaryAvailableWidth() const {
    auto content = getLocalBounds();
    content.removeFromLeft (18);                             // chevron gutter
    content.removeFromLeft (6);                              // gap after the chevron
    layoutTitleColumn (content);                             // content is now the summary remainder
    return content.reduced (8, 0).getWidth();                // matches paintButton's right-aligned inset
}

void DisclosureRow::paintButton (juce::Graphics& g, bool over, bool) {
    auto r = getLocalBounds();
    if (over) {                                              // L6-family hover affordance
        g.setColour (Theme::ctrl().withAlpha (0.5f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
    }
    if (hasKeyboardFocus (true)) {   // HIG M4: visible keyboard-focus ring
        g.setColour (Theme::accent());
        g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 6.0f, 2.0f);
    }
    auto chevBox = r.removeFromLeft (18).toFloat().withSizeKeepingCentre (9.0f, 9.0f);
    juce::Path chev;
    chev.startNewSubPath (chevBox.getX(), chevBox.getY());
    chev.lineTo (chevBox.getRight(), chevBox.getCentreY());
    chev.lineTo (chevBox.getX(), chevBox.getBottom());
    if (getToggleState())
        chev.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi,
                                                              chevBox.getCentreX(), chevBox.getCentreY()));
    g.setColour (Theme::textDim());
    g.strokePath (chev, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    r.removeFromLeft (6);                                     // gap after the chevron (mirrors summaryAvailableWidth)
    auto titleArea = layoutTitleColumn (r);                  // single source: measured, capped, un-starving
    g.setColour (Theme::text());
    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
    g.drawText (getButtonText(), titleArea, juce::Justification::centredLeft);
    if (summary_.isNotEmpty()) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (summary_, r.reduced (8, 0), juce::Justification::centredRight, true);
    }
}

} // namespace eb
