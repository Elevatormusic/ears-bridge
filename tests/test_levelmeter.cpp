#include <catch2/catch_test_macros.hpp>
#include "gui/LevelMeter.h"

// Regression for the field bug "clip never resets / levels red instead of green": the clip latch
// used to clear ONLY on full silence, so a single transient clip left the readout stuck on "CLIP"
// and the bar stuck red (LevelMeter paints danger() whenever clipLatched) for the whole session.
// The latch must auto-release after a hold window once clipping stops.

TEST_CASE("LevelMeter: a transient clip auto-releases after the hold window") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::LevelMeter m;

    m.setLevel (1.0f, true);                 // one clip arms the latch
    CHECK (m.isClipLatched());

    for (int i = 0; i < eb::LevelMeter::kClipHoldTicks - 1; ++i)
        m.setLevel (0.2f, false);            // healthy signal (~ -14 dBFS), no clip
    CHECK (m.isClipLatched());               // still held just before the window elapses

    m.setLevel (0.2f, false);                // the tick that elapses the hold
    CHECK_FALSE (m.isClipLatched());         // released: "CLIP" clears, bar reverts to green
}

TEST_CASE("LevelMeter: continuous clipping stays latched, then releases once it stops") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::LevelMeter m;

    for (int i = 0; i < eb::LevelMeter::kClipHoldTicks * 3; ++i)
        m.setLevel (1.0f, true);             // genuinely hot every block -> correctly stays latched
    CHECK (m.isClipLatched());

    for (int i = 0; i < eb::LevelMeter::kClipHoldTicks; ++i)
        m.setLevel (0.2f, false);            // clipping stops
    CHECK_FALSE (m.isClipLatched());         // auto-released within the hold window
}
