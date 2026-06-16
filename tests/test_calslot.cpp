#include <catch2/catch_test_macros.hpp>
#include "gui/CalSlotComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

// Regression: a HEQ / unidentified-type cal file used to draw its warning label in the SAME bounds as
// the filename label (resized() set both to `meta`), so the two overlapped into unreadable text. Once
// a warning is visible the warning and filename rectangles must never intersect.
TEST_CASE("CalSlotComponent: type-warning label never overlaps the filename label") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Right ear");
    slot.setSize (420, 206);

    // A "house curve": valid data rows but no HPN/HEQ/RAW/IDF marker -> CalType::Unknown -> warning.
    auto tmp = juce::File::createTempFile (".txt");
    tmp.replaceWithText ("* House Curve\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (tmp));
    tmp.deleteFile();

    auto* file = slot.findChildWithID ("calFile");
    auto* warn = slot.findChildWithID ("calWarn");
    REQUIRE (file != nullptr);
    REQUIRE (warn != nullptr);
    REQUIRE (warn->isVisible());          // the unknown type surfaced the warning
    CHECK   (file->isVisible());
    CHECK_FALSE (file->getBounds().intersects (warn->getBounds()));
}
