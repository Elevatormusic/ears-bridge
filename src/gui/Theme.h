#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {

// Application theme for the redesigned "Studio" UI. A LookAndFeel_V4 with a graphite
// palette (the same token set as docs/gui-redesign/prototype.html), capsule push-buttons,
// and rounded pop-up controls. Light vs dark is chosen once at startup from the system
// appearance (juce::Desktop::isDarkModeActive); the static accessors return the active
// palette so component paint code can pull named colours instead of hard-coding literals.
class Theme : public juce::LookAndFeel_V4 {
public:
    Theme();

    static bool dark();                 // active mode (system-derived)
    bool syncMode();                    // re-read the system appearance; reapply + return true if it changed
    void setDarkForTest (bool d);       // force light/dark deterministically (re-applies the palette) - design-QA gate

    // Palette (mode-aware) — matches the prototype's CSS custom properties.
    static juce::Colour bg();           // window background
    static juce::Colour surface();      // cards / monitor
    static juce::Colour rail();         // left configuration rail
    static juce::Colour barBg();        // title bar
    static juce::Colour ctrl();         // control fill (select / secondary button)
    static juce::Colour ctrlHover();
    static juce::Colour track();        // meter track
    static juce::Colour plot();         // FR plot background
    static juce::Colour grid();         // plot / control grid lines
    static juce::Colour sep();          // hairline separators
    static juce::Colour sep2();         // stronger borders
    static juce::Colour text();         // primary text
    static juce::Colour textDim();      // secondary text
    static juce::Colour textFaint();    // tertiary text
    static juce::Colour axis();         // plot axis labels
    static juce::Colour accent();       // primary blue
    static juce::Colour accentHover();
    static juce::Colour primaryFill();  // M3 (P2.9): primary-CTA FILL - W2 --accent-fill; accent() stays selection/focus
    static juce::Colour accentText();   // accent AS TEXT - 4.5:1-safe in both themes (stage eyebrows, "You are here")
    static juce::Colour ok();           // CLEAN/running status TEXT (WCAG 4.5:1-safe; use okFill for fills)
    static juce::Colour warn();         // resample/amber status TEXT (4.5:1-safe)
    static juce::Colour danger();       // clip/error status TEXT (4.5:1-safe)
    static juce::Colour okFill();       // bright green for FILLS (meter bars, grade dots) - not contrast-bound
    static juce::Colour warnFill();     // bright amber for fills
    static juce::Colour dangerFill();   // bright red for fills
    static juce::Colour onAccentText(); // text on the accent fill (white) - primary button / selection
    static juce::Colour chipBg();       // "Recommended" chip background
    static juce::Colour infoText();     // HPN badge text
    static juce::Colour infoBg();       // HPN badge background

    // Back-compat aliases used by components not rewritten in the redesign.
    static juce::Colour panel()    { return surface(); }
    static juce::Colour outline()  { return sep2(); }
    static juce::Colour meterLo()  { return ok(); }
    static juce::Colour meterMid() { return warn(); }
    static juce::Colour meterHi()  { return danger(); }

    // Capsule (pill) button background; primary buttons carry a "primary" component property.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool over, bool down) override;
    // Rounded pop-up control with a trailing chevron, matching the prototype's <select>.
    void drawComboBox (juce::Graphics&, int w, int h, bool isDown,
                       int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // P2.9: buttons opt into a leading 14px glyph via getProperties().set("glyph", "<id>")
    // (ids: play stop tick info refresh folder export warning). Glyph + 7px gap + text centred as a group (W2 .btn .ico).
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;

    // P2.9 card recipe (W2 .ear/.meters/.fr): surface fill + 1px sep() inner hairline, radius 12.
    // The single source for every stage/component card - do not hand-roll surface fills any more.
    static void paintCardSurface (juce::Graphics& g, juce::Rectangle<float> r, float radius = 12.0f);

private:
    void applyColours();   // install LnF colours for the active mode

    static bool s_dark;
};

} // namespace eb
