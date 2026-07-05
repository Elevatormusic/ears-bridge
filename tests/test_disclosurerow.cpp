#include <catch2/catch_test_macros.hpp>
#include "gui/DisclosureRow.h"
#include <juce_gui_basics/juce_gui_basics.h>

// The shared disclosure header (Advanced FIR / Not using Dirac?). clickForTest is the seam
// because juce::Button::triggerClick flashes asynchronously - the seam mirrors its effect
// (flip the toggle, then clicked()) synchronously and deterministically.
TEST_CASE("DisclosureRow: collapsed by default; click toggles and reports; setOpen is idempotent") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::DisclosureRow row ("Advanced FIR");
    row.setSize (400, 28);
    int opens = 0, closes = 0;
    row.onOpenChanged = [&] (bool o) { o ? ++opens : ++closes; };
    CHECK_FALSE (row.isOpen());
    row.clickForTest();
    CHECK (row.isOpen());   CHECK (opens == 1);
    row.clickForTest();
    CHECK_FALSE (row.isOpen()); CHECK (closes == 1);
    row.setOpen (true);  CHECK (opens == 2);
    row.setOpen (true);  CHECK (opens == 2);                  // idempotent: no re-fire
    CHECK (row.getWantsKeyboardFocus());                       // keyboard-operable (H1 family)
    CHECK (row.getTitle() == "Advanced FIR");                  // a11y name
}

// Honesty lock: a section holding an ACTIVE non-default control (the enabled non-Dirac
// override) may never be collapsed out of sight.
TEST_CASE("DisclosureRow: locked row force-opens and refuses to collapse") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::DisclosureRow row ("Not using Dirac?");
    row.setSize (400, 28);
    int closes = 0;
    row.onOpenChanged = [&] (bool o) { if (! o) ++closes; };
    row.setLocked (true);
    CHECK (row.isOpen());                                      // locking opened it
    row.clickForTest();
    CHECK (row.isOpen());   CHECK (closes == 0);               // the collapse was refused silently
    row.setOpen (false);
    CHECK (row.isOpen());                                      // API path refused too
    row.setLocked (false);
    row.setOpen (false);
    CHECK_FALSE (row.isOpen());                                // unlock restores normal behavior
}
