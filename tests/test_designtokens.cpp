#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/Theme.h"
#include "gui/juce_design_probe.h"
#include "gui/HigScore.h"

// P2.9 Task 1: the two unfrozen tokens, proven through the REAL gate path (probe -> scoreDescriptor),
// not a private re-implementation of WCAG. A Label carrying accentText on the window bg must score
// clean in BOTH themes; a deliberately-faint label on the same rig must be flagged (bite proof).
namespace {
int contrastFindings (juce::Component& rig, const juce::File& tmp) {
    auto jf = tmp.getChildFile ("t.json");
    hig::writeDesignProbe (rig, jf, tmp.getChildFile ("t.png"));
    int n = 0;
    for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
        if (f.category == "contrast") ++n;
    return n;
}
}

TEST_CASE("P2.9 tokens: accentText clears 4.5:1 on bg in both themes; the rig can bite") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::Theme theme;
    const bool wasDark = eb::Theme::dark();
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        struct Rig : juce::Component { juce::Label ok, bad; } rig;
        rig.setSize (300, 60);
        for (auto* l : { &rig.ok, &rig.bad }) {
            l->setColour (juce::Label::backgroundColourId, eb::Theme::bg());   // explicit bg: the exact scored pair
            l->setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            rig.addAndMakeVisible (*l);
        }
        rig.ok.setText ("STEP 2 OF 4", juce::dontSendNotification);
        rig.ok.setColour (juce::Label::textColourId, eb::Theme::accentText());
        rig.ok.setBounds (0, 0, 200, 16);
        rig.bad.setText ("faint", juce::dontSendNotification);
        // Bite control: an OPAQUE near-bg tone. (NOT textFaint: the probe's hexOf DROPS alpha, so a
        // translucent-white token scores as pure #FFFFFF and can never bite on a dark bg.)
        rig.bad.setColour (juce::Label::textColourId, eb::Theme::bg().brighter (0.1f));
        rig.bad.setBounds (0, 30, 200, 16);
        INFO ((dark ? "dark" : "light"));
        // Exactly ONE contrast finding: the near-bg label bites (proves the rig is live), accentText does not.
        CHECK (contrastFindings (rig, tmp) == 1);
    }
    theme.setDarkForTest (wasDark);
    tmp.deleteRecursively();
}

TEST_CASE("P2.9 tokens: primaryFill values are the W2/M3 pins") {
    eb::Theme theme;   // registers palettes
    const bool wasDark = eb::Theme::dark();
    theme.setDarkForTest (true);
    CHECK (eb::Theme::primaryFill()  == juce::Colour (0xff0A6FE0));   // W2 --accent-fill
    CHECK (eb::Theme::accentText()   == juce::Colour (0xff0091FF));   // W2 --accent as text (5.16:1 on #1E1E1E)
    CHECK (eb::Theme::accentHover()  == juce::Colour (0xff1F9DFF));   // W2 --accent-h (already pinned)
    theme.setDarkForTest (false);
    CHECK (eb::Theme::primaryFill()  == juce::Colour (0xff0088FF));   // macOS 27 System Blue light
    CHECK (eb::Theme::accentText()   == juce::Colour (0xff0067D6));   // 4.55:1 on #ECECEE
    theme.setDarkForTest (wasDark);
}
