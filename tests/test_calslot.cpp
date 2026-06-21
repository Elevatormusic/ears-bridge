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

// Load a cal file whose CONTENT carries no side line but whose NAME marks the side -> the slot must
// fill the Unknown content side from the filename (the second signal), so the validator can catch a
// swap on a content-silent file.
TEST_CASE("CalSlotComponent: a side-silent file's side is filled from the filename") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Right ear");
    slot.setSize (420, 206);

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    auto f = dir.getChildFile ("R_HPN_000-0000.txt");        // filename marks RIGHT; content is silent on side
    f.replaceWithText ("* HPN curve\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (f));
    f.deleteFile();

    REQUIRE (slot.calFile().has_value());
    CHECK (slot.calFile()->side == eb::CalSide::Right);       // filename filled the Unknown content side
}

// When the filename and the file CONTENT disagree on the side, the CONTENT wins (it's authoritative);
// the filename conflict is recorded as a parse warning but does not flip the stored side.
TEST_CASE("CalSlotComponent: filename/content side conflict keeps the content side") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    slot.setSize (420, 206);

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    auto f = dir.getChildFile ("R_HPN_conflict_000-0000.txt");   // filename says RIGHT...
    // ...but the content explicitly says LEFT. Content must win.
    f.replaceWithText ("\"Use this file on the LEFT channel.\"\n* HPN\n"
                       "20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (f));
    f.deleteFile();

    const auto loaded = slot.calFile();                      // one copy; don't dangle a temporary in the loop
    REQUIRE (loaded.has_value());
    CHECK (loaded->side == eb::CalSide::Left);               // content authoritative
    bool sawConflict = false;
    for (const auto& w : loaded->parseWarnings)
        if (w.containsIgnoreCase ("filename")) sawConflict = true;
    CHECK (sawConflict);                                      // the disagreement is recorded
}
