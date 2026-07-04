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
    const Sz sizes[] = { { "min", 900, 720 }, { "large", 1200, 1000 } };
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");

    juce::StringArray bad;
    // The wizard shows one stage at a time, so the gate must score EVERY step's hand-laid stage — not just
    // the launch step. Iterate the WizardStep enum as the OUTER axis (steps x theme x a11y x size).
    for (int si = 0; si < eb::kWizardStepCount; ++si) {
        static_assert (eb::kWizardStepCount == 4, "new step: extend the gate scenes");
        mc.forceWizardStepForTest ((eb::WizardStep) si);
        const char* stepName = (si == 0 ? "connect" : si == 1 ? "calibrate" : si == 2 ? "level" : "measure");
        for (bool dark : { true, false }) {
            mc.forceThemeForTest (dark);
            for (auto& a : a11ys) {
                eb::SystemA11y::setForTest (a.rm, a.ic, a.rt);
                for (auto& s : sizes) {
                    mc.setSize (s.w, s.h);
                    hig::writeDesignProbe (mc, jf, pf);
                    for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                        if (blocking (f))
                            bad.add (juce::String (stepName) + "/" + (dark ? "dark" : "light") + "/"
                                     + a.name + "/" + s.name
                                     + ": " + f.category + " on " + f.element + " - " + f.message);
                }
            }
        }
    }
    eb::SystemA11y::setForTest (false, false, false);   // restore the deterministic default for later tests
    mc.forceThemeForTest (wasDark);                     // restore Theme::s_dark (symmetry with the a11y restore)

    INFO ("blocking findings (" << bad.size() << "):\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

// ==================================================================================================
// C1 fake-axis regression guard. The gate iterates the WizardStep axis, but that axis is only real if
// forcing a different step actually swaps the VISIBLE (showing) element set. If forceWizardStepForTest
// ever silently re-probes the SAME stage for every step (e.g. a pin that is illegal in the hermetic env
// falls back to Connect), the per-step scenes collapse and Level/Measure are never scored. This test
// asserts the showing-element label sets at Connect and at Measure are NOT identical.
// ==================================================================================================
namespace {
// The set of non-empty labels of elements the probe reports as SHOWING (laid out + on screen).
juce::StringArray showingLabels (const juce::var& descriptor) {
    juce::StringArray out;
    if (const auto* els = descriptor.getProperty ("elements", {}).getArray())
        for (auto& e : *els)
            if ((bool) e.getProperty ("showing", false)) {
                const auto lbl = e.getProperty ("label", {}).toString();
                if (lbl.isNotEmpty()) out.add (lbl);
            }
    out.sort (true);
    return out;
}
}

TEST_CASE("HIG gate honesty: the visible element set DIFFERS between Connect and Measure (real step axis)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const bool wasDark = eb::Theme::dark();
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");

    mc.setSize (900, 780);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    hig::writeDesignProbe (mc, jf, pf);
    const auto connectLabels = showingLabels (juce::JSON::parse (jf));

    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    hig::writeDesignProbe (mc, jf, pf);
    const auto measureLabels = showingLabels (juce::JSON::parse (jf));

    mc.forceThemeForTest (wasDark);

    INFO ("connect(" << connectLabels.size() << "): " << connectLabels.joinIntoString (" | "));
    INFO ("measure(" << measureLabels.size() << "): " << measureLabels.joinIntoString (" | "));
    // Both stages must actually render something...
    CHECK (connectLabels.size() > 0);
    CHECK (measureLabels.size() > 0);
    // ...and the two stages must NOT present an identical visible-element set (the fake-axis guard).
    CHECK (connectLabels != measureLabels);
    tmp.deleteRecursively();
}

// ==================================================================================================
// C2 relayout contract: "MainComponent::resized() relays every stage." A stage-hosted surface made
// visible must be laid out (non-empty bounds) after a resized() at the SAME size — the old resized()
// only reset spine/stageHost bounds, so a same-size call never re-ran the child stages' resized() and
// the surface rendered 0x0 until an actual window resize.
// ==================================================================================================
TEST_CASE("MainComponent::resized() relays stages: a newly-visible stage surface gets non-empty bounds [C2]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const bool wasDark = eb::Theme::dark();
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");

    mc.setSize (900, 780);
    // Pin Measure so the status lines + dots are the visible stage; a distinctive driven text lets us
    // find the surface in the descriptor by label.
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    const juce::String driven = "Left earcup: verified clean";
    mc.driveHeaderForTest (driven, "Right earcup: verified clean", /*showDots*/ true);

    hig::writeDesignProbe (mc, jf, pf);
    const auto tree = juce::JSON::parse (jf);
    const auto* els = tree.getProperty ("elements", {}).getArray();
    REQUIRE (els != nullptr);

    bool foundShowingNonEmpty = false;
    for (auto& e : *els)
        if (e.getProperty ("label", {}).toString() == driven) {
            const bool showing = (bool) e.getProperty ("showing", false);
            const auto* b = e.getProperty ("bounds", {}).getDynamicObject();
            REQUIRE (b != nullptr);
            const int w = (int) b->getProperty ("w"), h = (int) b->getProperty ("h");
            INFO ("driven statusLine showing=" << (int) showing << " bounds " << w << "x" << h);
            if (showing && w > 0 && h > 0) foundShowingNonEmpty = true;
        }
    mc.forceThemeForTest (wasDark);
    CHECK (foundShowingNonEmpty);   // 0x0 here => resized() failed to relay MeasureStage (the C2 bug)
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
            mc.setSize (900, 720);                       // the app's hard minimum (Main.cpp setResizeLimits)
            // The driven status lines + dots now live in MeasureStage — pin it so they're in the visible tree.
            mc.forceWizardStepForTest (eb::WizardStep::Measure);
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
