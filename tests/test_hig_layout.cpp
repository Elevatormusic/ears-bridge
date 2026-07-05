#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"
#include "gui/CalSlotComponent.h"   // P2: the loaded cal card is scored standalone (the hermetic env never has cals)
#include "gui/juce_design_probe.h"
#include "gui/HigScore.h"
#include "gui/HigThresholds.h"  // T10: kMinFontPt (the min-font floor the min-font rule + its bite test read)
#include "gui/SystemA11y.h"
#include <functional>           // T10: std::function for the displacement-honesty colour walker
#include "gui/Theme.h"          // P2: setDarkForTest for the standalone CalSlot loaded-card case
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
                    for (auto& f : eb::hig::scoreMinFont (juce::JSON::parse (jf)))
                        bad.add (juce::String (stepName) + "/" + (dark ? "dark" : "light") + "/"
                                 + a.name + "/" + s.name + ": " + f.category + " on " + f.element
                                 + " - " + f.message);
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

// ==================================================================================================
// P2: the Advanced-FIR disclosure OPEN state. The 64-cell matrix scores Calibrate COLLAPSED
// (the default); the disclosed section is a distinct hand-laid layout that must also be clean.
//
// T10: scored at the TRUE 900x720 minimum again. Task 8 temporarily scored at 760 because the
// disclosed section straddled the viewport fold at 720 (the probe clips geometry through
// Viewports, so the trim slider scored 6px tall). The T10 compaction makes the advanced-open
// state fit entirely at 720 (ledger: worst case 518 <= 546), so the fold artifact is gone and
// 720 is load-bearing: if this case ever needs raising again, the no-scroll contract is broken.
// ==================================================================================================
TEST_CASE("HIG gate: Calibrate advanced-FIR disclosure open renders clean [P2]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const bool wasDark = eb::Theme::dark();
    const auto blocking = [] (const eb::hig::Finding& f) {
        return f.category == "overlap" || f.category == "clip" || f.category == "duplicate"
            || f.category == "target-size" || f.category == "contrast"; };
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");
    juce::StringArray bad;
    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    mc.calibrateStageForTest().setAdvancedOpen (true);
    struct Sz { const char* name; int w, h; };
    const Sz sizes[] = { { "min", 900, 720 }, { "large", 1200, 1000 } };
    for (bool dark : { true, false }) {
        mc.forceThemeForTest (dark);
        for (auto& s : sizes) {
            mc.setSize (s.w, s.h);
            hig::writeDesignProbe (mc, jf, pf);
            for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                if (blocking (f))
                    bad.add (juce::String (dark ? "dark" : "light") + "/" + s.name + ": "
                             + f.category + " on " + f.element + " - " + f.message);
            for (auto& f : eb::hig::scoreMinFont (juce::JSON::parse (jf)))
                bad.add (juce::String (dark ? "dark" : "light") + "/" + s.name + ": "
                         + f.category + " on " + f.element + " - " + f.message);
        }
    }
    mc.calibrateStageForTest().setAdvancedOpen (false);       // restore the default state
    mc.forceThemeForTest (wasDark);
    INFO ("blocking findings (" << bad.size() << "):\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

// ==================================================================================================
// P2: the LOADED cal card. The hermetic editor never has cals, so the matrix only ever scores
// the empty drop zones - score the worst-case loaded card standalone (Unknown-type caution +
// swap banner) at the two real cell widths, both themes.
//
// T5 debt (ledgered): the RAW-caution card had NO fixture in tests/data/, so Task 5's render pass
// could never drive the amber RAW chip + RAW caption path. This case now also scores a RAW card
// (loaded from the tests/data/L_RAW_0000000.txt fixture added with this task) so the second caution
// variant is gated too, not only the Unknown one.
// ==================================================================================================
TEST_CASE("HIG gate: loaded cal card (caution + swap banner) scores clean standalone [P2]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    const auto blocking = [] (const eb::hig::Finding& f) {
        return f.category == "overlap" || f.category == "clip" || f.category == "duplicate"
            || f.category == "target-size" || f.category == "contrast"; };
    eb::Theme theme;                                          // palette owner for setDarkForTest
    const bool wasDark = eb::Theme::dark();
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");
    juce::StringArray bad;
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        eb::CalSlotComponent slot ("Left ear");               // constructed AFTER the mode is set
        // Install the themed LookAndFeel on the standalone card, exactly as MainComponent does on itself
        // (setLookAndFeel(&theme)). The probe resolves a transparent label's background by walking up to the
        // nearest ancestor's ResizableWindow::backgroundColourId, which is REGISTERED on the LookAndFeel -
        // so without this the card's labels would be contrast-scored against the process-default (dark) bg
        // even in light mode, a harness-fidelity artifact rather than a real card contrast. Detached below.
        slot.setLookAndFeel (&theme);
        auto cal = juce::File::createTempFile (".txt");
        cal.replaceWithText ("* House Curve\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
        REQUIRE (slot.loadFromFile (cal));
        cal.deleteFile();
        slot.setProblem ("This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.");
        for (int w : { 289, 373 }) {                          // min-window and 760-cap cell widths
            slot.setSize (w, slot.preferredHeight());
            hig::writeDesignProbe (slot, jf, pf);
            for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                if (blocking (f))
                    bad.add (juce::String (dark ? "dark" : "light") + "/" + juce::String (w) + "px: "
                             + f.category + " on " + f.element + " - " + f.message);
        }

        // T5 debt: the RAW-caution variant. Same worst-case frame (swap banner + a caution note),
        // but the RAW chip is amber and the note copy differs, so it is a distinct laid-out state.
        eb::CalSlotComponent rawSlot ("Left ear");
        rawSlot.setLookAndFeel (&theme);                      // themed bg for the probe (see note above)
        const juce::File rawFixture = juce::File (EB_TEST_DATA_DIR).getChildFile ("L_RAW_0000000.txt");
        REQUIRE (rawSlot.loadFromFile (rawFixture));
        // Pin the resolved type: the fixture must parse AS RAW, or this case silently scores a
        // no-note card (no amber chip, no caution) and stays green if CalFile detection ordering
        // ever changed. Assert the seam the caution path keys off (rawCaution = type==Raw).
        REQUIRE (rawSlot.calFile().has_value());
        REQUIRE (rawSlot.calFile()->type == eb::CalType::Raw);
        rawSlot.setProblem ("This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.");
        for (int w : { 289, 373 }) {
            rawSlot.setSize (w, rawSlot.preferredHeight());
            hig::writeDesignProbe (rawSlot, jf, pf);
            for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
                if (blocking (f))
                    bad.add (juce::String (dark ? "dark" : "light") + "/RAW/" + juce::String (w) + "px: "
                             + f.category + " on " + f.element + " - " + f.message);
        }
        rawSlot.setLookAndFeel (nullptr);                     // detach before theme/slot destruct (JUCE rule)
        slot.setLookAndFeel (nullptr);
    }
    theme.setDarkForTest (wasDark);
    INFO ("blocking findings (" << bad.size() << "):\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

// ==================================================================================================
// T10 min-font rule (macOS ramp floor: 10pt body-legibility floor; adopted from the apple-hig
// v1.10.0 review bar). kMinFontPt is in PROBE units (Font::getHeightInPoints), which run BELOW
// the JUCE pixel height on Windows - the 11px ramp floor must clear the threshold with margin
// on every CI platform, or the constant must be recalibrated (measured - 0.5), never the rule
// dropped. This case asserts BOTH halves: bites on tiny text, quiet on the smallest ramp font.
// ==================================================================================================
TEST_CASE("HIG min-font rule: bites on 6px text, quiet on the 11px ramp floor [T10]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    struct Rig : juce::Component {
        juce::Label tiny, floor11;
        Rig() {
            tiny.setText ("too small", juce::dontSendNotification);
            tiny.setFont (juce::Font (juce::FontOptions (6.0f)));
            floor11.setText ("RATE", juce::dontSendNotification);
            floor11.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
            addAndMakeVisible (tiny);   tiny.setBounds (0, 0, 120, 12);
            addAndMakeVisible (floor11); floor11.setBounds (0, 20, 120, 14);
        }
    };
    Rig rig; rig.setSize (200, 60);
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    auto jf = tmp.getChildFile ("f.json");
    hig::writeDesignProbe (rig, jf, tmp.getChildFile ("f.png"));
    const auto tree = juce::JSON::parse (jf);

    // Measured probe-points for both labels (the calibration evidence this test exists to pin).
    double tinyPt = -1.0, floorPt = -1.0;
    if (const auto* els = tree.getProperty ("elements", {}).getArray())
        for (auto& e : *els) {
            const auto lbl = e.getProperty ("label", {}).toString();
            if (lbl == "too small") tinyPt  = (double) e.getProperty ("fontPt", 0.0);
            if (lbl == "RATE")      floorPt = (double) e.getProperty ("fontPt", 0.0);
        }
    INFO ("measured probe-points: tiny(6px)=" << tinyPt << " floor(11px)=" << floorPt
          << "  kMinFontPt=" << eb::hig::kMinFontPt);
    CHECK (tinyPt > 0.0);
    CHECK (tinyPt < eb::hig::kMinFontPt);                       // the rule CAN bite
    CHECK (floorPt >= eb::hig::kMinFontPt + 0.5);               // cross-platform margin guard

    const auto findings = eb::hig::scoreMinFont (tree);
    bool tinyFlagged = false;
    for (auto& f : findings)
        if (f.category == "min-font") tinyFlagged = true;
    CHECK (tinyFlagged);                                        // positive half: 6px text is flagged
    CHECK (findings.size() == 1);                               // negative half: the 11px floor label is NOT
    tmp.deleteRecursively();
}

// ==================================================================================================
// T10 PERMANENT RULES (user requirements, ledgered 2026-07-05; spec 4 amendment):
//   RULE 1 - no scrolling in any gated workflow state at the 900x720 minimum (Viewport = safety
//            net only). Fail-closed: static_assert on each stage's WorkflowState count.
//   RULE 2 - displacement honesty: with a disclosure open, (a) the disclosed content fully fits,
//            (b) the collapse affordance stays visible, (c) NO visible warn/danger-toned surface
//            is displaced out of the viewport. The walker below enforces (c) by CONSTRUCTION
//            (any Label whose text colour is Theme::warn()/danger() is covered automatically -
//            there is no list to forget to extend).
// ==================================================================================================
namespace {
juce::StringArray displacedWarnSurfaces (juce::Viewport& vp) {
    juce::StringArray out;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c) {
        if (! c.isVisible()) return;
        if (auto* l = dynamic_cast<juce::Label*> (&c)) {
            const auto col = l->findColour (juce::Label::textColourId);
            if (l->getText().isNotEmpty()
                && (col == eb::Theme::warn() || col == eb::Theme::danger())) {
                const auto inVp = vp.getLocalArea (l, l->getLocalBounds());
                if (! vp.getLocalBounds().contains (inVp))
                    out.add (l->getText().substring (0, 48));
            }
        }
        for (auto* ch : c.getChildren()) walk (*ch);
    };
    if (auto* content = vp.getViewedComponent()) walk (*content);
    return out;
}
bool fullyVisibleIn (juce::Viewport& vp, juce::Component& c) {
    return c.isVisible() && vp.getLocalBounds().contains (vp.getLocalArea (&c, c.getLocalBounds()));
}
} // namespace

TEST_CASE("No-scroll + displacement gate: Calibrate workflow states at 900x720 [T10]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    static_assert (eb::CalibrateStage::kWorkflowStateCount == 6,
                   "new Calibrate workflow state: extend this gate's driver + the T10 ledger");
    using WS = eb::CalibrateStage::WorkflowState;
    const juce::File data (EB_TEST_DATA_DIR);
    mc.setSize (900, 720);

    const auto apply = [&] (WS s) {
        auto& L = mc.leftCalForTest(); auto& R = mc.rightCalForTest();
        if (L.hasCal()) L.clearCal();
        if (R.hasCal()) R.clearCal();
        L.setProblem ({}); R.setProblem ({});
        switch (s) {
            case WS::EmptyUnity:
            case WS::EmptyUnityAdvancedOpen: break;
            case WS::LoadedCleanPair:
                REQUIRE (L.loadFromFile (data.getChildFile ("L_HEQ_0000000.txt")));
                REQUIRE (R.loadFromFile (data.getChildFile ("R_HEQ_0000000.txt")));
                break;
            case WS::LoadedWorstPair:
            case WS::LoadedWorstAdvancedOpen:
                // Worst legal card: RAW left (amber on-card caution note) + HEQ right, swap
                // banners on BOTH -> problem row + note row + thumbnail stack all at once.
                REQUIRE (L.loadFromFile (data.getChildFile ("L_RAW_0000000.txt")));
                REQUIRE (R.loadFromFile (data.getChildFile ("R_HEQ_0000000.txt")));
                L.setProblem ("This looks like the RIGHT cal, but it's in the LEFT slot - swap the files.");
                R.setProblem ("This looks like the LEFT cal, but it's in the RIGHT slot - swap the files.");
                break;
            case WS::EmptyErrorStripAdvancedOpen:
                L.loadFromFile (data.getChildFile ("no-such-cal-file.txt"));   // #38 stale-path strip
                break;
        }
        mc.calibrateStageForTest().setAdvancedOpen (s == WS::EmptyUnityAdvancedOpen
                                                    || s == WS::LoadedWorstAdvancedOpen
                                                    || s == WS::EmptyErrorStripAdvancedOpen);
        mc.forceWizardStepForTest (eb::WizardStep::Calibrate);   // re-render unity/caption + relayout
    };

    juce::StringArray bad;
    for (int i = 0; i < eb::CalibrateStage::kWorkflowStateCount; ++i) {
        apply ((WS) i);
        auto& vp = mc.calibrateStageForTest().viewportForTest();
        const int contentH = vp.getViewedComponent()->getHeight(), vpH = vp.getHeight();
        if (contentH > vpH)
            bad.add ("RULE1 state " + juce::String (i) + ": content " + juce::String (contentH)
                     + " > viewport " + juce::String (vpH));
        for (auto& s : displacedWarnSurfaces (vp))
            bad.add ("RULE2 state " + juce::String (i) + ": warn surface displaced: " + s);
    }
    // RULE 2 (a)+(b) on the open worst state: disclosure row + every disclosed control visible.
    apply (WS::LoadedWorstAdvancedOpen);
    {
        auto& stage = mc.calibrateStageForTest();
        auto& vp = stage.viewportForTest();
        if (! fullyVisibleIn (vp, stage.advancedFirForTest()))
            bad.add ("RULE2a/b: the Advanced FIR collapse affordance left the visible area");
    }
    apply (WS::EmptyUnity);   // restore
    INFO ("violations:\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

TEST_CASE("No-scroll + displacement gate: Connect workflow states at 900x720 [T10]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    static_assert (eb::ConnectStage::kWorkflowStateCount == 6,
                   "new Connect workflow state: extend this gate's driver + the T10 ledger");
    using WS = eb::ConnectStage::WorkflowState;
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);

    const auto apply = [&] (WS s) {
        auto& stage = mc.connectStageForTest();
        stage.notUsingDiracForTest().setOpen (s == WS::OverrideOpen || s == WS::OverrideOpenWorst);
        mc.driveConnectWarningsForTest (s == WS::DiracHintFix || s == WS::DiracHintFixRateWarn || s == WS::OverrideOpenWorst,
                                        s == WS::RateWarn || s == WS::DiracHintFixRateWarn || s == WS::OverrideOpenWorst);
    };

    juce::StringArray bad;
    for (int i = 0; i < eb::ConnectStage::kWorkflowStateCount; ++i) {
        apply ((WS) i);
        auto& vp = mc.connectStageForTest().viewportForTest();
        const int contentH = vp.getViewedComponent()->getHeight(), vpH = vp.getHeight();
        if (contentH > vpH)
            bad.add ("RULE1 state " + juce::String (i) + ": content " + juce::String (contentH)
                     + " > viewport " + juce::String (vpH));
        for (auto& s2 : displacedWarnSurfaces (vp))
            bad.add ("RULE2 state " + juce::String (i) + ": warn surface displaced: " + s2);
    }
    // RULE 2 (b) worst open state: the disclosure row + the disclosed override toggle visible.
    apply (WS::OverrideOpenWorst);
    {
        auto& stage = mc.connectStageForTest();
        auto& vp = stage.viewportForTest();
        if (! fullyVisibleIn (vp, stage.notUsingDiracForTest()))
            bad.add ("RULE2b: the Not-using-Dirac collapse affordance left the visible area");
        if (! fullyVisibleIn (vp, mc.overrideToggleForTest()))
            bad.add ("RULE2a: the disclosed override toggle left the visible area");
    }

    // BOUNDARY (the documented safety-net state, NOT a gated workflow state): force the
    // post-Start preflight stack on top of the worst gated state. Scrolling is ALLOWED here -
    // but every live warning must still sit ABOVE the fold (they all live in the upper cards).
    // This is the negative test that RULE 1 does not over-claim and RULE 2 cannot false-pass.
    mc.preflightLabelForTest().setText ("Start failed: could not open the output device in shared mode",
                                        juce::dontSendNotification);
    mc.resized();
    for (auto& s2 : displacedWarnSurfaces (mc.connectStageForTest().viewportForTest()))
        bad.add ("BOUNDARY: warn surface below the fold in the safety-net state: " + s2);
    mc.preflightLabelForTest().setText ({}, juce::dontSendNotification);

    // NEGATIVE (false-fire guard): at a tall window with everything forced, the walker must
    // report nothing (all surfaces trivially visible) - proves it keys on geometry, not state.
    mc.setSize (900, 1000);
    apply (WS::OverrideOpenWorst);
    for (auto& s2 : displacedWarnSurfaces (mc.connectStageForTest().viewportForTest()))
        bad.add ("NEGATIVE(tall): walker false-fired on: " + s2);

    apply (WS::Default);   // restore
    INFO ("violations:\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}
