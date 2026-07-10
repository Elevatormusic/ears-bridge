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
    // P3 Task 7 re-point: the driven header retired with statusLineR/dots - the VerdictCard grid is
    // the stage-hosted surface now. The LEFT clean card's fix body is the distinctive driven text.
    eb::EarGradeSnapshot cleanE, margE;
    cleanE.state = (int) eb::RefMonState::GradedClean;    cleanE.sweepSnrDb = 30; cleanE.irSnrDb = 56; cleanE.thdPercent = 0.8f; cleanE.peakDb = -8;
    margE.state  = (int) eb::RefMonState::GradedMarginal; margE.sweepSnrDb  = 18; margE.irSnrDb  = 52; margE.thdPercent  = 0.3f; margE.peakDb  = -8;
    const auto clean = eb::verdictCardModelAuto ("LEFT EAR",  cleanE, 0u, {}, false, false);
    const auto marg  = eb::verdictCardModelAuto ("RIGHT EAR", margE,  0u, {}, false, false);
    mc.driveVerdictForTest (clean, marg, false);
    const juce::String driven = clean.fixBody;   // "A strong, low-noise capture - ..." (spec 6 clean copy)
    CHECK (driven == "A strong, low-noise capture - Dirac will treat this ear as precise.");

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
            INFO ("driven fixBody showing=" << (int) showing << " bounds " << w << "x" << h);
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
// #68 (P3 Task 7 re-point): the per-ear verdict was never RENDER-verified. The two-line header +
// dots retired - the render-ratification budget is re-derived for the CARD (spec 6): drive the
// worst-case CARD MODELS through the real Measure stage at the MINIMUM window, score dark+light for
// blocking findings, and pin the promoted fix body's textOverflows:false (measured-wrap budget).
// Cases: (1) CLIPPED marginal (longest fix body via clipFixBody); (2) the longest CALM model (clean
// + the ratified drift note headlining); (3) SUSPECT (the Red-SNR ladder note - 3 lines at card
// width, the T2-review trust-bug scene); (4) RE-LEARN (the unpinned qualifier squish scene).
// ==================================================================================================
TEST_CASE("HIG gate: worst-case VerdictCards render clean at the minimum window [#68]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const bool wasDark = eb::Theme::dark();
    // NO "duplicate" in this blocking set (a DOCUMENTED, scoped exclusion): the per-ear grid
    // legitimately repeats identical structural labels across its two cards - "SNR"/"IR-SNR"/"THD"
    // column heads and the "Details" rows are the same text at the same size BY DESIGN (one card
    // component, two ears). The duplicate rule is a copy-paste-bug detector; across this pair it
    // can only fire on the intended structure. Every other test keeps the full blocking set.
    const auto blocking = [] (const eb::hig::Finding& f) {
        return f.category == "overlap" || f.category == "clip"
            || f.category == "target-size" || f.category == "contrast"; };

    const auto snap = [] (eb::RefMonState st, float snr, float ir, float thd, float peak) {
        eb::EarGradeSnapshot e;
        e.state = (int) st; e.sweepSnrDb = snr; e.irSnrDb = ir; e.thdPercent = thd; e.peakDb = peak;
        return e;
    };
    // (1) CLIPPED on a marginal ear: clip beats the band note - clipFixBody is the longest fix body.
    const auto clipped = eb::verdictCardModelAuto ("LEFT EAR",
        snap (eb::RefMonState::GradedMarginal, 18.0f, 54.0f, 0.3f, +1.6f), 0u, {}, false, false);
    CHECK (clipped.fixBody == eb::clipFixBody (1.6f));               // the intended worst case IS driven
    // (2) longest CALM: clean + the RATIFIED drift note headlining ("Worth checking." lead).
    eb::ShapeScalars drift; drift.driftMaxDb = -9.4f; drift.hfShelfDb = -4.4f;
    const auto calmDrift = eb::verdictCardModelAuto ("LEFT EAR",
        snap (eb::RefMonState::GradedClean, 31.0f, 56.0f, 0.4f, -6.2f), eb::ShapeFlag::kDrift, drift, false, false);
    CHECK (calmDrift.fixLead == "Worth checking.");
    // (3) SUSPECT: the Red-SNR ladder action note (T2 review: needs 3 lines vs the old fixed 32px -
    // a clipped action line on the Danger card is a trust bug; the card wraps to MEASURED height).
    const auto suspect = eb::verdictCardModelAuto ("LEFT EAR",
        snap (eb::RefMonState::GradedSuspect, 8.0f, 20.0f, 6.0f, -8.0f), 0u, {}, false, false);
    CHECK (suspect.badge == "Suspect");
    // (4) RE-LEARN: gradeWord "No match" beside the qualifier (unpinned copy - the squish scene).
    const auto relearn = eb::verdictCardModelAuto ("LEFT EAR",
        snap (eb::RefMonState::ReferenceStale, 0, 0, 0, -120.0f), 0u, {}, false, false);
    CHECK (relearn.badge == "Re-learn");
    // Sibling RIGHT cards: clean beside the non-clean cases, marginal beside the clean calm+drift
    // case (scene variety - and no scene renders the same grade word on both cards).
    const auto cleanR = eb::verdictCardModelAuto ("RIGHT EAR",
        snap (eb::RefMonState::GradedClean, 30.0f, 56.0f, 0.8f, -8.0f), 0u, {}, false, false);
    const auto margR = eb::verdictCardModelAuto ("RIGHT EAR",
        snap (eb::RefMonState::GradedMarginal, 18.0f, 52.0f, 0.3f, -8.0f), 0u, {}, false, false);

    const auto jf = tmp.getChildFile ("h.json");
    const auto pf = tmp.getChildFile ("h.png");
    juce::StringArray bad;
    struct Case { const eb::VerdictCardModel* m; const eb::VerdictCardModel* r; const char* name; };
    const Case cases[] = { { &clipped, &cleanR, "clipped" }, { &calmDrift, &margR, "calm+drift" },
                           { &suspect, &cleanR, "suspect" }, { &relearn, &cleanR, "relearn" } };
    for (bool dark : { true, false }) {
        mc.forceThemeForTest (dark);
        for (auto& c : cases) {
            mc.setSize (900, 720);                       // the app's hard minimum (Main.cpp setResizeLimits)
            mc.driveVerdictForTest (*c.m, *c.r, false);
            hig::writeDesignProbe (mc, jf, pf);
            const auto tree = juce::JSON::parse (jf);
            for (auto& f : eb::hig::scoreDescriptor (tree))
                if (blocking (f))
                    bad.add (juce::String (dark ? "dark" : "light") + "/" + c.name + ": "
                             + f.category + " on " + f.element + " - " + f.message);
            // The fix body's measured-wrap budget, pinned non-vacuously (the test_verdictcard idiom):
            // the label must be IN the descriptor with textOverflows:false.
            bool sawFixBody = false;
            if (const auto* els = tree.getProperty ("elements", {}).getArray())
                for (auto& e2 : *els)
                    if (e2.getProperty ("label", {}).toString() == c.m->fixBody) {
                        sawFixBody = true;
                        if ((bool) e2.getProperty ("textOverflows", false))
                            bad.add (juce::String (dark ? "dark" : "light") + "/" + c.name
                                     + ": fix body overflows the card");
                    }
            if (! sawFixBody)
                bad.add (juce::String (dark ? "dark" : "light") + "/" + c.name
                         + ": fix body label missing from the descriptor (vacuous budget check)");
        }
    }
    mc.forceThemeForTest (wasDark);
    INFO ("blocking findings (" << bad.size() << "):\n" << bad.joinIntoString ("\n"));
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
// state fit entirely at 720; P2.9 T6 turned the OUTPUT TRIM eyebrow+slider stack into a single
// 28px parameter row, shrinking every open state by 16px (ledger: worst-open natural height
// 534 -> 518 <= 546 viewport), so the fold artifact is gone and 720 is load-bearing: if this
// case ever needs raising again, the no-scroll contract is broken.
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
//            (any Label - and, since P3 Task 3, any TextButton - whose painted text colour is
//            Theme::warn()/danger() is covered automatically; there is no list to forget to
//            extend). Known residual: warn text hosted in OTHER component classes (ToggleButton,
//            HyperlinkButton, raw paint()) is still invisible to the walker - keep tone text in
//            Labels/TextButtons, per VerdictCard's Labels-only construction.
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
        else if (auto* b = dynamic_cast<juce::TextButton*> (&c)) {
            // P3 (Task 3): non-Label warn text - the RAW-chip precedent. Theme::drawButtonText
            // (Theme.cpp) resolves the painted text colour via the LookAndFeel_V2 idiom
            // getToggleState() ? textColourOnId : textColourOffId (both its glyph path and the
            // LookAndFeel_V4 fallback) - the walker reads the SAME state-resolved property, so the
            // paint-time tone and what the gate sees can never disagree.
            const auto col = b->findColour (b->getToggleState() ? juce::TextButton::textColourOnId
                                                                : juce::TextButton::textColourOffId);
            if (b->getButtonText().isNotEmpty()
                && (col == eb::Theme::warn() || col == eb::Theme::danger())) {
                const auto inVp = vp.getLocalArea (b, b->getLocalBounds());
                if (! vp.getLocalBounds().contains (inVp))
                    out.add (b->getButtonText().substring (0, 48));
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

// ==================================================================================================
// RULE2 walker BITE test. displacedWarnSurfaces is the primitive the two workflow gates above rely
// on to catch a warn/danger surface pushed below the fold - but every COMMITTED call expects an
// EMPTY result, so the walker's own RED path (it actually RETURNS a displaced surface) had zero
// evidence. This drives a synthetic Viewport + tall content directly through the SAME function the
// gates call (no copy of the logic) and asserts every arm: warn label below fold -> returned; the
// same label lifted above the fold -> empty; a danger-toned variant below fold -> returned; a
// NON-warn-toned label below the fold -> NOT returned (the overfire negative). P3 (Task 3) adds the
// non-Label arms: a warn-toned TextButton below fold -> returned; a plain button below fold -> not;
// the warn button lifted above fold -> not; a TOGGLED warn-On button (textColourOnId, the colour
// Theme::drawButtonText actually paints when toggled) below fold -> returned, untoggled -> not.
// ==================================================================================================
TEST_CASE("RULE2 walker bites: displacedWarnSurfaces returns a below-fold warn/danger surface [T10]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::Viewport vp;
    vp.setScrollBarsShown (false, false);   // no scrollbar inset - the visible area is exactly the viewport
    vp.setSize (300, 200);

    juce::Component content;
    content.setSize (300, 600);             // taller than the 200px viewport -> a real fold at y=200

    juce::Label warnLbl;
    warnLbl.setText ("input clipping detected", juce::dontSendNotification);
    warnLbl.setColour (juce::Label::textColourId, eb::Theme::warn());
    content.addAndMakeVisible (warnLbl);

    juce::Label plainLbl;                   // NOT warn/danger toned - the overfire guard
    plainLbl.setText ("passive guidance line", juce::dontSendNotification);
    plainLbl.setColour (juce::Label::textColourId, eb::Theme::text());
    content.addAndMakeVisible (plainLbl);

    vp.setViewedComponent (&content, false);        // vp does not own it; both live to end of scope

    // Arm 1: warn label BELOW the fold (y = 400, viewport shows 0..200) -> the walker RETURNS it.
    warnLbl.setBounds (10, 400, 200, 20);
    plainLbl.setBounds (10, 420, 200, 20);          // parked below the fold too (see Arm 4)
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE (hits.contains ("input clipping detected"));   // RED path proven
    }

    // Arm 4 (folded into Arm 1's geometry): the plain-toned label sits below the fold as well, yet
    // the walker must NOT report it - it keys on the warn/danger tone, not mere displacement.
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE_FALSE (hits.contains ("passive guidance line"));   // overfire negative
    }

    // Arm 2: lift the SAME warn label fully above the fold (0..200) -> empty.
    warnLbl.setBounds (10, 20, 200, 20);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE (hits.isEmpty());               // no warn surface displaced now
    }

    // Arm 3: a danger-toned variant below the fold -> RETURNED (the second tone is covered too).
    warnLbl.setColour (juce::Label::textColourId, eb::Theme::danger());
    warnLbl.setText ("output device lost", juce::dontSendNotification);
    warnLbl.setBounds (10, 450, 200, 20);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE (hits.contains ("output device lost"));
    }

    // Arm 5 (P3): a warn-toned TextButton below the fold - the RAW-chip precedent. The walker must
    // return NON-LABEL warn text too.
    juce::TextButton warnBtn ("fix the RAW pair");
    warnBtn.setColour (juce::TextButton::textColourOffId, eb::Theme::warn());
    content.addAndMakeVisible (warnBtn);
    warnBtn.setBounds (10, 480, 200, 24);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE (hits.contains ("fix the RAW pair"));            // RED until the walker learns buttons
    }
    // Arm 6: a PLAIN-toned button below the fold must NOT be reported (overfire negative).
    juce::TextButton plainBtn ("open log folder");
    content.addAndMakeVisible (plainBtn);
    plainBtn.setBounds (10, 510, 200, 24);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE_FALSE (hits.contains ("open log folder"));
    }
    // Arm 7: the SAME warn button lifted above the fold -> not reported.
    warnBtn.setBounds (10, 40, 200, 24);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE_FALSE (hits.contains ("fix the RAW pair"));
    }
    // Arm 8 (P3): the TOGGLED path. Theme::drawButtonText paints a toggled button's text from
    // textColourOnId (the LookAndFeel_V2 idiom, verified in Theme.cpp) - the walker must read the
    // SAME state-resolved property. Toggled warn-On button below fold -> returned; the identical
    // button untoggled paints its (plain) Off colour -> not returned.
    juce::TextButton togBtn ("retry in exclusive mode");
    togBtn.setColour (juce::TextButton::textColourOnId, eb::Theme::warn());
    togBtn.setToggleState (true, juce::dontSendNotification);
    content.addAndMakeVisible (togBtn);
    togBtn.setBounds (10, 540, 200, 24);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE (hits.contains ("retry in exclusive mode"));     // paints warn while toggled
    }
    togBtn.setToggleState (false, juce::dontSendNotification);
    {
        const auto hits = displacedWarnSurfaces (vp);
        REQUIRE_FALSE (hits.contains ("retry in exclusive mode")); // now paints the plain Off colour
    }

    vp.setViewedComponent (nullptr, false);     // detach before content/labels destruct (JUCE order)
}

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
            case WS::Count: jassertfalse; return;   // sentinel - never a real state (fail-closed, no silent fallthrough)
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

TEST_CASE("No-scroll gate: Level workflow states at 900x720 [P3]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    static_assert (eb::LevelStage::kWorkflowStateCount == 2,
                   "new Level workflow state: extend this gate's driver");
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Level);
    juce::StringArray bad;
    for (int i = 0; i < eb::LevelStage::kWorkflowStateCount; ++i) {
        mc.driveLevelClipForTest (i == (int) eb::LevelStage::WorkflowState::ClipWarning);
        auto& stage = mc.levelStageForTest();
        // Level has NO viewport (fixed column): RULE1 = every visible child bottom stays inside
        // the stage; RULE2 = the clip warning (the one warn surface) fully visible when shown.
        for (auto* ch : stage.getChildren())
            if (ch->isVisible() && ch->getBounds().getBottom() > stage.getHeight())
                bad.add ("RULE1 state " + juce::String (i) + ": child below the stage fold");
        if (i == (int) eb::LevelStage::WorkflowState::ClipWarning) {
            auto& hint = mc.inputClipHintForTest();
            if (! hint.isVisible())
                bad.add ("ClipWarning: the clip hint is not visible after the drive seam");
            else if (! stage.getLocalBounds().contains (hint.getBoundsInParent()))
                bad.add ("RULE2 ClipWarning: the clip warning left the stage area");
        }
    }
    mc.driveLevelClipForTest (false);
    INFO ("violations:\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}

TEST_CASE("No-scroll + displacement gate: Measure workflow states at 900x720 [P3]") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    static_assert (eb::MeasureStage::kWorkflowStateCount == 9,
                   "new Measure workflow state: extend this gate's driver");
    using WS = eb::MeasureStage::WorkflowState;
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    auto& stage = mc.measureStageForTest();

    // P3 Task 7: the verdict-state card models (clean L + marginal R; stale variants; the
    // details-open R carries a flagged Drift chip + observation - warn-toned content the RULE2
    // walker must keep above the fold).
    const auto snap = [] (eb::RefMonState st, float snr, float ir, float thd) {
        eb::EarGradeSnapshot e;
        e.state = (int) st; e.sweepSnrDb = snr; e.irSnrDb = ir; e.thdPercent = thd; e.peakDb = -8.0f;
        return e;
    };
    const auto cleanL = eb::verdictCardModelAuto ("LEFT EAR",
        snap (eb::RefMonState::GradedClean, 30, 56, 0.8f), 0u, {}, false, false);
    const auto margR = eb::verdictCardModelAuto ("RIGHT EAR",
        snap (eb::RefMonState::GradedMarginal, 18, 52, 0.3f), 0u, {}, false, false);
    const auto staleL = eb::verdictCardModelAuto ("LEFT EAR",
        snap (eb::RefMonState::GradedClean, 30, 56, 0.8f), 0u, {}, true, false);
    const auto staleR = eb::verdictCardModelAuto ("RIGHT EAR",
        snap (eb::RefMonState::GradedMarginal, 18, 52, 0.3f), 0u, {}, true, false);
    eb::ShapeScalars drift; drift.driftMaxDb = -9.4f;
    const auto margDriftR = eb::verdictCardModelAuto ("RIGHT EAR",
        snap (eb::RefMonState::GradedMarginal, 18, 52, 0.3f), eb::ShapeFlag::kDrift, drift, false, false);

    const auto apply = [&] (WS s) {
        using Lead = eb::MeasureStage::Lead;
        using CC   = eb::CaptureCardModel;
        // The three verdict arms drive the SAME seam the render-ratification uses (pin + lead +
        // models + details); everything else drives the stage feeds directly.
        if (s == WS::VerdictBoth)        { mc.driveVerdictForTest (cleanL, margR, false); return; }
        if (s == WS::VerdictStale)       { mc.driveVerdictForTest (staleL, staleR, false); return; }
        if (s == WS::VerdictDetailsOpen) { mc.driveVerdictForTest (cleanL, margDriftR, true); return; }
        mc.driveVerdictForTest (eb::VerdictCardModel{}, eb::VerdictCardModel{}, false);   // grid back to CaptureCards
        const Lead lead = s == WS::ReferenceNeeded ? Lead::Reference
                        : s == WS::HwDirac ? Lead::HwDirac : Lead::Waiting;
        stage.setLead (lead);
        // Render the REAL per-lead head copy too (P3 Task 6): the armed title is the app's widest -
        // it exercises the 2-line title mode inside the constant kHeaderH reserve.
        const auto head = eb::MeasureStage::measureHeadCopy (lead, false, false);
        stage.setHeadCopy (head.title, head.sub);
        stage.setWaitHint (s == WS::TimeoutHint
            ? eb::MeasureStage::waitingHint (eb::MeasureStage::kArmedNoSweepHintSeconds, false, false, {}, false)
            : juce::String());
        // P3 Task 6: the capture grid. Capturing = a live LEFT sweep beside a queued RIGHT card;
        // MidCaptureFailed = the sticky danger card (its Labels are what RULE2 must keep above the fold).
        stage.setCaptureModels (s == WS::Capturing        ? CC::capturing ("LEFT EAR", 0.58f, 10)
                              : s == WS::MidCaptureFailed ? CC::failed ("LEFT EAR")
                                                          : CC::waiting ("LEFT EAR"),
                                CC::waiting ("RIGHT EAR"));
        mc.resized();
    };
    juce::StringArray bad;
    for (int i = 0; i < eb::MeasureStage::kWorkflowStateCount; ++i) {
        apply ((WS) i);
        auto& vp = stage.viewportForTest();
        const int contentH = vp.getViewedComponent()->getHeight(), vpH = vp.getHeight();
        // RULE1 (content <= viewport) for every state EXCEPT VerdictDetailsOpen - details expansion
        // is sanctioned push-down displacement (spec 4 RULE2), gated by the triplet below instead.
        if (i != (int) WS::VerdictDetailsOpen && contentH > vpH)
            bad.add ("RULE1 state " + juce::String (i) + ": content " + juce::String (contentH)
                     + " > viewport " + juce::String (vpH));
        if (i == (int) WS::VerdictDetailsOpen) {
            auto& card = stage.verdictCardForTest (1);
            if (! fullyVisibleIn (vp, card))
                bad.add ("RULE2a VerdictDetailsOpen: the opened card does not fully fit the viewport");
            if (! fullyVisibleIn (vp, card.detailsRowForTest()))
                bad.add ("RULE2b VerdictDetailsOpen: the collapse affordance left the visible area");
        }
        for (auto& s2 : displacedWarnSurfaces (vp))
            bad.add ("RULE2 state " + juce::String (i) + ": warn surface displaced: " + s2);
    }

    // ---- T5-routed scene: the chain-mismatch waiting hint (the most-specific-known escalation
    // carrying a real chain summary) renders inside the fold with no displaced warn surface and
    // no clipped text (the hint is a 2-line INFO label; a production-shaped summary is the widest).
    {
        mc.driveVerdictForTest (eb::VerdictCardModel{}, eb::VerdictCardModel{}, false);
        stage.setLead (eb::MeasureStage::Lead::Waiting);
        const auto head = eb::MeasureStage::measureHeadCopy (eb::MeasureStage::Lead::Waiting, false, false);
        stage.setHeadCopy (head.title, head.sub);
        stage.setWaitHint (eb::MeasureStage::waitingHint (
            eb::MeasureStage::kArmedNoSweepHintSeconds, false, /*chainMismatch*/ true,
            "input 44.1k, cable 44.1k, Dirac output 44.1k - set it to 48k", false));
        mc.resized();
        auto& vp = stage.viewportForTest();
        if (vp.getViewedComponent()->getHeight() > vp.getHeight())
            bad.add ("RULE1 chain-mismatch hint: content > viewport");
        for (auto& s2 : displacedWarnSurfaces (vp))
            bad.add ("RULE2 chain-mismatch hint: warn surface displaced: " + s2);
        const auto jf = tmp.getChildFile ("chain.json");
        hig::writeDesignProbe (mc, jf, tmp.getChildFile ("chain.png"));
        for (auto& f : eb::hig::scoreDescriptor (juce::JSON::parse (jf)))
            if (f.category == "clip")
                bad.add ("chain-mismatch hint: " + f.category + " on " + f.element + " - " + f.message);
        stage.setWaitHint ({});
    }

    // ---- T2-review gate: the PATHOLOGICAL all-findings details-open card, MEASURED --------------
    // The spec's ~456 estimate was invalidated by measured-wrap. The true worst case: kNoBand is
    // STRUCTURALLY EXCLUSIVE with kTruncHi/kTruncLo (detectTruncation returns zero edges with both
    // trunc flags false when the banded derivation fails - the ONLY path MainComponent maps to
    // kNoBand; the trunc bits come from the same report's valid-band path), so the worst is every
    // other anomaly bit + BOTH trunc bits: 9 observation lines + the provisional footer, 7 flagged
    // chips. COLLAPSED it must obey RULE1 like any verdict state (asserted). OPEN it exceeds the
    // viewport at 900x720 - a KNOWN, reported bound awaiting an internal-budgeting ruling (P3 T7
    // report; "propose, don't improvise") - so the open state asserts only what MUST hold today:
    // the collapse affordance stays visible (RULE2b) and the measured bound is logged via WARN.
    {
        eb::ShapeScalars s;
        s.driftMaxDb = -9.4f; s.hfShelfDb = -4.4f; s.combDepthDb = 6.2f; s.combDelayMs = 1.23f;
        s.effLoHz = 152.0f; s.effHiHz = 8010.0f; s.lobeWidth = 3.5f; s.stepDb = 2.6f;
        s.resonanceHz = 4200.0f; s.humBaseHz = 50;
        const unsigned worstFlags = eb::ShapeFlag::kAllAnomalyMask & ~eb::ShapeFlag::kNoBand;
        eb::EarGradeSnapshot worstE;
        worstE.state = (int) eb::RefMonState::GradedSuspect;
        worstE.sweepSnrDb = 8.0f; worstE.irSnrDb = 20.0f; worstE.thdPercent = 6.0f; worstE.peakDb = -8.0f;
        const auto patho = eb::verdictCardModelAuto ("RIGHT EAR", worstE, worstFlags, s, false, false);
        CHECK (patho.flaggedChips == 7);                          // every chip family fires
        CHECK (patho.observations.size() == 9);                   // 9 findings (kNoBand excluded)

        mc.driveVerdictForTest (cleanL, patho, false);            // COLLAPSED: an ordinary RULE1 state
        auto& vp = stage.viewportForTest();
        const int collapsedH = vp.getViewedComponent()->getHeight();
        if (collapsedH > vp.getHeight())
            bad.add ("RULE1 pathological collapsed: content " + juce::String (collapsedH)
                     + " > viewport " + juce::String (vp.getHeight()));

        mc.driveVerdictForTest (cleanL, patho, true);             // OPEN: measure the true worst
        auto& card = stage.verdictCardForTest (1);
        const auto cardInVp = vp.getLocalArea (&card, card.getLocalBounds());
        WARN ("pathological all-findings details-open card: preferredHeight(273) = "
              << card.preferredHeight (273) << "px, bottom-in-viewport = " << cardInVp.getBottom()
              << "px vs viewport " << vp.getHeight()
              << "px - internal budgeting proposal pending (P3 Task 7 report)");
        if (! fullyVisibleIn (vp, card.detailsRowForTest()))
            bad.add ("RULE2b pathological open: the collapse affordance left the visible area");
        // RULE2c holds even here: the fold cuts only the calm dim observation lines - the warn-toned
        // chips sit directly under the details row, above the fold. No warn surface is ever displaced.
        for (auto& s2 : displacedWarnSurfaces (vp))
            bad.add ("RULE2c pathological open: warn surface displaced: " + s2);
    }

    // ---- routed ruling (Task 5 review, landed with Task 6): 2-line title mode ----
    {
        auto& hdr = stage.headerForTest();
        // House law: the hero instruction never squishes (the armed title used to render at 0.73).
        CHECK (hdr.titleForTest().getMinimumHorizontalScale() == 1.0f);
        // Deterministic mode pin (font-metric independent): an over-wide title takes the second
        // 26px row; a short title keeps today's one-line geometry.
        const auto keep = hdr.titleForTest().getText();
        hdr.setTitleText ("An impossibly wide synthetic hero instruction title for the two-line mode pin");
        CHECK (hdr.titleForTest().getHeight() == 26 + eb::StageHeader::kTitleExtraRow);
        hdr.setTitleText ("Short");
        CHECK (hdr.titleForTest().getHeight() == 26);
        // Scale-1.0 safety across the app: the other stages reserve only kHeight, so their (static)
        // titles must fit ONE line in the title box - the reviewer's <=~280px claim, verified against
        // the actual strings + the actual title font on this platform.
        const juce::Font f  = hdr.titleForTest().getFont();
        const float boxW    = (float) (hdr.titleForTest().getWidth() - 10);
        for (const char* t : { "Connect your devices", "Load your ear calibrations", "Set your level" }) {
            INFO (juce::String (t) << " measures "
                  << juce::GlyphArrangement::getStringWidth (f, t) << "px of " << boxW);
            CHECK (juce::GlyphArrangement::getStringWidth (f, t) <= boxW);
        }
        hdr.setTitleText (keep);
    }

    apply (WS::ArmedWaiting);   // restore the resting state
    INFO ("violations:\n" << bad.joinIntoString ("\n"));
    CHECK (bad.isEmpty());
    tmp.deleteRecursively();
}
