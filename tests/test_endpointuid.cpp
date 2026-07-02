#include <catch2/catch_test_macros.hpp>
#include "platform/EndpointUid.h"

// #28: eb::jucifyEndpointNames must reproduce EXACTLY how JUCE builds the WASAPI device-list display
// names (juce_WASAPI_windows.cpp): the default endpoint moves to index 0, then duplicates get
// appendNumbersToDuplicates(false, false) - first instance bare, later ones "Name (2)", "Name (3)".
// The UID resolver keys its map on these names, so a duplicate-named endpoint pair resolves
// positionally instead of both aliasing onto the first ("the wrong endpoint's UID").

TEST_CASE("jucifyEndpointNames: duplicates numbered exactly like JUCE (first bare, then (2), (3)) [#28]") {
    juce::StringArray names { "Speakers", "Speakers", "Mic", "Speakers" };
    const auto out = eb::jucifyEndpointNames (names, -1);
    REQUIRE (out.size() == 4);
    CHECK (out[0] == "Speakers");
    CHECK (out[1] == "Speakers (2)");
    CHECK (out[2] == "Mic");
    CHECK (out[3] == "Speakers (3)");
}

TEST_CASE("jucifyEndpointNames: the default endpoint moves to the FRONT before numbering [#28]") {
    // JUCE inserts the default device at index 0, so when duplicates exist and the DEFAULT is the
    // later one, JUCE's bare "Speakers" is the DEFAULT and the enumeration-first one becomes "(2)".
    juce::StringArray names { "Speakers", "Mic", "Speakers" };
    const auto out = eb::jucifyEndpointNames (names, /*defaultIndex*/ 2);
    REQUIRE (out.size() == 3);
    CHECK (out[0] == "Speakers");        // the default (was enumeration index 2)
    CHECK (out[1] == "Speakers (2)");    // the enumeration-first duplicate
    CHECK (out[2] == "Mic");
}

TEST_CASE("jucifyEndpointNames: no duplicates + default already first -> unchanged [#28]") {
    juce::StringArray names { "EARS", "CABLE Input" };
    const auto out = eb::jucifyEndpointNames (names, 0);
    CHECK (out[0] == "EARS");
    CHECK (out[1] == "CABLE Input");
    // defaultIndex -1 (unknown) leaves the order alone too.
    const auto out2 = eb::jucifyEndpointNames (names, -1);
    CHECK (out2[0] == "EARS");
}
