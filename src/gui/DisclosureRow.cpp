#include "gui/DisclosureRow.h"
#include "gui/Theme.h"
#include <cmath>

namespace eb {

namespace {
constexpr int   kChevronGutterW = 18, kChevronGap = 6, kSummaryInsetX = 8, kTitleColCap = 200;
constexpr float kTitleFontPt = 12.5f, kSummaryFontPt = 11.5f;
} // namespace

DisclosureRow::DisclosureRow (const juce::String& title) : juce::Button (title) {
    setButtonText (title);
    setTitle (title);                    // a11y name; expanded state rides the toggle state
    setClickingTogglesState (true);
    setWantsKeyboardFocus (true);
    chevRamp_.onTick = [this] { repaint(); };   // P4: repaint only this row, only while the ramp runs
}

void DisclosureRow::setOpen (bool open) {
    if (locked_ && ! open) return;                            // a locked row may not close
    if (getToggleState() == open) return;
    setToggleState (open, juce::dontSendNotification);
    chevRamp_.start();                                        // P4: ease the chevron through the change
    if (auto* h = getAccessibilityHandler()) h->notifyAccessibilityEvent (juce::AccessibilityEvent::structureChanged);
    if (onOpenChanged) onOpenChanged (open);
}

void DisclosureRow::clicked() {
    // Button already flipped the toggle. Enforce the lock (revert silently), then report.
    // NOTE: on a real click of a LOCKED row, the toggle is momentarily false before this revert -
    // a registered Button::Listener would observe that forbidden transient. Do not attach
    // Button::Listeners to this component; onOpenChanged is the sanctioned change channel.
    if (locked_ && ! getToggleState()) {
        setToggleState (true, juce::dontSendNotification);
        return;
    }
    chevRamp_.start();                   // P4: the locked-revert above starts NO ramp (no visual change)
    if (auto* h = getAccessibilityHandler()) h->notifyAccessibilityEvent (juce::AccessibilityEvent::structureChanged);
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
    setDescription (summary_);           // AT reads the always-visible summary as the description
    repaint();
}

std::unique_ptr<juce::AccessibilityHandler> DisclosureRow::createAccessibilityHandler() {
    // P4 (HIG a11y / P2-T4 ledger): a disclosure announces EXPANDED/COLLAPSED, not checkbox on/off.
    // Replaces the toggle-button handler (setClickingTogglesState made VoiceOver read on/off) with a
    // plain button handler carrying the expandable state; press = the same lock-respecting click path.
    struct Handler : juce::AccessibilityHandler {
        explicit Handler (DisclosureRow& r)
            : juce::AccessibilityHandler (r, juce::AccessibilityRole::button,
                  juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press,
                                                          [&r] { r.clickForTest(); })),
              row (r) {}
        juce::AccessibleState getCurrentState() const override {
            const auto s = juce::AccessibilityHandler::getCurrentState().withExpandable();
            return row.isOpen() ? s.withExpanded() : s.withCollapsed();
        }
        DisclosureRow& row;
    };
    return std::make_unique<Handler> (*this);
}

// Single source for the title column width. `content` is the row space AFTER the chevron gutter
// and gap have been removed; this carves off the title column (mutating `content` to the summary
// remainder) and returns it. The column is sized to the MEASURED title (+ a little breathing room),
// capped at 200px, so a short title no longer starves the summary the way a fat fixed reservation
// did. Both paintButton and summaryAvailableWidth() route through here so their math can't drift.
juce::Rectangle<int> DisclosureRow::layoutTitleColumn (juce::Rectangle<int>& content) const {
    const juce::Font titleFont (juce::FontOptions (kTitleFontPt).withStyle ("Bold"));
    const int measured = (int) std::ceil (juce::GlyphArrangement::getStringWidth (titleFont, getButtonText())) + 6;
    const int titleW = juce::jmin (content.getWidth(), juce::jmin (measured, kTitleColCap));
    return content.removeFromLeft (titleW);
}

int DisclosureRow::summaryAvailableWidth() const {
    auto content = getLocalBounds();
    content.removeFromLeft (kChevronGutterW);                // chevron gutter
    content.removeFromLeft (kChevronGap);                    // gap after the chevron
    layoutTitleColumn (content);                             // content is now the summary remainder
    return content.reduced (kSummaryInsetX, 0).getWidth();   // matches paintButton's right-aligned inset
}

void DisclosureRow::paintButton (juce::Graphics& g, bool over, bool) {
    auto r = getLocalBounds();
    if (over) {                                              // L6-family hover affordance
        g.setColour (Theme::ctrl().withAlpha (0.5f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
    }
    if (hasKeyboardFocus (true)) {   // HIG M4: visible keyboard-focus ring
        g.setColour (Theme::accent());
        g.drawRoundedRectangle (r.toFloat().reduced (2.0f), 6.0f, 1.5f);   // P2.9: tighter inset ring on full-width rows (1.5px at inset 2)
    }
    auto chevBox = r.removeFromLeft (kChevronGutterW).toFloat().withSizeKeepingCentre (9.0f, 9.0f);
    juce::Path chev;
    chev.startNewSubPath (chevBox.getX(), chevBox.getY());
    chev.lineTo (chevBox.getRight(), chevBox.getCentreY());
    chev.lineTo (chevBox.getX(), chevBox.getBottom());
    // P4 motion: the eased amount (0 closed .. 1 open) drives the rotation; the toggle state only
    // supplies the DIRECTION. At rest this reduces to the old binary 0/90 - mid-angles exist only
    // while the ramp runs (and never under Reduce Motion, where start() snaps).
    const float amt = getToggleState() ? chevRamp_.value() : 1.0f - chevRamp_.value();
    if (amt > 0.001f)
        chev.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi * amt,
                                                              chevBox.getCentreX(), chevBox.getCentreY()));
    g.setColour (Theme::textDim());
    g.strokePath (chev, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    r.removeFromLeft (kChevronGap);                           // gap after the chevron (mirrors summaryAvailableWidth)
    auto titleArea = layoutTitleColumn (r);                  // single source: measured, capped, un-starving
    g.setColour (Theme::text());
    g.setFont (juce::Font (juce::FontOptions (kTitleFontPt).withStyle ("Bold")));
    g.drawText (getButtonText(), titleArea, juce::Justification::centredLeft);
    if (summary_.isNotEmpty()) {
        g.setColour (Theme::textDim());
        g.setFont (juce::Font (juce::FontOptions (kSummaryFontPt)));
        g.drawText (summary_, r.reduced (kSummaryInsetX, 0), juce::Justification::centredRight, true);
    }
}

} // namespace eb
