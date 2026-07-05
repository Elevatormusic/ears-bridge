#include "gui/DisclosureRow.h"
#include "gui/Theme.h"

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

void DisclosureRow::paintButton (juce::Graphics& g, bool over, bool) {
    auto r = getLocalBounds();
    if (over || hasKeyboardFocus (true)) {                    // L6-family hover/focus affordance
        g.setColour (Theme::ctrl().withAlpha (0.5f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
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
    r.removeFromLeft (6);
    g.setColour (Theme::text());
    g.setFont (juce::Font (juce::FontOptions (12.5f).withStyle ("Bold")));
    g.drawText (getButtonText(), r.removeFromLeft (juce::jmin (r.getWidth(), 200)),
                juce::Justification::centredLeft);
    if (summary_.isNotEmpty()) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (summary_, r.reduced (8, 0), juce::Justification::centredRight, true);
    }
}

} // namespace eb
