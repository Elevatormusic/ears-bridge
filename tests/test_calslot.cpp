#include <catch2/catch_test_macros.hpp>
#include "gui/CalSlotComponent.h"
#include "gui/juce_design_probe.h"
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

// #38: a persisted cal path that has gone STALE (file moved/deleted) must leave the user a way to CLEAR it -
// the Remove button must be visible on the error card (it used to exist only on a loaded card, so the error
// re-appeared every launch with Start locked closed and no affordance to unstick it).
TEST_CASE("CalSlotComponent: a failed load from a stale path shows the Remove (clear) button") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    slot.setSize (420, 206);

    const juce::File gone ("Z:/definitely/not/here/L_HEQ_000-0000.txt");
    REQUIRE_FALSE (slot.loadFromFile (gone));

    auto* warn = slot.findChildWithID ("calWarn");
    REQUIRE (warn != nullptr);
    CHECK (warn->isVisible());                       // the error is surfaced...
    bool sawVisibleButton = false;                    // ...and a Remove button is reachable to clear the path
    for (int i = 0; i < slot.getNumChildComponents(); ++i)
        if (auto* b = dynamic_cast<juce::TextButton*> (slot.getChildComponent (i)))
            if (b->getButtonText() == "Remove" && b->isVisible() && ! b->getBounds().isEmpty())
                sawVisibleButton = true;
    CHECK (sawVisibleButton);
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

// ---- P2 empty-state redesign (spec 5.2 + the firstrun frame) ------------------------------------

// H1: the empty slot must be operable WITHOUT drag-and-drop or a pointer - a real, visible,
// focusable Browse button (the old empty card was click-to-browse only: invisible to keyboard).
TEST_CASE("CalSlotComponent empty state: visible, focusable, per-ear Browse button [H1]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    slot.setSize (289, slot.preferredHeight());
    juce::TextButton* browse = nullptr;
    for (int i = 0; i < slot.getNumChildComponents(); ++i)
        if (auto* b = dynamic_cast<juce::TextButton*> (slot.getChildComponent (i)))
            if (b->getButtonText().startsWith ("Browse")) browse = b;
    REQUIRE (browse != nullptr);
    CHECK (browse->isVisible());
    CHECK (! browse->getBounds().isEmpty());
    CHECK (browse->getWantsKeyboardFocus());
    // Per-ear text: distinct labels are honest a11y (a screen reader hears WHICH ear) and the
    // design gate's duplicate rule would rightly flag two identical "Browse..." twins.
    CHECK (browse->getButtonText() == "Browse left ear...");
}

// Map #8b: the Required line exists ONLY in the honest asymmetric case (the OTHER ear loaded ->
// Start is truly blocked until this one loads). Both-empty shows no per-card nudge (the
// stage-level unity hint owns that, Task 6) - and the line cannot stick.
TEST_CASE("CalSlotComponent empty state: Required line tracks the sibling, with a negative") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Right ear");
    slot.setSize (289, slot.preferredHeight());
    auto* req = slot.findChildWithID ("calDzReq");
    REQUIRE (req != nullptr);
    CHECK_FALSE (req->isVisible());                 // both empty: no per-card requirement claim
    slot.setSiblingLoaded (true);
    CHECK (req->isVisible());
    slot.setSiblingLoaded (false);
    CHECK_FALSE (req->isVisible());                 // NEGATIVE: it does not false-fire/stick
}

// The drop-zone affordances are EMPTY-state only; Remove returns the card to the drop zone.
TEST_CASE("CalSlotComponent: loading hides the drop-zone affordances; clearing restores them") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    slot.setSize (289, slot.preferredHeight());
    auto tmp = juce::File::createTempFile (".txt");
    tmp.replaceWithText ("* HEQ\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (tmp));
    tmp.deleteFile();
    CHECK_FALSE (slot.findChildWithID ("calDzMain")->isVisible());
    slot.clearCal();
    CHECK (slot.findChildWithID ("calDzMain")->isVisible());
}

// setProblem changes the card's height inputs (preferredHeight() counts problemLabel), so it must
// fire the onLayoutChanged seam the host stage grid listens on - or a swap banner appearing via the
// per-tick gate refresh would silently desync the layout. The no-op guard must still hold: the same
// text must NOT re-fire. clearing (empty string) fires once more (the banner row goes away).
TEST_CASE("CalSlotComponent: setProblem fires onLayoutChanged on a real change, not on a repeat") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    slot.setSize (289, slot.preferredHeight());
    int fires = 0;
    slot.onLayoutChanged = [&fires] { ++fires; };

    slot.setProblem ("swap suspected");     // clean -> problem: a real height-input change
    CHECK (fires == 1);
    slot.setProblem ("swap suspected");     // same text: the no-op guard must suppress a re-fire
    CHECK (fires == 1);
    slot.setProblem (juce::String());       // clear: the banner row goes away -> another real change
    CHECK (fires == 2);
}

// The redesigned empty card at the MINIMUM grid cell width (289px at the 900px window):
// no label may clip (probe-verified).
TEST_CASE("CalSlotComponent empty state: probe-clean at the minimum cell width") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Right ear");
    slot.setSiblingLoaded (true);                    // worst case: Required line present
    slot.setSize (289, slot.preferredHeight());
    const auto tree = juce::JSON::parse (hig::describeComponentTree (slot));
    const auto* els = tree.getProperty ("elements", {}).getArray();
    REQUIRE (els != nullptr);
    for (auto& e : *els)
        if ((bool) e.getProperty ("showing", false))
            CHECK_FALSE ((bool) e.getProperty ("textOverflows", false));
}

// ---- P2 loaded-card redesign --------------------------------------------------------------------

// Map #4/#5: HEQ/HPN per-TYPE guidance moved to the ONE stage caption (it rendered once per
// card - twice for a pair). The card's calWarn stays free for real per-file cautions.
TEST_CASE("CalSlotComponent loaded card: HEQ and HPN guidance no longer occupy the card") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (const char* marker : { "* HEQ", "* HPN curve" }) {
        eb::CalSlotComponent slot ("Left ear");
        slot.setSize (289, 300);
        auto tmp = juce::File::createTempFile (".txt");
        tmp.replaceWithText (juce::String (marker) + "\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
        REQUIRE (slot.loadFromFile (tmp));
        tmp.deleteFile();
        CHECK_FALSE (slot.findChildWithID ("calWarn")->isVisible());
        CHECK (slot.findChildWithID ("calSerial")->isVisible());
        CHECK (slot.findChildWithID ("calFile")->isVisible());
    }
}

// Map #1/#2 + the NEGATIVE: the swap banner still renders on the redesigned card - and a clean
// load shows NO banner (the problem surface cannot false-fire).
TEST_CASE("CalSlotComponent: problem banner on the redesigned card, absent on a clean card") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    auto tmp = juce::File::createTempFile (".txt");
    tmp.replaceWithText ("* HEQ\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (tmp));
    tmp.deleteFile();
    auto* problem = slot.findChildWithID ("calProblem");
    REQUIRE (problem != nullptr);
    CHECK_FALSE (problem->isVisible());                       // NEGATIVE half
    slot.setProblem ("This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.");
    slot.setSize (289, slot.preferredHeight());
    CHECK (problem->isVisible());
    CHECK_FALSE (problem->getBounds().intersects (slot.findChildWithID ("calFile")->getBounds()));
    slot.setProblem ({});
    CHECK_FALSE (problem->isVisible());
}

// The stage re-layout hook: every height-changing state flip announces itself exactly once
// (setProblem's set-if-changed guard also suppresses the no-op repeat).
TEST_CASE("CalSlotComponent: onLayoutChanged fires on load, problem flips and clear - not on no-ops") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Right ear");
    slot.setSize (289, slot.preferredHeight());
    int fired = 0;
    slot.onLayoutChanged = [&] { ++fired; };
    auto tmp = juce::File::createTempFile (".txt");
    tmp.replaceWithText ("* HEQ\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (tmp));
    tmp.deleteFile();
    CHECK (fired == 1);
    slot.setProblem ("swap");   CHECK (fired == 2);
    slot.setProblem ("swap");   CHECK (fired == 2);           // no-op guard
    slot.setProblem ({});       CHECK (fired == 3);
    slot.clearCal();            CHECK (fired == 4);
}

// Worst-case loaded card (Unknown-type caution + swap banner) probe-clean at the minimum cell
// width: nothing clips, and the four text rows occupy disjoint rectangles.
TEST_CASE("CalSlotComponent loaded card: worst case probe-clean at the minimum cell width") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::CalSlotComponent slot ("Left ear");
    auto tmp = juce::File::createTempFile (".txt");
    tmp.replaceWithText ("* House Curve\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (slot.loadFromFile (tmp));                        // Unknown type -> on-card caution
    tmp.deleteFile();
    slot.setProblem ("This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.");
    slot.setSize (289, slot.preferredHeight());
    const auto tree = juce::JSON::parse (hig::describeComponentTree (slot));
    const auto* els = tree.getProperty ("elements", {}).getArray();
    REQUIRE (els != nullptr);
    for (auto& e : *els)
        if ((bool) e.getProperty ("showing", false))
            CHECK_FALSE ((bool) e.getProperty ("textOverflows", false));
    const char* ids[] = { "calProblem", "calWarn", "calFile", "calSerial" };
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) {
            auto* a = slot.findChildWithID (ids[i]);
            auto* b = slot.findChildWithID (ids[j]);
            REQUIRE (a != nullptr); REQUIRE (b != nullptr);
            if (a->isVisible() && b->isVisible())
                CHECK_FALSE (a->getBounds().intersects (b->getBounds()));
        }
}
