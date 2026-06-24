#include "gui/Theme.h"
#include "gui/SystemA11y.h"

namespace eb {

bool Theme::s_dark = true;

bool Theme::dark() { return s_dark; }

// pick(darkARGB, lightARGB) — return the active-mode colour.
static inline juce::Colour pick (juce::uint32 d, juce::uint32 l) {
    return juce::Colour (Theme::dark() ? d : l);
}

juce::Colour Theme::bg()        { return pick (0xff1E1E1E, 0xffECECEE); }
juce::Colour Theme::surface()   { return pick (0xff2A2A2A, 0xffFFFFFF); }
juce::Colour Theme::rail()      { return pick (0xff232323, 0xffF6F6F8); }
juce::Colour Theme::barBg()     { return pick (0xff262626, 0xffF6F6F8); }
juce::Colour Theme::ctrl()      { return pick (0xff38383A, 0xffFFFFFF); }
juce::Colour Theme::ctrlHover() { return pick (0xff414146, 0xffF0F0F2); }
juce::Colour Theme::track()     { return pick (0xff161616, 0xffE3E3E6); }
juce::Colour Theme::plot()      { return pick (0xff202020, 0xffF3F3F5); }
juce::Colour Theme::grid()      { return pick (0x14ffffff, 0x17000000); }
juce::Colour Theme::sep()       { const auto c = pick (0x1fffffff, 0x1f000000);   // HIG Increase Contrast: strengthen hairlines
    return SystemA11y::increaseContrast() ? c.withAlpha (juce::jmin (1.0f, c.getFloatAlpha() * 3.0f)) : c; }
juce::Colour Theme::sep2()      { const auto c = pick (0x29ffffff, 0x2e000000);
    return SystemA11y::increaseContrast() ? c.withAlpha (juce::jmin (1.0f, c.getFloatAlpha() * 3.0f)) : c; }
juce::Colour Theme::text()      { return pick (0xffffffff, 0xd9000000); }
juce::Colour Theme::textDim()   { return pick (0x9effffff, 0x99000000); }
juce::Colour Theme::textFaint() { return pick (0x4dffffff, 0x59000000); }
juce::Colour Theme::axis()      { return pick (0xff98989D, 0xff5C5C61); }
juce::Colour Theme::accent()    { return pick (0xff0091FF, 0xff0088FF); }
juce::Colour Theme::accentHover(){ return pick (0xff1F9DFF, 0xff0A7BEA); }
// HIG audit (2026-06-23): ok/warn/danger are the TEXT colours (status/warning/error labels), tuned to pass
// WCAG 4.5:1 on the ACTUAL label backgrounds in BOTH appearances (recomputed on barBg #F6F6F8 / bg #ECECEE /
// surface #2A2A2A, not just pure white). The bright originals live on as okFill/warnFill/dangerFill for meter
// bars and grade dots, which are graphical fills not bound by the 4.5:1 text rule. (Fill/text split per re-audit.)
juce::Colour Theme::ok()        { return pick (0xff30D158, 0xff157A33); }   // text green:  dark ~7.8:1, light ~5.0:1 on #F6F6F8
juce::Colour Theme::warn()      { return pick (0xffFF9230, 0xff9A4E00); }   // text amber:  dark ~6.4:1, light ~5.6:1
juce::Colour Theme::danger()    { return pick (0xffFF5C5E, 0xffC42B28); }   // text red:    dark ~4.75:1, light ~4.8:1 on #ECECEE
juce::Colour Theme::okFill()    { return pick (0xff30D158, 0xff34C759); }   // bright FILLS (meters/dots); not held to text contrast
juce::Colour Theme::warnFill()  { return pick (0xffFF9230, 0xffC2630A); }
juce::Colour Theme::dangerFill(){ return pick (0xffFF4245, 0xffD6322F); }
juce::Colour Theme::onAccentText(){ return juce::Colours::white; }          // white on the saturated accent fill (both modes)
juce::Colour Theme::chipBg()    { const auto c = pick (0x1affffff, 0x0f000000);   // HIG Reduce Transparency: firm up the tint
    return SystemA11y::reduceTransparency() ? c.withAlpha (juce::jmin (1.0f, c.getFloatAlpha() * 3.0f)) : c; }
juce::Colour Theme::infoText()  { return pick (0xff5AB7FF, 0xff0067D6); }
juce::Colour Theme::infoBg()    { const auto c = pick (0x290091FF, 0x1f0088FF);
    return SystemA11y::reduceTransparency() ? c.withAlpha (juce::jmin (1.0f, c.getFloatAlpha() * 2.0f)) : c; }

Theme::Theme() {
    s_dark = juce::Desktop::getInstance().isDarkModeActive();
    applyColours();
}

bool Theme::syncMode() {
    const bool now = juce::Desktop::getInstance().isDarkModeActive();
    if (now == s_dark) return false;
    s_dark = now;
    applyColours();
    return true;
}

void Theme::applyColours() {
    LookAndFeel_V4::ColourScheme scheme = {
        bg(), surface(), rail(), sep2(), text(), accent(), text(), accent(), text()
    };
    setColourScheme (scheme);

    setColour (juce::ResizableWindow::backgroundColourId,    bg());
    setColour (juce::Label::textColourId,                    text());
    setColour (juce::ComboBox::backgroundColourId,           ctrl());
    setColour (juce::ComboBox::textColourId,                 text());
    setColour (juce::ComboBox::outlineColourId,              sep2());
    setColour (juce::ComboBox::arrowColourId,                textDim());
    setColour (juce::PopupMenu::backgroundColourId,          surface());
    setColour (juce::PopupMenu::textColourId,                text());
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent().withAlpha (0.30f));
    setColour (juce::PopupMenu::highlightedTextColourId,     text());
    setColour (juce::TextButton::buttonColourId,             ctrl());
    setColour (juce::TextButton::buttonOnColourId,           accent());
    setColour (juce::TextButton::textColourOnId,             onAccentText());
    setColour (juce::TextButton::textColourOffId,            text());
    setColour (juce::Slider::thumbColourId,                  accent());
    setColour (juce::Slider::trackColourId,                  accent());
    setColour (juce::Slider::backgroundColourId,             track());
    setColour (juce::Slider::textBoxTextColourId,            textDim());
    setColour (juce::Slider::textBoxOutlineColourId,         juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId,             text());
    setColour (juce::ToggleButton::tickColourId,             accent());
    setColour (juce::ToggleButton::tickDisabledColourId,     sep2());
    setColour (juce::TooltipWindow::backgroundColourId,      surface());
    setColour (juce::TooltipWindow::textColourId,            text());
}

void Theme::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                  const juce::Colour& /*backgroundColour*/,
                                  bool over, bool down) {
    auto r = b.getLocalBounds().toFloat();
    const float radius = r.getHeight() * 0.5f;   // capsule
    const bool primary = (bool) b.getProperties().getWithDefault ("primary", false);

    juce::Colour fill;
    if (! b.isEnabled())   fill = primary ? ctrl() : ctrl();
    else if (primary)      fill = over ? accentHover() : accent();
    else                   fill = over ? ctrlHover()  : ctrl();
    if (down && b.isEnabled()) fill = fill.darker (0.08f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    if (! primary || ! b.isEnabled()) {
        g.setColour (sep2());
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }
    if (b.hasKeyboardFocus (true)) {   // HIG M4: visible keyboard-focus ring (white on the accent primary, accent elsewhere)
        g.setColour (primary ? onAccentText() : accent());
        g.drawRoundedRectangle (r.reduced (1.5f), radius, 2.0f);
    }
}

void Theme::drawComboBox (juce::Graphics& g, int w, int h, bool /*isDown*/,
                          int /*bx*/, int /*by*/, int /*bw*/, int /*bh*/, juce::ComboBox& box) {
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    const float radius = 8.0f;
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, radius);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, radius, 1.0f);
    if (box.hasKeyboardFocus (true)) {   // HIG M4: visible keyboard-focus ring
        g.setColour (accent());
        g.drawRoundedRectangle (r.reduced (1.0f), radius, 2.0f);
    }

    // Trailing chevron.
    juce::Path chev;
    const float cx = (float) w - 18.0f, cy = (float) h * 0.5f, s = 4.0f;
    chev.startNewSubPath (cx - s, cy - s * 0.5f);
    chev.lineTo (cx, cy + s * 0.6f);
    chev.lineTo (cx + s, cy - s * 0.5f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (chev, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}

juce::Font Theme::getComboBoxFont (juce::ComboBox&) {
    return juce::Font (juce::FontOptions (13.0f));
}

void Theme::positionComboBoxText (juce::ComboBox& box, juce::Label& label) {
    label.setBounds (12, 0, box.getWidth() - 34, box.getHeight());
    label.setFont (getComboBoxFont (box));
}

} // namespace eb
