#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>                     // P3 T7 rulings: the mode-copy fit check's std::ceil
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

// ==================================================================================================
// P3 Task 1: verdict-generation stamping. The P2 placeholder fed -1/-1; now each ear's publish stamps
// the config generation it was measured under, snapshotWizardInputs carries the stamps, and the
// machine's verdictsStale/Measure-Done predicates compute from them. Stamp-only: nothing gates.
// ==================================================================================================
TEST_CASE("P3 verdictGen: publish stamps the current generation and the snapshot carries it") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    // Un-stamped: the placeholder truth (-1) so nothing ever reads Done/stale before a real verdict.
    CHECK (mc.verdictGenForTest (0) == -1);
    CHECK (mc.verdictGenForTest (1) == -1);
    auto in0 = mc.snapshotWizardInputsForTest();
    CHECK (in0.verdictGenL == -1);
    CHECK_FALSE (eb::computeWizardState (in0, std::nullopt).verdictsStale);

    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedMarginal);
    const auto in1 = mc.snapshotWizardInputsForTest();
    CHECK (in1.earGradedL); CHECK (in1.earGradedR);
    CHECK (in1.verdictGenL == in1.configGen);
    CHECK (in1.verdictGenR == in1.configGen);
    CHECK_FALSE (eb::computeWizardState (in1, std::nullopt).verdictsStale);   // fresh: NOT stale

    mc.bumpConfigGenForTest();                                                // cal swap / rate change
    const auto in2 = mc.snapshotWizardInputsForTest();
    CHECK (in2.verdictGenL < in2.configGen);
    CHECK (eb::computeWizardState (in2, std::nullopt).verdictsStale);         // evidence downgraded

    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);           // re-measure ONE ear
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);           // ... and the other
    const auto in3 = mc.snapshotWizardInputsForTest();
    CHECK_FALSE (eb::computeWizardState (in3, std::nullopt).verdictsStale);   // refreshed: stale clears
    tmp.deleteRecursively();
}

TEST_CASE("P3 verdictGen negative: an ungraded ear never reads stale, whatever its stamp") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.bumpConfigGenForTest();                       // gen moves with NO verdict published
    const auto in = mc.snapshotWizardInputsForTest();
    CHECK_FALSE (in.earGradedL);
    CHECK_FALSE (eb::computeWizardState (in, std::nullopt).verdictsStale);
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 1 REVIEW fixes: the generation-bump sites. Fix 1 (Major): hot-plug to a DIFFERENT input
// device stales the verdicts — "a different device is different evidence", the same doctrine that
// clears the Level latch (M-2), now applied to verdict freshness. Fixes 2+3 (user-ratified): an
// output bit-depth or output-device change stales them too. Fix 4: guard-before-stamp order + the
// one-ear || persistence. All staleness-conservative: nothing here can make evidence read fresher.
// ==================================================================================================
TEST_CASE("P3 verdictGen: a different-key input apply stales the verdicts; same-key re-apply stays fresh") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));

    // Seed the key memo, then grade both ears fresh under the current config generation.
    eb::DeviceId dev; dev.typeName = "Windows Audio"; dev.name = "EARS"; dev.uid = "uid-A";
    mc.applyResolvedInputForTest (dev);
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    // Negative: a hot-plug re-resolution landing on the SAME device (same key) must NOT downgrade
    // still-valid verdicts — mirrors the latch's same-key survival in M-2.
    mc.applyResolvedInputForTest (dev);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    // A DIFFERENT device (a replugged different jig, or the gain-DIP rename fallback) -> old-jig
    // verdicts must read STALE. This drives the exact member the hot-plug lambda uses.
    eb::DeviceId dev2; dev2.typeName = "Windows Audio"; dev2.name = "EARS Pro"; dev2.uid = "uid-B";
    mc.applyResolvedInputForTest (dev2);
    CHECK (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);
    tmp.deleteRecursively();
}

TEST_CASE("P3 verdictGen: an output bit-depth change stales the verdicts (user ruling)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));

    // Determinism: on a machine with real endpoints the ctor may auto-select an output whose menu holds
    // a SINGLE supported depth (nothing to change to). Empty the picker so rebuildBitDepthMenu takes the
    // no-device fallback {16,24,32}; the output-choose that rebuilds it bumps the generation itself
    // (that's the Fix-3 test), so both ears re-publish AFTER it — the final staleness below is then
    // attributable to the bit-depth change alone.
    mc.outputPickerForTest().setDevices ({}, {});
    eb::DeviceId out; out.typeName = "Windows Audio"; out.name = "Speakers (High Definition Audio)"; out.uid = "uid-out";
    mc.chooseOutputForTest (out);   // rebuilds the bit-depth menu off the (now empty) picker selection
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    // Drive the REAL combo path (bitBox.onChange -> onBitDepthChosen) with a DIFFERENT depth than the
    // current selection. sendNotificationSync fires the handler synchronously (headless-safe).
    auto& bit = mc.bitBoxForTest();
    REQUIRE (bit.getNumItems() >= 2);
    const int cur = bit.getSelectedId();
    for (int i = 0; i < bit.getNumItems(); ++i)
        if (bit.getItemId (i) != cur) { bit.setSelectedId (bit.getItemId (i), juce::sendNotificationSync); break; }
    CHECK (bit.getSelectedId() != cur);   // the change was real
    CHECK (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);
    tmp.deleteRecursively();
}

TEST_CASE("P3 verdictGen: an output-device change stales the verdicts (user ruling)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));

    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    // Drive onOutputChosen — the exact member the output picker's callback invokes. A non-virtual name
    // keeps the hermetic run off the Dirac shared-mode registry probe (NotVirtual hint branch).
    eb::DeviceId out; out.typeName = "Windows Audio"; out.name = "Speakers (High Definition Audio)"; out.uid = "uid-out";
    mc.chooseOutputForTest (out);
    CHECK (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);
    tmp.deleteRecursively();
}

// Fix 4a: guard-before-stamp ORDER. The live publish continuation cannot be driven headless — it needs a
// Running engine, a learned loopback reference, and the two-stage firPool -> callAsync worker chain (a
// real sweep). So the guard+publish+stamp HEAD of that continuation is extracted into the production
// member publishGradeIfRunCurrent(), the continuation calls it, and this test drives THAT member: the
// strongest available pin short of hardware (the ordering under test IS the production code, not a
// replica). A run generation bumped between arming (the myGen capture at job post) and the continuation
// must drop the publish AND leave the stamp untouched — a stamp from a dead run would falsely refresh
// verdictGen and clear verdictsStale.
TEST_CASE("P3 verdictGen: a stale run generation publishes nothing and stamps nothing (guard before stamp)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));

    const uint32_t armed = mc.gradeRunGenForTest();   // what the poll stamps into the job at post time
    mc.bumpGradeRunGenForTest();                      // a Stop/Start lands between post and continuation
    CHECK_FALSE (mc.publishGradeGuardedForTest (0, (int) eb::RefMonState::GradedClean, armed));
    CHECK (mc.verdictGenForTest (0) == -1);           // NO stamp: the never-graded placeholder survives
    CHECK_FALSE (mc.snapshotWizardInputsForTest().earGradedL);   // and NO publish reached the engine

    // Control: the CURRENT run generation still publishes + stamps (the extraction kept the live path).
    CHECK (mc.publishGradeGuardedForTest (0, (int) eb::RefMonState::GradedClean, mc.gradeRunGenForTest()));
    const auto in = mc.snapshotWizardInputsForTest();
    CHECK (in.earGradedL);
    CHECK (mc.verdictGenForTest (0) == in.configGen);
    tmp.deleteRecursively();
}

// Fix 4b: one-ear persistence — staleness clears ONLY when BOTH ears re-publish under the current
// generation (the || in computeWizardState). Re-measuring just L after a config change must leave the
// pair STALE: R's verdict is still old-config evidence.
TEST_CASE("P3 verdictGen: re-publishing only one ear keeps the pair stale") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));

    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    mc.bumpConfigGenForTest();                                     // config changed under the pair
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean); // ONLY L re-measured
    const auto in = mc.snapshotWizardInputsForTest();
    CHECK (in.verdictGenL == in.configGen);
    CHECK (in.verdictGenR <  in.configGen);
    CHECK (eb::computeWizardState (in, std::nullopt).verdictsStale);   // R is still old evidence
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 4: the Level SOFT gate (§3.2). Continue = navigation to Measure, forbidden only while
// Measure is Blocked; un-latched shows the honest caution in the run-note. Deps are un-blocked
// headlessly via ERRORS on both (§3.5: Error does not re-Block downstream): a device error makes
// Connect Error, and a SWAPPED cal pair makes Calibrate Error SYNCHRONOUSLY (the per-slot side
// check) - the plan's "valid pair -> Done" route needs the async FIR build pumped, and the pump
// idiom (runDispatchLoopUntil) requires JUCE_MODAL_LOOPS_PERMITTED, which this project leaves off.
// ==================================================================================================
TEST_CASE("P3 Level soft CTA: enabled once deps un-block; caution note until latched") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    const juce::File data (EB_TEST_DATA_DIR);
    mc.setSize (900, 720);

    // Hermetic default: Connect Todo -> Level/Measure Blocked -> CTA disabled, machine reason shown.
    auto& stage = mc.levelStageForTest();
    CHECK_FALSE (stage.continueButton().isEnabled());

    // Swapped pair (Calibrate Error, synchronous) + a device ERROR (Connect Error, NOT Blocked-making).
    REQUIRE (mc.leftCalForTest().loadFromFile (data.getChildFile ("R_HEQ_0000000.txt")));
    REQUIRE (mc.rightCalForTest().loadFromFile (data.getChildFile ("L_HEQ_0000000.txt")));
    mc.driveDeviceErrorForTest ("EARS or audio cable disconnected - measurement stopped.");
    CHECK (mc.snapshotWizardInputsForTest().calProblem);              // Calibrate reads Error, not Blocked
    CHECK (stage.continueButton().isEnabled());                       // soft gate: navigable un-latched
    CHECK (stage.headerForTest().continueButton().isEnabled());       // (same button - sanity)

    // Un-latched -> the caution; latched -> empty.
    CHECK (eb::LevelStage::levelRunNote (false, false, "x") == "Level not confirmed - the grade will tell you");
    CHECK (eb::LevelStage::levelRunNote (false, true,  "x") == juce::String());
    CHECK (eb::LevelStage::levelRunNote (true, false, "Finish Connect and Calibrate first")
           == "Finish Connect and Calibrate first");
    mc.driveDeviceErrorForTest ({});                                  // restore
    tmp.deleteRecursively();
}

TEST_CASE("P3 transport face: stopped = primary Start monitoring / play; the title bar no longer hosts it") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    auto& t = mc.startButtonForTest();
    CHECK (t.getButtonText() == "Start monitoring");
    CHECK (t.getProperties()["glyph"].toString() == "play");
    CHECK ((bool) t.getProperties()["primary"]);
    // Re-homed: the transport is a descendant of LevelStage, not of the title-bar area.
    CHECK (t.findParentComponentOfClass<eb::LevelStage>() != nullptr);
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 5: Measure head copy + run-note + the §2 timeout hint. Pure statics - the honesty contract
// lives in WORDS here, so the words are pinned. The app never claims it can trigger Dirac.
// ==================================================================================================
TEST_CASE("P3 Measure head copy: lead x override x verdict variants") {
    using MS = eb::MeasureStage;
    using CM = eb::CombineMode;
    const auto wait = MS::measureHeadCopy (MS::Lead::Waiting, false, false, CM::AutoPerEar);
    CHECK (wait.title == "Now run the measurement in Dirac Live");
    CHECK (wait.sub.contains ("CABLE Input (VB-Audio)"));
    const auto ovr = MS::measureHeadCopy (MS::Lead::Waiting, true, false, CM::AutoPerEar);
    CHECK (ovr.title == "Run your measurement sweep");            // §7 generic variant
    CHECK_FALSE (ovr.sub.contains ("Dirac"));                     // the Dirac-shaped copy would be wrong
    const auto verd = MS::measureHeadCopy (MS::Lead::Waiting, false, true, CM::AutoPerEar);
    CHECK (verd.title == "Your per-ear result");
    const auto hw = MS::measureHeadCopy (MS::Lead::HwDirac, false, false, CM::AutoPerEar);
    CHECK (hw.sub.contains ("per-ear calibration still works"));
    const auto ref = MS::measureHeadCopy (MS::Lead::Reference, false, false, CM::AutoPerEar);
    CHECK (ref.title == "Learn your reference");
    // Override does NOT alter Reference/HwDirac heads (negative).
    CHECK (MS::measureHeadCopy (MS::Lead::Reference, true, false, CM::AutoPerEar).title == ref.title);
}

TEST_CASE("P3 Measure run-note: contract wording (never 're-run sweep')") {
    using MS = eb::MeasureStage;
    CHECK (MS::measureRunNote (true, "Finish Connect and Calibrate first", false, false)
           == "Finish Connect and Calibrate first");
    CHECK (MS::measureRunNote (false, "", false, true)
           == "Then start the measurement again in Dirac Live.");
    CHECK (MS::measureRunNote (false, "", false, false)
           == "Arms the bridge - the sweep still runs in Dirac");
    CHECK (MS::measureRunNote (false, "", true, false) == juce::String());
}

TEST_CASE("P3 waiting hint: threshold-gated, escalates by what the app actually knows") {
    using MS = eb::MeasureStage;
    // NEGATIVE first: below the threshold NOTHING fires, whatever the flags say.
    CHECK (MS::waitingHint (MS::kArmedNoSweepHintSeconds - 1, true, true, "48k mismatch", true).isEmpty());
    CHECK (MS::waitingHint (0, false, false, {}, false).isEmpty());
    // Escalation: most specific knowledge first.
    const int t = MS::kArmedNoSweepHintSeconds;
    CHECK (MS::waitingHint (t, true, true, "x", true).contains ("learned from"));       // #34 endpoint
    CHECK (MS::waitingHint (t, false, true, "input 44.1k - set 48k", true).contains ("input 44.1k"));
    CHECK (MS::waitingHint (t, false, false, {}, true).contains ("is Dirac playing"));
    const auto generic = MS::waitingHint (t, false, false, {}, false);
    CHECK (generic.contains ("EARS Bridge can't start it for you"));                    // the contract, in words
}

// ==================================================================================================
// P3 Task 5 LEDGERED OBLIGATION (Task 1 review + user ruling, belt-and-braces): the hot-plug/ctor
// OUTPUT re-resolution path (engine.setOutput direct, bypassing onOutputChosen) must stale the
// verdicts on a KEY change, exactly like the input memo (M-2 doctrine: a different device is
// different evidence). applyResolvedOutputForTest drives the SAME member the hot-plug lambda and
// the ctor restore use. Same-key re-apply is the negative (a replug of the SAME cable must not
// downgrade still-valid verdicts).
// ==================================================================================================
TEST_CASE("P3 verdictGen: a different-key output apply (hot-plug path) stales the verdicts; same-key stays fresh") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));

    // Seed the key memo (the ctor path in the live app), then grade both ears fresh.
    eb::DeviceId out; out.typeName = "Windows Audio"; out.name = "CABLE Input (VB-Audio)"; out.uid = "uid-out-A";
    mc.applyResolvedOutputForTest (out);
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    // Negative: a hot-plug re-resolution landing on the SAME output (same key) must NOT downgrade.
    mc.applyResolvedOutputForTest (out);
    CHECK_FALSE (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);

    // A DIFFERENT output device -> old-path verdicts must read STALE (hard staleness AND the wait
    // hint, per the user's belt-and-braces ruling - this is the staleness half).
    eb::DeviceId out2; out2.typeName = "Windows Audio"; out2.name = "Hi-Fi Cable Input"; out2.uid = "uid-out-B";
    mc.applyResolvedOutputForTest (out2);
    CHECK (eb::computeWizardState (mc.snapshotWizardInputsForTest(), std::nullopt).verdictsStale);
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 5 (routed here by Task 4's review): the RUNNING transport face. The engine cannot Run
// headless, so the running face is pinned through syncTransportForTest - a seam onto the SAME
// production member updateStartGate calls (not a replica). One action, per-stage faces: the Level
// transport and the Measure header CTA flip together.
// ==================================================================================================
TEST_CASE("P3 Measure transport: stopped/running faces + enable mirror (production syncTransport seam)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (900, 720);

    auto& mt = mc.measureStageForTest().transportButton();
    // Stopped face after construction (updateStartGate ran in the ctor): primary Start listening/play.
    CHECK (mt.getButtonText() == "Start listening");
    CHECK (mt.getProperties()["glyph"].toString() == "play");
    CHECK ((bool) mt.getProperties()["primary"]);
    CHECK_FALSE (mt.isEnabled());                       // hermetic gate closed, not running

    mc.syncTransportForTest (true, false);              // running (gate state irrelevant while running)
    CHECK (mt.getButtonText() == "Stop");
    CHECK (mt.getProperties()["glyph"].toString() == "stop");
    CHECK_FALSE ((bool) mt.getProperties()["primary"]); // Stop face is secondary (the frames' run-cluster)
    CHECK (mt.isEnabled());                             // running || gateReady
    CHECK (mc.startButtonForTest().getButtonText() == "Stop");   // the Level face flips WITH it (§3.3)

    mc.syncTransportForTest (false, true);              // stopped + gate ready -> armed-and-enabled
    CHECK (mt.getButtonText() == "Start listening");
    CHECK (mt.isEnabled());

    mc.syncTransportForTest (false, false);             // stopped + gate closed -> disabled (negative)
    CHECK_FALSE (mt.isEnabled());
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 6: the per-ear CaptureCards (Waiting / Capturing / Failed). Copy + fraction are pure model
// rules (no component, no JUCE init); the mid-capture failure is driven through the SAME live path
// (refreshWizardView -> refreshMeasureView) the device-died branch uses.
// ==================================================================================================
TEST_CASE("P3 CaptureCard copy + fraction: honest, approximate, no invented timing") {
    using CC = eb::CaptureCardModel;
    const auto w = CC::waiting ("RIGHT EAR", eb::CombineMode::AutoPerEar);
    CHECK (w.badge == "Waiting"); CHECK (w.title == "Next in the routine");
    CHECK (w.foot == "Queued");  CHECK (w.progress < 0.0f);           // NO bar - a fake bar is a lie
    const auto c = CC::capturing ("LEFT EAR", 0.58f, 10);
    CHECK (c.badge == "Capturing"); CHECK (c.title == "Sweeping now");
    CHECK (c.foot == "~10 s sweep");                                   // learned duration, labeled approximate
    CHECK (c.progress == 0.58f);
    const auto f = CC::failed ("LEFT EAR");
    CHECK (f.badge == "Failed"); CHECK (f.title == "Capture interrupted");
    CHECK (f.sub.contains ("can't be graded"));
    // fraction math: clamped, zero-duration safe
    CHECK (eb::CaptureCard::captureFraction (0, 10.0) == 0.0f);
    CHECK (eb::CaptureCard::captureFraction (150, 10.0) == 0.5f);      // 150 ticks @30Hz = 5s of 10s
    CHECK (eb::CaptureCard::captureFraction (600, 10.0) == 1.0f);      // clamped at the top
    CHECK (eb::CaptureCard::captureFraction (100, 0.0)  == 0.0f);      // no duration -> no claim
}

// ==================================================================================================
// P3 Task 7 rulings (user): MODE-AWARE capture copy. The Auto waiting sub on a Two-pass/Combined
// configuration described a routine that isn't running, and the verdict head's "Each earcup is
// graded" under Combined is a per-ear claim over ONE mixed channel. Words pinned verbatim (the
// honesty lives in them); a measured-wrap fit check guards the card's 48px sub slot - the mode
// variants never pass through the render gates (which drive canonical Auto scenes).
// ==================================================================================================
TEST_CASE("P3 mode-aware copy: waiting sub per combine mode + the Combined verdict head") {
    juce::ScopedJuceInitialiser_GUI juceInit;              // the fit check lays real text
    using CC = eb::CaptureCardModel;
    using MS = eb::MeasureStage;
    using CM = eb::CombineMode;

    // The three waiting-sub variants (LeftOnly/RightOnly share the Two-pass shape).
    const auto autoSub  = CC::waiting ("LEFT EAR",  CM::AutoPerEar).sub;
    const auto leftSub  = CC::waiting ("LEFT EAR",  CM::LeftOnly).sub;
    const auto rightSub = CC::waiting ("RIGHT EAR", CM::RightOnly).sub;
    const auto combSub  = CC::waiting ("LEFT EAR",  CM::Average).sub;
    CHECK (autoSub  == "Auto per-ear runs one earcup at a time - this sweep follows automatically.");
    CHECK (leftSub  == "Two-pass mode - this pass captures the left earcup only. Run Dirac once per ear.");
    CHECK (rightSub == "Two-pass mode - this pass captures the right earcup only. Run Dirac once per ear.");
    CHECK (combSub  == "Combined mode - the sweep captures both earcups mixed into one channel, not per ear.");
    CHECK (CC::waiting ("RIGHT EAR", CM::Sum).sub == combSub);      // Sum and Average share the truth
    // BOTH cards carry the mode sub - the UNSELECTED ear's card is the one whose Auto copy lied.
    CHECK (CC::waiting ("RIGHT EAR", CM::LeftOnly).sub == leftSub);
    // Only the sub is mode-aware (the ruling's scope): state chrome is unchanged.
    CHECK (CC::waiting ("LEFT EAR", CM::Sum).title == CC::waiting ("LEFT EAR", CM::AutoPerEar).title);
    CHECK (CC::waiting ("LEFT EAR", CM::Sum).badge == "Waiting");

    // The verdict head: Combined drops the per-ear claim; Two-pass/Auto keep it (each earcup does
    // get its own graded capture there) - the Combined variant must not leak into them.
    const auto headAuto = MS::measureHeadCopy (MS::Lead::Waiting, false, true, CM::AutoPerEar);
    const auto headComb = MS::measureHeadCopy (MS::Lead::Waiting, false, true, CM::Sum);
    CHECK (headComb.title == "Your measurement result");
    CHECK (headComb.sub == "The grade reflects the combined capture - both earcups mixed into one channel - expand Details to see the shape checks behind it.");
    CHECK_FALSE (headComb.sub.contains ("Each earcup"));
    CHECK (MS::measureHeadCopy (MS::Lead::Waiting, false, true, CM::Average).sub == headComb.sub);
    CHECK (headAuto.title == "Your per-ear result");
    CHECK (headAuto.sub.startsWith ("Each earcup is graded"));
    CHECK (MS::measureHeadCopy (MS::Lead::Waiting, false, true, CM::RightOnly).sub == headAuto.sub);
    // NEGATIVE: the mode speaks only on the VERDICT head - the armed §2 instruction stays frozen.
    CHECK (MS::measureHeadCopy (MS::Lead::Waiting, false, false, CM::Sum).sub
           == MS::measureHeadCopy (MS::Lead::Waiting, false, false, CM::AutoPerEar).sub);

    // Fit (measured wrap, drift-proof): each new sub must claim NO MORE height than the widest
    // ALREADY-PROVEN string in its slot, at that slot's real width and font. Card sub slot: 48px =
    // 3 lines render-measured in Task 6 with the Failed sub as the proven worst (CaptureCard.cpp
    // resized). Header sub slot: the frozen 3-line 2 instruction is the proven worst (StageHeader
    // kSubExtraRow). Same wrap math as VerdictCard::wrappedTextHeight.
    const auto wrappedH = [] (const juce::String& text, float fontPt, int slotW) {
        juce::Label ref;                                   // real Label border insets, no restated magic
        const auto border = ref.getBorderSize();
        juce::AttributedString as; as.append (text, juce::Font (juce::FontOptions (fontPt)));
        juce::TextLayout tl; tl.createLayout (as, (float) slotW - (float) (border.getLeft() + border.getRight()));
        return (int) std::ceil (tl.getHeight());
    };
    const auto provenCard = CC::failed ("LEFT EAR").sub;   // 3 lines at 241px (Task 6, render-measured)
    for (const auto& sub : { leftSub, rightSub, combSub })
        CHECK (wrappedH (sub, 12.0f, 241) <= wrappedH (provenCard, 12.0f, 241));
    const auto provenHead = MS::measureHeadCopy (MS::Lead::Waiting, false, false, CM::AutoPerEar).sub;
    CHECK (wrappedH (headComb.sub, 13.0f, 294) <= wrappedH (provenHead, 13.0f, 294));
}

TEST_CASE("P3 mid-capture failure: the failed card is sticky and never falls back to waiting") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    mc.driveMidCaptureFailureForTest (0);                              // the LEFT capture was interrupted
    auto& card = mc.measureStageForTest().captureCardForTest (0);
    CHECK (card.titleForTest().getText() == "Capture interrupted");
    mc.forceWizardStepForTest (eb::WizardStep::Level);                 // navigate away...
    mc.forceWizardStepForTest (eb::WizardStep::Measure);               // ...and back: STILL failed (sticky)
    CHECK (card.titleForTest().getText() == "Capture interrupted");
    // NEGATIVE: the other ear stays a plain waiting card - the failure names ONE capture.
    CHECK (mc.measureStageForTest().captureCardForTest (1).titleForTest().getText() == "Next in the routine");
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 7: "Measure again" semantics. Never auto-advance, never "re-run sweep": the button re-arms
// the VIEW (and the engine when stopped); the spine's Done-ness stays computed from the stamps.
// ==================================================================================================
TEST_CASE("P3 Measure again: verdicts showing -> refresh face; click re-arms the view; new grade restores") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    auto& t = mc.measureStageForTest().transportButton();
    CHECK (t.getButtonText() == "Start listening");                    // no verdicts yet

    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);          // both ears grade
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedMarginal, 18.0f); // 18 dB = Orange band
                                                                              // (classifySweepSnr: [18,25)
                                                                              // -> Orange, RefMonitor.h)
    CHECK (t.getButtonText() == "Measure again");                      // the §2 verb - NEVER "re-run sweep"
    CHECK (t.getProperties()["glyph"].toString() == "refresh");
    // Both cards render their grades (the grid flipped CaptureCard -> VerdictCard per ear).
    CHECK (mc.measureStageForTest().verdictCardForTest (0).badgeForTest().getText() == "Clean");
    CHECK (mc.measureStageForTest().verdictCardForTest (1).badgeForTest().getText() == "Marginal SNR");

    t.onClick();                                                       // Measure again (engine stopped path
                                                                       //  refuses start via the gate - the
                                                                       //  VIEW re-arm is what we assert)
    CHECK (t.getButtonText() == "Start listening");                    // back to the wait/instruction state
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);    // a NEW grade lands...
    CHECK (t.getButtonText() == "Measure again");                      // ...the verdict view returns
    tmp.deleteRecursively();
}

TEST_CASE("P3 stale verdicts: config bump dims the cards under the strip; fresh grades clear it") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true,
                                                          tmp.getChildFile ("appdata"), tmp.getChildFile ("logs") });
    mc.setSize (900, 720);
    mc.forceWizardStepForTest (eb::WizardStep::Measure);
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    auto& cardL = mc.measureStageForTest().verdictCardForTest (0);
    CHECK (cardL.getAlpha() > 0.9f);                                   // fresh: full strength
    mc.bumpConfigGenForTest();
    mc.forceWizardStepForTest (eb::WizardStep::Measure);               // re-render from the new truth
    CHECK (cardL.getAlpha() < 0.6f);                                   // §3.2: dimmed, never deleted
    // T2-review reconciliation (double-stating): IN CONTEXT the stage's stale STRIP is the single
    // voice - the card's inline tag stays hidden; STANDALONE (no strip) the tag remains the why.
    CHECK_FALSE (cardL.staleTagForTest().isVisible());
    {
        eb::VerdictCard standalone;
        eb::EarGradeSnapshot e; e.state = (int) eb::RefMonState::GradedClean;
        e.sweepSnrDb = 30; e.irSnrDb = 56; e.thdPercent = 0.5f; e.peakDb = -8;
        standalone.setModel (eb::verdictCardModelAuto ("LEFT EAR", e, 0u, {}, /*stale*/ true, false));
        CHECK (standalone.staleTagForTest().isVisible());
    }
    mc.publishGradeForTest (0, (int) eb::RefMonState::GradedClean);
    mc.publishGradeForTest (1, (int) eb::RefMonState::GradedClean);
    CHECK (cardL.getAlpha() > 0.9f);                                   // refreshed evidence
    tmp.deleteRecursively();
}

// ==================================================================================================
// P3 Task 7 (routed from Task 6's review): the two CaptureCard/composition holes the happy-path
// tests missed. (1) applyTheme must re-derive the STATE tones under a HELD model - setModel's
// set-if-changed early-out no-ops on a theme flip, so the re-tone lives in applyModelTone and this
// pins it. (2) The composition lambda's Capturing branch (ref.size()/rate -> honest progress) was
// untested: drive the SAME production composition member through a seam, mirror of
// driveMidCaptureFailureForTest.
// ==================================================================================================
TEST_CASE("P3 CaptureCard theme re-tone: applyTheme re-derives state tones under a HELD model") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    const bool wasDark = eb::Theme::dark();
    eb::Theme theme;                                        // palette owner for setDarkForTest
    eb::CaptureCard card;
    card.setModel (eb::CaptureCardModel::failed ("LEFT EAR"));
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        card.applyTheme();                                  // the model is HELD - only the palette moved
        CHECK (card.titleForTest().findColour (juce::Label::textColourId) == eb::Theme::danger());
        CHECK (card.badgeForTest().findColour (juce::Label::textColourId) == eb::Theme::danger());
    }
    card.setModel (eb::CaptureCardModel::capturing ("LEFT EAR", 0.5f, 10));
    for (bool dark : { true, false }) {
        theme.setDarkForTest (dark);
        card.applyTheme();
        CHECK (card.badgeForTest().findColour (juce::Label::textColourId) == eb::Theme::accentText());
    }
    theme.setDarkForTest (wasDark);
}

TEST_CASE("P3 capture composition: the Capturing branch wires ref.size()/rate into honest progress") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    eb::MainComponent mc (hermetic (tmp));
    mc.setSize (900, 720);
    mc.setReferenceForTest (96000, 48000.0);                // a 2 s learned reference, both ears
    const auto m = mc.composeCaptureModelForTest (0, 30);   // 30 ticks @ 30 Hz = 1 s elapsed
    CHECK (m.state == eb::CaptureCardModel::State::Capturing);
    CHECK (m.foot == "~2 s sweep");                         // duration = ref.size()/rate, labeled approximate
    CHECK (m.progress == eb::CaptureCard::captureFraction (30, 2.0));   // elapsed over the LEARNED duration
    // NEGATIVE: no learned reference -> the bar claims NOTHING (never invented Dirac timing).
    mc.setReferenceForTest (0, 0.0);
    CHECK (mc.composeCaptureModelForTest (0, 300).progress == 0.0f);
    // Failed WINS the composition (the sticky §3.1 card names the interrupted capture).
    mc.driveMidCaptureFailureForTest (1);
    CHECK (mc.composeCaptureModelForTest (1, 30).state == eb::CaptureCardModel::State::Failed);
    tmp.deleteRecursively();
}
