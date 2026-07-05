#include <catch2/catch_test_macros.hpp>
#include "gui/CurveThumbnail.h"
#include "cal/CalFile.h"
#include <juce_gui_basics/juce_gui_basics.h>

static eb::CalFile parseSmallCal() {
    // |extreme| = 6 dB -> autoFit = ceil((6+3)/6)*6 = 12
    return eb::CalFile::parse ("* HEQ\n20 0.0\n100 1.0\n1000 6.0\n10000 -3.0\n20000 -6.0\n");
}

// M5/L1: the FR plot is an IMAGE to assistive tech - it must expose the image role, a name,
// and a live description of what it shows (range + point count), not sit as an unnamed blob.
TEST_CASE("CurveThumbnail: image a11y role, title, live range description") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CurveThumbnail th;
    th.setSize (210, 84);
    // getAccessibilityHandler() returns null until the component has a native window
    // handle (JUCE gates it on getWindowHandle()); give it one for the role assertion.
    th.addToDesktop (0);
    CHECK (th.getTitle().isNotEmpty());
    auto* h = th.getAccessibilityHandler();
    REQUIRE (h != nullptr);
    CHECK (h->getRole() == juce::AccessibilityRole::image);
    CHECK (th.getDescription() == "No calibration loaded");
    th.setCalFile (parseSmallCal());
    CHECK (th.getDescription().contains ("12 dB"));      // range is in the description
    th.clear();
    CHECK (th.getDescription() == "No calibration loaded");
    th.removeFromDesktop();
}

// L9: labelled axes paint (smoke: deterministic range + no crash empty/loaded; the pixel
// truth is Task 9's real-render check + the loaded-card gate case in Task 8).
TEST_CASE("CurveThumbnail: paints empty and loaded with the fitted range") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CurveThumbnail th;
    th.setSize (210, 84);
    juce::Image img (juce::Image::ARGB, 210, 84, true);
    juce::Graphics g (img);
    th.paint (g);                          // empty: placeholder, no axis labels
    th.setCalFile (parseSmallCal());
    CHECK (th.autoFitTopDb() == 12.0f);
    th.paint (g);                          // loaded: curve + dB + Hz labels
}
