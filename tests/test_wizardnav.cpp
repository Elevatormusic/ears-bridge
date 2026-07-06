#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"
#include "gui/FormatCluster.h"       // P2.9 Task 5: the title-bar format cluster (pure builder + seam)
#include "gui/juce_design_probe.h"   // P2 Task 5: probe the showing/label set of the Calibrate stage
#include "state/Settings.h"          // P2.9 Task 7: seed the settings FILE through the real writer

// Task 4: navigation, tick wiring, focus & a11y. Headless MainComponent (hermetic TestConfig — never
// touches the real %APPDATA%/Settings/log dir, no network). A fresh temp profile has NO devices, so the
// launch/first-unmet resolution lands on Connect and Level/Measure are Blocked — the anchor for the pin
// legality tests below. These tests assert the wiring Task 4 adds on top of the pure WizardState machine
// (already exhaustively tested in test_wizardstate.cpp): stage forcing, CTA text/enablement, focus
// placement, the M1 gate helpText, and the H2 picker inner-combo titles.

namespace {
eb::MainComponent::TestConfig hermetic (juce::File tmp) {
    return eb::MainComponent::TestConfig { tmp, /*disableNetwork*/ true,
                                           tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") };
}
// Is `descendant` the same as, or nested under, `ancestor`?
bool isDescendantOf (const juce::Component* descendant, const juce::Component* ancestor) {
    for (auto* c = descendant; c != nullptr; c = c->getParentComponent())
        if (c == ancestor) return true;
    return false;
}
} // namespace

TEST_CASE("WizardNav fresh construct shows Connect (first unmet)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    CHECK (mc.shownStageForTest() == eb::WizardStep::Connect);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav pinning Calibrate is honored (never Blocked)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    CHECK (mc.shownStageForTest() == eb::WizardStep::Calibrate);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav pinning Measure with nothing configured falls back to Connect") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // Measure is Blocked in the empty profile (Connect+Calibrate not Done) -> an ordinary pin is illegal
    // and the machine resolves to first-unmet == Connect. (This uses the live-navigation seam, NOT
    // forceWizardStepForTest, which deliberately overrides pin legality for the gate.)
    mc.pinStepForTest (eb::WizardStep::Measure);
    CHECK (mc.shownStageForTest() == eb::WizardStep::Connect);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav Continue CTA text and disabled-while-Todo") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // Fresh: Connect is Todo (no devices) -> its Continue is disabled and reads "Continue to calibration".
    auto& cta = mc.connectContinueForTest();
    CHECK (cta.getButtonText() == juce::String ("Continue to calibration"));
    CHECK_FALSE (cta.isEnabled());

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav focus lands inside the shown stage on switch") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    // The documented seam: the first-focus target of the shown stage lives inside CalibrateStage.
    auto* target = mc.firstFocusTargetForTest();
    REQUIRE (target != nullptr);
    CHECK (isDescendantOf (target, &mc.calibrateStageForTest()));
    // minor-3: focus must land on a real control, never the scroll Viewport (which wants focus by default).
    CHECK (dynamic_cast<juce::Viewport*> (target) == nullptr);

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav M1 Start help text carries the gate reason when not ready") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // Empty profile: the gate is not ready, so Start's helpText names WHY (a nonempty reason).
    CHECK (mc.startButtonForTest().getHelpText().isNotEmpty());

    tmp.deleteRecursively();
}

TEST_CASE("WizardNav H2 device pickers carry inner-combo accessible titles") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    CHECK (mc.inputPickerForTest().comboTitleForTest() == juce::String ("Input device"));
    CHECK (mc.outputPickerForTest().comboTitleForTest() == juce::String ("Output virtual cable"));

    tmp.deleteRecursively();
}

// M-2: the Level green-band latch must be invalidated on an input-device change through BOTH the user
// pick and the hot-plug re-resolution. Both route through applyResolvedInput; this drives that shared
// path directly (the hot-plug lambda's exact member) and asserts a CHANGED key clears the latch while an
// UNCHANGED key leaves a still-valid latch alone. Guards the regression where a replug (or the EARS
// gain-DIP rename fallback) re-applied the input without clearing the latch, so Level kept claiming
// "In green band" on old-gain evidence.
TEST_CASE("WizardNav levelLatched clears when the applied input key changes (M-2)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (960, 780);

    // A distinct input device (its own uid -> its own key()). Applying it must clear a set latch.
    eb::DeviceId dev; dev.typeName = "Windows Audio"; dev.name = "EARS"; dev.uid = "uid-A";
    mc.applyResolvedInputForTest (dev);   // seed the memo to dev's key
    mc.setLevelLatchedForTest (true);

    // Same device again -> key unchanged -> the still-valid latch survives.
    mc.applyResolvedInputForTest (dev);
    CHECK (mc.levelLatchedForTest());

    // A genuinely different device -> key changes -> the latch is invalidated.
    eb::DeviceId dev2; dev2.typeName = "Windows Audio"; dev2.name = "EARS Pro"; dev2.uid = "uid-B";
    mc.applyResolvedInputForTest (dev2);
    CHECK_FALSE (mc.levelLatchedForTest());

    tmp.deleteRecursively();
}

// The recorded carryover: calBuilding causes a TRANSIENT auto-navigation risk. When the user has pinned
// Measure and an async FIR rebuild flips Level/Measure to Blocked, the machine's ws.active would resolve
// back to Calibrate/Connect and yank the view off Measure — a flicker. resolveShownStage HOLDS the pinned
// stage across that transient (pin is Blocked in ws but NOT in the calBuilding-masked state), while the
// spine still renders the rebuild truthfully. This tests the PURE hold decision directly (the honest unit
// — a fully-configured L+R+cal device stack cannot be fabricated in a hermetic no-device test env).
TEST_CASE("WizardNav resolveShownStage holds a pin blocked only by calBuilding") {
    // Build the two states the app would compute at the transient: everything satisfied, Measure pinned.
    eb::WizardInputs base;
    base.haveDevs = true; base.haveCals = true; base.gateReady = true;
    base.engineRunning = true; base.levelLatched = true;
    base.configGen = 1; base.verdictGenL = 1; base.verdictGenR = 1;
    base.earGradedL = true; base.earGradedR = true;

    // ws: calBuilding ON -> Calibrate Todo(rebuilding), Level+Measure Blocked; pinned Measure is illegal so
    // ws.active resolves to first-unmet (Calibrate). masked: calBuilding OFF -> Measure legal (Done).
    auto withBuild = base;    withBuild.calBuilding = true;
    const auto ws     = eb::computeWizardState (withBuild, eb::WizardStep::Measure);
    const auto masked = eb::computeWizardState (base,      eb::WizardStep::Measure);

    REQUIRE (ws.active != eb::WizardStep::Measure);                                  // machine yanked away
    REQUIRE (ws.steps[(int) eb::WizardStep::Measure].state == eb::StepState::Blocked);
    REQUIRE (masked.steps[(int) eb::WizardStep::Measure].state != eb::StepState::Blocked);

    // The view rule holds Measure across the transient.
    CHECK (eb::MainComponent::resolveShownStage (ws, eb::WizardStep::Measure, masked)
               == eb::WizardStep::Measure);

    // Non-transient illegality (genuinely blocked even with the rebuild masked) is NOT held.
    eb::WizardInputs empty;   // no devices/cals -> Measure Blocked both with and without calBuilding
    const auto wsEmpty     = eb::computeWizardState (empty, eb::WizardStep::Measure);
    const auto maskedEmpty = eb::computeWizardState (empty, eb::WizardStep::Measure);
    CHECK (eb::MainComponent::resolveShownStage (wsEmpty, eb::WizardStep::Measure, maskedEmpty)
               == wsEmpty.active);
}

// P2 Task 5: exactly ONE guidance caption on the Calibrate stage (spec 5.2 - the HEQ note used
// to render per card), and the Advanced-FIR section genuinely discloses (hidden controls are
// out of the probe's showing set; open brings them back).
TEST_CASE("Calibrate stage: one guidance caption; Advanced FIR discloses") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.forceWizardStepForTest (eb::WizardStep::Calibrate);
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");

    auto countShowing = [&] (const juce::String& needle) {
        hig::writeDesignProbe (mc, jf, pf);
        const auto tree = juce::JSON::parse (jf);
        int n = 0;
        if (const auto* els = tree.getProperty ("elements", {}).getArray())
            for (auto& e : *els)
                if ((bool) e.getProperty ("showing", false)
                    && e.getProperty ("label", {}).toString().contains (needle))
                    ++n;
        return n;
    };

    CHECK (countShowing ("HEQ files for headphones") == 1);   // the ONE caption, exactly once
    CHECK (countShowing ("FIR LENGTH") == 0);                 // collapsed by default (all-default settings)
    mc.calibrateStageForTest().setAdvancedOpen (true);
    mc.resized();
    CHECK (countShowing ("FIR LENGTH") == 1);                 // disclosed
    mc.calibrateStageForTest().setAdvancedOpen (false);
    mc.resized();
    CHECK (countShowing ("FIR LENGTH") == 0);                 // and collapses again
    tmp.deleteRecursively();
}

// Map #9/#10: the header run-note mirrors the machine's first-unmet reason and clears on Done.
TEST_CASE("Calibrate stage: run-note carries the machine reason while not Done") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.pinStepForTest (eb::WizardStep::Calibrate);
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");
    hig::writeDesignProbe (mc, jf, pf);
    // Pin THE header's own run-note element (componentID "calRunNote"), not just "some showing element
    // whose label matches" - the spine's per-step meta Label has carried the same machine reason since
    // P1, so a label-only match stays green even if the header wiring is deleted. Find the element by
    // its id, then assert ITS label is the machine reason.
    // Bind the parsed tree to a named local before taking getArray() - the array pointer is owned by
    // the parsed var, so `JSON::parse(jf).getProperty(...).getArray()` would dangle at the end of the
    // full expression (the idiom every other probe test uses: hold `tree`, then iterate).
    const auto tree = juce::JSON::parse (jf);
    bool foundRunNote = false, runNoteHasReason = false;
    if (const auto* els = tree.getProperty ("elements", {}).getArray())
        for (auto& e : *els)
            if (e.getProperty ("id", {}).toString() == "calRunNote"
                && (bool) e.getProperty ("showing", false)) {
                foundRunNote = true;
                runNoteHasReason = e.getProperty ("label", {}).toString() == eb::kReasonNoCals();
            }
    CHECK (foundRunNote);        // the header's run-note element is present + showing
    CHECK (runNoteHasReason);    // and IT (not some sibling) carries the machine reason
    tmp.deleteRecursively();
}

// P2 Task 6 (spec 5.2 unity path + map #8): continue-without-calibration is an EXPLICIT,
// session-scoped choice: it completes Calibrate, retires its own button, keeps the warning
// visible as evidence - and ANY cal load revokes it (with the Required line lighting up on
// the still-empty sibling).
TEST_CASE("Unity path: explicit accept completes Calibrate; a cal load revokes it") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.pinStepForTest (eb::WizardStep::Calibrate);
    auto& stage = mc.calibrateStageForTest();

    // Fresh: both empty -> hint + button offered; CTA disabled (Calibrate Todo).
    REQUIRE (stage.unityButtonForTest().isVisible());
    CHECK_FALSE (stage.continueButton().isEnabled());

    stage.unityButtonForTest().onClick();                     // the REAL wired path
    // Accepted: Calibrate is Done (unity), the CTA enables, the button retires, the hint stays.
    CHECK (stage.continueButton().isEnabled());
    CHECK_FALSE (stage.unityButtonForTest().isVisible());

    // Revocation: loading ONE cal resets the choice AND masks it (noCalsLoaded false).
    auto cal = juce::File::createTempFile (".txt");
    cal.replaceWithText ("* HEQ\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (mc.leftCalForTest().loadFromFile (cal));
    cal.deleteFile();
    CHECK_FALSE (stage.continueButton().isEnabled());         // one file != a valid applied pair
    CHECK_FALSE (stage.unityButtonForTest().isVisible());     // not both-empty any more
    // Map #8b/#10: the still-empty RIGHT card now claims Required (Start is truly blocked).
    auto* req = mc.rightCalForTest().findChildWithID ("calDzReq");
    REQUIRE (req != nullptr);
    CHECK (req->isVisible());
    // And clearing back to empty does NOT silently resurrect the old acceptance.
    mc.leftCalForTest().clearCal();
    CHECK (stage.unityButtonForTest().isVisible());           // offered again - not auto-applied
    CHECK_FALSE (stage.continueButton().isEnabled());
    tmp.deleteRecursively();
}

// P2 T6 decision (revoke-on-ATTEMPT): a FAILED cal load - parse error / missing / oversize - is still an
// attempt to calibrate, so it revokes a session's unity acceptance exactly like a successful load. A red
// "load failed" card must NEVER co-exist with a "Done (unity)" Calibrate. (The success case is covered by
// the test above; this one pins the FAILED path, which onXCalLoaded's success-only reset does not reach.)
TEST_CASE("Unity path: a FAILED cal-load attempt revokes the acceptance") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.pinStepForTest (eb::WizardStep::Calibrate);
    auto& stage = mc.calibrateStageForTest();

    // Accept unity through the REAL wired button path -> Calibrate Done (unity), the CTA enables, the
    // button retires.
    REQUIRE (stage.unityButtonForTest().isVisible());
    stage.unityButtonForTest().onClick();
    REQUIRE (stage.continueButton().isEnabled());             // Calibrate Done via unity
    REQUIRE_FALSE (stage.unityButtonForTest().isVisible());

    // Attempt to load an INVALID file (a missing path - onLoadAttempted fires, then the load returns false).
    // This is the failed-attempt idiom the #38 stale-path test uses.
    const juce::File gone ("Z:/definitely/not/here/L_HEQ_000-0000.txt");
    REQUIRE_FALSE (mc.leftCalForTest().loadFromFile (gone));

    // Revoked: the unity acceptance is gone even though the load FAILED. The hint + the
    // "Continue without calibration" button are offered again, and Calibrate is no longer Done via unity.
    CHECK (stage.unityButtonForTest().isVisible());           // the choice is offered again, not auto-kept
    CHECK_FALSE (stage.continueButton().isEnabled());         // Calibrate NOT Done (unity revoked, no cal applied)
    tmp.deleteRecursively();
}

// Cold-verifier MAJOR-1 (mask coverage): `in.unityAccepted = unityAcceptedSession_ && gate.noCalsLoaded`
// was mutation-uncovered because every PUBLIC route resets the flag before a cal loads (removing the
// `&& gate.noCalsLoaded` AND-term left all 670 green). This pins the AND-term DIRECTLY and deterministically:
// force a stale acceptance (setUnityAcceptedForTest) with a cal LOADED (noCalsLoaded false), then read the
// live-snapshot unityAccepted (snapshotUnityAcceptedForTest) - the mask must have zeroed it. Reading the
// snapshot value avoids the async FIR build entirely (resolveCalibrate would short-circuit on calBuilding
// while the build is in flight, so a state-level assertion could not exercise this line headlessly).
// Verified RED under a temporary mutation (see below) then restored.
TEST_CASE("Unity mask: a stale acceptance is inert once a cal is loaded") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.pinStepForTest (eb::WizardStep::Calibrate);
    auto& stage = mc.calibrateStageForTest();

    // Sanity: with BOTH slots empty and NO acceptance, unityAccepted is false (nothing to mask yet).
    REQUIRE_FALSE (mc.snapshotUnityAcceptedForTest());

    // Load ONE valid cal -> noCalsLoaded becomes false. The pair is incomplete, so it never becomes haveCals.
    auto cal = juce::File::createTempFile (".txt");
    cal.replaceWithText ("* HEQ\n20 0.0\n100 1.0\n1000 2.0\n10000 -1.0\n20000 -3.0\n");
    REQUIRE (mc.leftCalForTest().loadFromFile (cal));
    cal.deleteFile();

    // Set the stale flag (a hypothetical missed reset) and refresh through the live path.
    mc.setUnityAcceptedForTest (true);
    mc.pinStepForTest (eb::WizardStep::Calibrate);   // refreshWizardView() -> re-snapshot + re-render

    // The AND-mask keeps the stale flag inert: with a cal loaded, gate.noCalsLoaded is false, so the live
    // snapshot's unityAccepted is false REGARDLESS of the raw flag. Deleting `&& gate.noCalsLoaded` flips this
    // to true -> RED (verified by temporary mutation, then restored). This is the direct pin of the AND-term.
    CHECK_FALSE (mc.snapshotUnityAcceptedForTest());

    // Corroborating view surfaces: the unity affordance is gone (not both-empty) and the CTA is not enabled by
    // the stale flag (Calibrate is not Done-via-unity - the mask neutralised it; one cal is not a valid pair).
    CHECK_FALSE (stage.unityButtonForTest().isVisible());
    CHECK_FALSE (stage.continueButton().isEnabled());

    // And the spine's Calibrate meta must NOT read the unity string - the "No calibration (unity)" summary is
    // stamped ONLY when both slots are empty, so a stale flag can never surface it with a cal loaded (this
    // guard is independent of the AND-mask, but it confirms no unity wording leaks into the loaded-cal view).
    const auto jf = tmp.getChildFile ("mask.json");
    const auto pf = tmp.getChildFile ("mask.png");
    hig::writeDesignProbe (mc, jf, pf);
    const auto tree = juce::JSON::parse (jf);
    bool sawUnityString = false;
    if (const auto* els = tree.getProperty ("elements", {}).getArray())
        for (auto& e : *els)
            if ((bool) e.getProperty ("showing", false)
                && e.getProperty ("label", {}).toString().containsIgnoreCase ("No calibration (unity)"))
                sawUnityString = true;
    CHECK_FALSE (sawUnityString);
    tmp.deleteRecursively();
}

// P2 Task 7: the non-Dirac override lives behind "Not using Dirac?" - hidden until disclosed,
// but NEVER hideable while ON (the lock). Also: the Connect run-note mirrors the machine reason.
TEST_CASE("Connect stage: override discloses, and locks open while the override is ON") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    auto& row = mc.connectStageForTest().notUsingDiracForTest();
    auto& toggle = mc.overrideToggleForTest();

    // NOTE: Component::isShowing() is peer-dependent (always false headless) - assert on
    // isVisible() + non-empty bounds, which is what layoutContent actually drives.
    CHECK_FALSE (row.isOpen());                               // collapsed by default (override off)
    CHECK_FALSE (toggle.isVisible());
    row.clickForTest();
    CHECK (toggle.isVisible());                               // disclosed
    CHECK (! toggle.getBounds().isEmpty());
    row.clickForTest();
    CHECK_FALSE (toggle.isVisible());                         // collapsible while OFF

    row.clickForTest();                                       // open again, then arm the override
    toggle.setToggleState (true, juce::sendNotification);     // fires the real onClick wiring
    CHECK (row.isOpen());
    row.clickForTest();                                       // attempt to hide the armed override
    CHECK (row.isOpen());                                     // REFUSED: locked while ON
    CHECK (toggle.isVisible());
    toggle.setToggleState (false, juce::sendNotification);
    row.clickForTest();
    CHECK_FALSE (row.isOpen());                               // unlocked once OFF
    tmp.deleteRecursively();
}

// P2 Task 7 review (Fix 1): on a TALL window the ConnectStage content is clamped to the viewport
// height, so content_.setSize() is a no-op across a relayout that grows a card. Content::paint draws
// the card fills from groupRects_, so a stale rect would render the old inter-card gap as a background
// stripe across the grown SIGNAL PATH card. The stripe itself is paint-path (a headless test can't see
// pixels - proven by render), but the HONEST proxy is that the geometry the paint READS is fresh: after
// forcing diracCableHint visible + relayout on a tall stage, the SIGNAL PATH card rect must have GROWN.
// (The delta-repaint in layoutContent then invalidates content_ so the paint reads THIS fresh rect.)
TEST_CASE("Connect stage: SIGNAL PATH card rect stays fresh when the cable hint appears (tall window)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 1400);                                   // TALL: viewport height > content height
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    auto& stage = mc.connectStageForTest();

    // Deterministically drive the SIGNAL PATH card's hosted-hint state (do not rely on the ctor default -
    // the adopted label's visibility is whatever updateDiracCableHint last set). HIDDEN baseline first.
    auto* hint = stage.diracCableHintForTest();
    REQUIRE (hint != nullptr);
    hint->setVisible (false);
    stage.resized();
    REQUIRE (stage.groupRectsForTest().size() >= 1u);
    const auto cardBefore = stage.groupRectsForTest().front();

    // Now the standard VB-CABLE state: the hint appears, and the layout must grow card 0 to host it.
    hint->setText ("The Hi-Fi Cable connects to Dirac but won't carry audio through it.",
                   juce::dontSendNotification);
    hint->setVisible (true);
    stage.resized();

    const auto cardAfter = stage.groupRectsForTest().front();
    // The paint reads groupRects_; if it stayed stale the height would be unchanged. It must have grown by
    // the hint row - proving the geometry the fill paints from is fresh (not the pre-hint rect).
    CHECK (cardAfter.getHeight() > cardBefore.getHeight());
    // Same top-left origin: the card grew DOWNWARD, it did not shift (a sanity that we compared the same card).
    CHECK (cardAfter.getX() == cardBefore.getX());
    CHECK (cardAfter.getY() == cardBefore.getY());
    tmp.deleteRecursively();
}

// P2 Task 7 review (Fix 2): the honesty lock's PERSISTED-ON RESTORE path. The live onClick path is
// covered above; this pins the OTHER entry - a settings file with advancedOverride pre-persisted ON,
// so at construction MainComponent restores the toggle ON and syncOverrideDisclosure() opens the row
// LOCKED. A ctor reordering (adopt/sync out of order, or the restore sync dropped) would break this
// silently. RED reason: with the post-adopt connectStage_.syncOverrideDisclosure() removed, the row
// constructs COLLAPSED at launch, so the three PRE-click CHECKs (row not open, toggle hidden, empty
// bounds) fail - that is where the test goes red. The post-click CHECKs would still PASS in the
// broken build (clickForTest merely OPENS the collapsed row) - they guard the lock's
// refuse-collapse behavior, NOT the restore; the PRE-click CHECKs are the restore guard.
TEST_CASE("Connect stage: a persisted-ON override restores the disclosure locked open at launch") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    // Pre-persist the override ON in the SAME backing file MainComponent's Settings will read (the
    // test_settings.cpp idiom: construct Settings over the dir, set + flush, then re-open in the app).
    {
        eb::Settings seed (tmp);
        seed.setAdvancedOverride (true);
        REQUIRE (seed.flush());
    }
    eb::MainComponent mc (hermetic (tmp));                    // settingsDir == tmp -> reads the seeded ON state
    mc.setSize (900, 780);
    mc.forceWizardStepForTest (eb::WizardStep::Connect);
    auto& row = mc.connectStageForTest().notUsingDiracForTest();
    auto& toggle = mc.overrideToggleForTest();

    // Restored ON at launch: the toggle is checked + visible, and the row opened LOCKED (no click needed).
    CHECK (toggle.getToggleState());                         // persisted ON survived the reload
    CHECK (row.isOpen());                                    // disclosure force-opened by the restore sync
    CHECK (toggle.isVisible());                              // disclosed content is laid out (non-collapsed)
    CHECK (! toggle.getBounds().isEmpty());

    // The lock holds against a collapse attempt (the honesty invariant, exercised on the RESTORE path).
    row.clickForTest();
    CHECK (row.isOpen());                                    // REFUSED: an armed override is never hideable
    CHECK (toggle.isVisible());
    tmp.deleteRecursively();
}

// Map #9/#10 (Connect): the header run-note mirrors the machine's first-unmet reason and clears
// on Done. Pin THE header's own run-note element by componentID ("connectRunNote"), not just "some
// showing element whose label matches" - the spine's per-step meta Label has carried the same
// machine reason since P1, so a label-only match would stay green even if the header wiring were
// deleted (the exact Task-5-review lesson for the Calibrate run-note). Find the element by its id,
// then assert ITS label is the machine reason.
TEST_CASE("Connect stage: run-note carries the machine reason while not Done") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 780);
    const auto jf = tmp.getChildFile ("d.json");
    const auto pf = tmp.getChildFile ("d.png");
    hig::writeDesignProbe (mc, jf, pf);                       // launch lands on Connect (first unmet)
    // Bind the parsed tree to a named local before getArray() - the array pointer is owned by the
    // parsed var, so parse(jf).getProperty(...).getArray() would dangle at the end of the expression.
    const auto tree = juce::JSON::parse (jf);
    bool foundRunNote = false, runNoteHasReason = false;
    if (const auto* els = tree.getProperty ("elements", {}).getArray())
        for (auto& e : *els)
            if (e.getProperty ("id", {}).toString() == "connectRunNote"
                && (bool) e.getProperty ("showing", false)) {
                foundRunNote = true;
                runNoteHasReason = e.getProperty ("label", {}).toString() == eb::kReasonNoDevices();
            }
    CHECK (foundRunNote);        // the header's run-note element is present + showing
    CHECK (runNoteHasReason);    // and IT (not some sibling) carries the machine reason
    tmp.deleteRecursively();
}

TEST_CASE("P2.9 chrome: the format cluster tracks live settings and the brand word-mark is gone") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (900, 720);
    // Defaults: 48k / 24-bit / Auto per-ear (Settings defaults) -> the cluster carries them.
    // (The cluster is painted; assert through its parts seam via a re-set + the pure builder.)
    auto p = eb::formatClusterParts (48000.0, 24, eb::CombineMode::AutoPerEar);
    CHECK (p[0] == "48 kHz");
    CHECK (mc.fmtClusterForTest().getTitle() == "Session format");
    CHECK (! mc.fmtClusterForTest().getBounds().isEmpty());   // laid out in the bar (headless-safe assertion)
    tmp.deleteRecursively();
}

// P2.9 T7: the launch-path negatives the hermetic scenes never had. The disclosure open/closed
// decision is seeded at construction AFTER the persisted cals load (MainComponent.cpp:522-525 then
// :549) - a path the header/component tests never drove. These two lock the frozen contract on the
// REAL construction ordering: cals restoring must NOT open the disclosure (all knobs default), and a
// stored non-default knob MUST. If the first ever fails (disclosure opens with all-default settings),
// the walkthrough bug has reproduced under test and its mechanism must be root-caused here first.
TEST_CASE("P2.9 disclosure seed: launches CLOSED with all-default settings even when cals load at startup") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    // Pre-seed the settings FILE the way the live app finds it - through the REAL Settings writer
    // (never hand-written XML): cal paths present, every advanced knob left at its default.
    const juce::File data (EB_TEST_DATA_DIR);
    {
        eb::Settings seed (tmp);
        seed.setLeftCalPath  (data.getChildFile ("L_HEQ_0000000.txt").getFullPathName());
        seed.setRightCalPath (data.getChildFile ("R_HEQ_0000000.txt").getFullPathName());
        REQUIRE (seed.flush());   // through the real writer; props().save() is private (test_settings idiom)
    }
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    CHECK (mc.leftCalForTest().hasCal());                      // the cals really loaded (live-app delta present)
    CHECK_FALSE (mc.calibrateStageForTest().advancedOpenForTest());   // and the disclosure stayed CLOSED
    tmp.deleteRecursively();
}

TEST_CASE("P2.9 disclosure seed: launches OPEN when a stored advanced setting is non-default") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    {
        eb::Settings seed (tmp);
        seed.setOutputTrimDb (-6.0);
        REQUIRE (seed.flush());   // through the real writer; props().save() is private (test_settings idiom)
    }
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    CHECK (mc.calibrateStageForTest().advancedOpenForTest());
    tmp.deleteRecursively();
}

TEST_CASE("P2.9 combine combo: the recommended badge rides shortcutKeyText, not triple-space text") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    int recommendedBadges = 0;
    juce::PopupMenu::MenuItemIterator it (*mc.combineBoxForTest().getRootMenu());
    while (it.next()) {
        auto& item = it.getItem();
        CHECK (! item.text.contains ("(recommended)"));
        if (item.shortcutKeyDescription == "recommended") { ++recommendedBadges; CHECK (item.text == "Auto per-ear (Dirac)"); }   // JUCE Item field -> LnF shortcutKeyText param
    }
    CHECK (recommendedBadges == 1);
    CHECK (mc.combineBoxForTest().getText() == "Auto per-ear (Dirac)");   // default restore path intact
    tmp.deleteRecursively();
}
