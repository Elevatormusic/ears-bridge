#include "gui/Theme.h"

namespace eb {

Theme::Theme() {
    // ColourScheme is a NESTED type of LookAndFeel_V4 — there is no juce::ColourScheme at
    // namespace scope, so name it via the base class (the 9-colour braced init then invokes
    // the variadic ColourScheme ctor which static_asserts exactly 9 colours).
    LookAndFeel_V4::ColourScheme scheme = {
        bg(),          // windowBackground
        panel(),       // widgetBackground
        outline(),     // menuBackground
        outline(),     // outline
        text(),        // defaultText
        accent(),      // defaultFill
        text(),        // highlightedText
        accent(),      // highlightedFill
        text()         // menuText
    };
    setColourScheme (scheme);

    // Direct component-colour overrides for the controls the GUI uses.
    setColour (juce::ResizableWindow::backgroundColourId,    bg());
    setColour (juce::Label::textColourId,                    text());
    setColour (juce::ComboBox::backgroundColourId,           panel());
    setColour (juce::ComboBox::textColourId,                 text());
    setColour (juce::ComboBox::outlineColourId,              outline());
    setColour (juce::ComboBox::arrowColourId,                accent());
    setColour (juce::PopupMenu::backgroundColourId,          panel());
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent().withAlpha (0.35f));
    setColour (juce::TextButton::buttonColourId,             panel());
    setColour (juce::TextButton::buttonOnColourId,           accent());
    setColour (juce::TextButton::textColourOnId,             juce::Colours::black);
    setColour (juce::TextButton::textColourOffId,            text());
    setColour (juce::Slider::thumbColourId,                  accent());
    setColour (juce::Slider::trackColourId,                  accent().withAlpha (0.6f));
    setColour (juce::Slider::backgroundColourId,             outline());
    setColour (juce::ToggleButton::textColourId,             text());
    setColour (juce::ToggleButton::tickColourId,             accent());
    setColour (juce::TooltipWindow::backgroundColourId,      panel());
    setColour (juce::TooltipWindow::textColourId,            text());
}

} // namespace eb
