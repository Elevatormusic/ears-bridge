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
    static juce::Colour ok();           // running / clean / meter green
    static juce::Colour warn();         // resample / amber zone
    static juce::Colour danger();       // clip / error / red zone
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

private:
    void applyColours();   // install LnF colours for the active mode

    static bool s_dark;
};

} // namespace eb
