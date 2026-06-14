#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {

// Dark application theme. Subclasses LookAndFeel_V4 with a custom dark ColourScheme
// plus EARS-Bridge brand colours, and exposes named accents for component code to
// pull (meters, badges) instead of hard-coding literals.
class Theme : public juce::LookAndFeel_V4 {
public:
    Theme();

    // Brand palette (also installed into the ColourScheme where applicable).
    static juce::Colour bg()        { return juce::Colour (0xff141619); } // window
    static juce::Colour panel()     { return juce::Colour (0xff1d2024); } // cards/slots
    static juce::Colour outline()   { return juce::Colour (0xff2c3036); }
    static juce::Colour text()      { return juce::Colour (0xffe6e8ea); }
    static juce::Colour textDim()   { return juce::Colour (0xff9aa0a6); }
    static juce::Colour accent()    { return juce::Colour (0xff4ea1ff); } // primary blue
    static juce::Colour ok()        { return juce::Colour (0xff39c07a); } // running / clean
    static juce::Colour warn()      { return juce::Colour (0xfff0a93b); } // resample / HEQ badge
    static juce::Colour danger()    { return juce::Colour (0xffe5484d); } // clip / error
    static juce::Colour meterLo()   { return juce::Colour (0xff39c07a); } // meter green zone
    static juce::Colour meterMid()  { return juce::Colour (0xffe0c43a); } // meter yellow zone
    static juce::Colour meterHi()   { return juce::Colour (0xffe5484d); } // meter red zone
};

} // namespace eb
