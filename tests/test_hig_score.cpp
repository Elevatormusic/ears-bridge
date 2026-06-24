#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/juce_design_probe.h"

// Task 1: the probe's `role` field was declared + emitted but never assigned (always ""). It is now derived
// from the component type (and overridden by the accessibility handler when a peer is attached). This test
// also doubles as the first proof that describeComponentTree works on a stock widget HEADLESS (no peer).
TEST_CASE("probe: a TextButton emits a non-empty role and a valid descriptor") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::TextButton b ("Go");
    b.setSize (80, 30);
    const auto json = hig::describeComponentTree (b);
    auto v = juce::JSON::parse (json);
    REQUIRE (v.isObject());
    auto* elements = v.getProperty ("elements", {}).getArray();
    REQUIRE (elements != nullptr);
    REQUIRE (elements->size() >= 1);
    const auto& root = elements->getReference (0);
    CHECK (root.getProperty ("type", {}).toString() == "TextButton");
    CHECK (root.getProperty ("role", {}).toString().isNotEmpty());   // was always "" before the fix
}
