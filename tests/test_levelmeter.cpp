#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "gui/LevelMeter.h"
#include "gui/SystemA11y.h"

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

TEST_CASE("LevelMeter::tagFor - worded tags, never colour alone (P3)") {
    using LM = eb::LevelMeter;
    CHECK (LM::tagFor (-15.0f, false, false) == "in band");
    CHECK (LM::tagFor (-18.0f, false, false) == "in band");   // boundary: the band is inclusive
    CHECK (LM::tagFor (-12.0f, false, false) == "in band");
    CHECK (LM::tagFor (-24.0f, false, false) == "low");
    CHECK (LM::tagFor (-80.0f, false, false) == "idle");
    CHECK (LM::tagFor (-6.0f,  false, false) == "hot");
    CHECK (LM::tagFor (-15.0f, true,  false) == "clip");      // clip beats everything
    CHECK (LM::tagFor (-15.0f, false, true)  == "to Dirac");  // the Out meter is a routing readout
    CHECK (LM::tagFor (-15.0f, true,  true)  == "clip");      // ...but a clipped Out still says clip
}

TEST_CASE("LevelMeter ballistics: Reduce Motion snaps; animated path keeps fast-attack/slow-release (P4)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::LevelMeter m ("L");
    // POSITIVE (RM OFF): release is ballistic - a drop decays by 0.80/tick, it does not snap.
    eb::SystemA11y::setForTest (false, false, false);
    m.setLevel (1.0f, false);
    m.setLevel (0.1f, false);
    CHECK (m.levelForTest() == Catch::Approx (0.80f));         // max(0.1, 1.0*0.8)
    // NEGATIVE (RM ON): the same drop SNAPS to the value - no easing under the flag.
    eb::SystemA11y::setForTest (true, false, false);
    m.setLevel (1.0f, false);
    m.setLevel (0.1f, false);
    CHECK (m.levelForTest() == Catch::Approx (0.1f));
    eb::SystemA11y::setForTest (false, false, false);
}
