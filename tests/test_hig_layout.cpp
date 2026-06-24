#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"

// Task 6: the real editor must construct HEADLESS (no window/peer) with a temp-dir Settings (hermetic - never
// touches the developer's/CI's real %APPDATA% file) and with the launch-time GitHub update check SUPPRESSED.
// resized() is a peer-free top-down pass, so setSize() lays out every child offscreen - the basis of the gate.
TEST_CASE("MainComponent constructs headless with a temp-dir Settings and no network") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile ("");
    tmp.createDirectory();

    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, /*disableNetwork*/ true });
    mc.setSize (900, 780);
    CHECK (mc.getWidth() == 900);
    CHECK (mc.getNumChildComponents() > 0);   // children laid out, no peer needed

    tmp.deleteRecursively();
}
