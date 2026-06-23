#include <catch2/catch_test_macros.hpp>
#include "audio/HardwareDiracDetect.h"

// Pure hardware-Dirac detection. The box generates its sweep INTERNALLY, so it reaches the EARS mic but
// never appears as a PC render stream. We detect that BEHAVIOR (no device names).

TEST_CASE ("outputRendered: real render vs silent vs unknown", "[hwdirac]") {
    CHECK      (eb::outputRendered (0.05f));     // a real PC render stream
    CHECK_FALSE(eb::outputRendered (0.0f));      // silent
    CHECK_FALSE(eb::outputRendered (0.001f));    // below the 0.0015 floor (idle dither)
    CHECK_FALSE(eb::outputRendered (-1.0f));     // the "unknown" sentinel -> NOT a render
}

TEST_CASE ("sweepWasInternal truth table: only mic && readable && !rendered && validMode", "[hwdirac]") {
    CHECK      (eb::sweepWasInternal (true,  true,  false, true));   // the hardware-Dirac signature
    CHECK_FALSE(eb::sweepWasInternal (false, true,  false, true));   // no mic sweep
    CHECK_FALSE(eb::sweepWasInternal (true,  false, false, true));   // UNREADABLE output -> never auto-suggest
    CHECK_FALSE(eb::sweepWasInternal (true,  true,  true,  true));   // output DID render (software Dirac)
    CHECK_FALSE(eb::sweepWasInternal (true,  true,  false, false));  // not a valid Windows-Audio mode
}

TEST_CASE ("hardware-Dirac copy is honest", "[hwdirac]") {
    const juce::String sugg = eb::hardwareDiracSuggestion();
    CHECK (sugg.containsIgnoreCase ("hardware"));
    CHECK (sugg.containsIgnoreCase ("Advanced"));          // points the user to the toggle
    const juce::String off = eb::hardwareDiracGradingOff();
    CHECK (off.containsIgnoreCase ("calibration"));        // per-ear calibration still active
}
