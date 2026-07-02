#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"
#include "gui/juce_design_probe.h"
#include "gui/HigScore.h"
#include "gui/SystemA11y.h"
#include "gui/StatusLadder.h"   // #68: worst-case captured wording generated FROM the pure ladder

// Task 6: the real editor must construct HEADLESS (no window/peer) with a temp-dir Settings (hermetic - never
// touches the developer's/CI's real %APPDATA% file) and with the launch-time GitHub update check SUPPRESSED.
// resized() is a peer-free top-down pass, so setSize() lays out every child offscreen - the basis of the gate.
TEST_CASE("MainComponent constructs headless with a temp-dir Settings and no network") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile ("");
    tmp.createDirectory();

    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, /*disableNetwork*/ true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    CHECK (mc.getWidth() == 900);
    CHECK (mc.getNumChildComponents() > 0);   // children laid out, no peer needed

    tmp.deleteRecursively();
}

// Task 7: THE BUILD GATE. Construct the real editor headless and score its STOCK-WIDGET subset across the cross-
// cutting axes (light/dark x the 3 a11y modes x min/large size), failing on any overlap/clip/duplicate/sub-24px/
// contrast finding. Custom-painted nodes are measurable:false -> not scored here (Phase 2 instruments them). The
// a11y/theme states are FORCED (the OS reads are nondeterministic in CI). Phase-1 deliverable.
TEST_CASE("HIG gate: the real editor has no blocking layout finding in any theme/a11y/size") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    // #24: fully hermetic - Settings, the reference store AND the log dir all point at the temp dir, so the
    // scored layout can never depend on this machine's real %APPDATA% reference files or write real logs.
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const bool wasDark = eb::Theme::dark();   // restore the static mode at the end (no cross-test leakage)

    const auto blocking = [] (const eb::hig::Finding& f) {
        return f.category == "overlap" || f.category == "clip" || f.category == "duplicate"
            || f.category == "target-size" || f.category == "contrast"; };

    struct A11y { const char* name; bool rm, ic, rt; };
    const A11y a11ys[] = { { "none", false, false, false }, { "reduce-motion", true, false, false },
                           { "increase-contrast", false, true, false }, { "reduce-transparency", false, false, true } };
    struct Sz { const char* name; int w, h; };
    const Sz sizes[] = { { "min", 780, 720 }, { "large", 1200, 1000 } };
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");

    juce::StringArray bad;
    for (bool dark : { true, false }) {
        mc.forceThemeForTest (dark);
        for (auto& a : a11ys) {
            eb::SystemA11y::setForTest (a.rm, a.ic, a.rt);
            for (auto& s : sizes) {
                mc.setSize (s.w, s.h);
                hig::writeDesignProbe (mc, jf, pf);
                for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                    if (blocking (f))
                        bad.add (juce::String (dark ? "dark" : "light") + "/" + a.name + "/" + s.name
                                 + ": " + f.category + " on " + f.element + " - " + f.message);
            }
        }
    }
    eb::SystemA11y::setForTest (false, false, false);   // restore the deterministic default for later tests
    mc.forceThemeForTest (wasDark);                     // restore Theme::s_dark (symmetry with the a11y restore)

    INFO ("blocking findings (" << bad.size() << "):\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

// The gate must be PROVEN to bite end-to-end - else a green badge is meaningless. Drive a live JUCE tree with a
// DELIBERATE overlap + a DELIBERATE clip through the real path (probe -> scoreDescriptor) and assert both are
// reported. If this ever passes with neither, the gate has silently become a no-op. (Cramming MainComponent does
// NOT work: JUCE's removeFromTop clamps a too-small area to zero-size children rather than overlapping them.)
TEST_CASE("HIG gate bites: probe+score flags a real overlap and a real clip (not a no-op)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    struct Rig : juce::Component {
        juce::Label a, b, clipped;
        Rig() {
            a.setText ("AAAA", juce::dontSendNotification);
            b.setText ("BBBB", juce::dontSendNotification);   // distinct text -> overlap, not duplicate
            clipped.setText ("this is a very long label that cannot possibly fit", juce::dontSendNotification);
            for (auto* l : { &a, &b, &clipped }) addAndMakeVisible (*l);
            a.setBounds (0, 0, 100, 20);
            b.setBounds (50, 0, 100, 20);        // overlaps a by 50px (> the 2px noise floor)
            clipped.setBounds (0, 40, 30, 16);   // ~50 chars in 30px -> textOverflows
        }
    };
    Rig rig; rig.setSize (200, 80);

    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    auto jf = tmp.getChildFile ("d.json");
    hig::writeDesignProbe (rig, jf, tmp.getChildFile ("d.png"));
    bool sawOverlap = false, sawClip = false;
    for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf))) {
        if (f.category == "overlap") sawOverlap = true;
        if (f.category == "clip")    sawClip = true;
    }
    CHECK (sawOverlap);
    CHECK (sawClip);
    tmp.deleteRecursively();
}

// ==================================================================================================
// #68: the per-ear verdict text + quality dots were never RENDER-verified - the captured two-line
// header only exists mid-run, which the construct-and-probe sweep above never reaches. Drive the
// WORST-CASE captured wording (generated from the pure StatusLadder, so this test tracks wording
// changes automatically) through the REAL header at the MINIMUM window and score the render.
// ==================================================================================================
TEST_CASE("HIG gate: the captured two-line header renders clean at the minimum window [#68]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const bool wasDark = eb::Theme::dark();
    const auto blocking = [] (const eb::hig::Finding& f) {
        return f.category == "overlap" || f.category == "clip" || f.category == "duplicate"
            || f.category == "target-size" || f.category == "contrast"; };

    // Case 1: the WORST captured state - both ears graded but CLIPPED with low SNR (the longest
    // compact line2: two full clip-cut clauses) under the Warn headline.
    eb::EarGradeSnapshot clipped;
    clipped.state = (int) eb::RefMonState::GradedClean;
    clipped.irSnrDb = 54.0f; clipped.thdPercent = 0.3f; clipped.sweepSnrDb = 12.0f; clipped.peakDb = 1.6f;
    eb::RunningSnapshot s1;
    s1.referenceLoaded = true; s1.earL = clipped; s1.earR = clipped;
    const auto out1 = eb::runningStatus (s1);

    // Case 2: the longest CALM captured state - verified ears with peak tails, plus the chain
    // advisory tail (only calm lines carry it) on the green headline.
    eb::EarGradeSnapshot verified = clipped;
    verified.sweepSnrDb = 31.0f; verified.peakDb = -6.2f;
    eb::RunningSnapshot s2;
    s2.referenceLoaded = true; s2.earL = verified; s2.earR = verified;
    s2.advisoryTail = " - input, cable 16-bit - 24-bit+ recommended for the cleanest measurement";
    const auto out2 = eb::runningStatus (s2);

    const auto jf = tmp.getChildFile ("h.json");
    const auto pf = tmp.getChildFile ("h.png");
    juce::StringArray bad;
    struct Case { const eb::StatusOut* o; const char* name; };
    const Case cases[] = { { &out1, "clipped" }, { &out2, "verified+advisory" } };
    for (bool dark : { true, false }) {
        mc.forceThemeForTest (dark);
        for (auto& c : cases) {
            mc.setSize (780, 720);                       // the app's hard minimum (Main.cpp setResizeLimits)
            mc.driveHeaderForTest (c.o->line1.text, c.o->line2.text, /*showDots*/ true);
            hig::writeDesignProbe (mc, jf, pf);
            for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                if (blocking (f))
                    bad.add (juce::String (dark ? "dark" : "light") + "/" + c.name + ": "
                             + f.category + " on " + f.element + " - " + f.message);
        }
    }
    mc.forceThemeForTest (wasDark);
    INFO ("line1(clipped)  = " << out1.line1.text
          << "\nline2(clipped)  = " << out1.line2.text
          << "\nline1(verified) = " << out2.line1.text
          << "\nline2(verified) = " << out2.line2.text
          << "\nblocking findings (" << bad.size() << "):\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}
