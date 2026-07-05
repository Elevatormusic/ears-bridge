#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/WizardSpine.h"
#include "gui/juce_design_probe.h"
#include "gui/Theme.h"   // Theme::ok()/textDim() — reference-value tone assertions

using eb::WizardSpine;
using eb::WizardState;
using eb::WizardStep;
using eb::StepState;

namespace {
// A representative snapshot the view would feed the spine: Connect Done (with a device-name
// viewMeta the view owns), Calibrate Active, Level Todo, Measure Blocked. Built directly so the
// test pins the RENDER contract independent of the state machine's resolution rules.
WizardState makeSnapshot() {
    WizardState ws;
    ws.steps[(int) WizardStep::Connect]   = { StepState::Done,   {} };
    ws.steps[(int) WizardStep::Calibrate] = { StepState::Active, juce::String ("Load both ear calibration files") };
    ws.steps[(int) WizardStep::Level]     = { StepState::Todo,   juce::String ("Start monitoring and set the level") };
    ws.steps[(int) WizardStep::Measure]   = { StepState::Blocked, juce::String ("Finish Connect and Calibrate first") };
    ws.active = WizardStep::Calibrate;
    return ws;
}

const char* kViewMetas[eb::kWizardStepCount] = {
    "miniDSP EARS -> VB-Audio Cable", "", "", ""
};
}

TEST_CASE("WizardSpine: four navigable rows exist as children") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);

    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");

    CHECK (spine.rowCountForTest() == eb::kWizardStepCount);
}

TEST_CASE("WizardSpine: the Blocked row is disabled and not clickable") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");

    CHECK_FALSE (spine.rowEnabledForTest ((int) WizardStep::Measure));   // Blocked
    CHECK       (spine.rowEnabledForTest ((int) WizardStep::Connect));   // Done -> navigable

    // Clicking the Blocked row fires NOTHING.
    int fired = -1;
    spine.onStepClicked = [&fired] (WizardStep s) { fired = (int) s; };
    spine.clickStepForTest ((int) WizardStep::Measure);
    CHECK (fired == -1);
}

TEST_CASE("WizardSpine: clicking a navigable row fires onStepClicked with its step") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");

    int fired = -1;
    spine.onStepClicked = [&fired] (WizardStep s) { fired = (int) s; };

    spine.clickStepForTest ((int) WizardStep::Connect);   // Done row
    CHECK (fired == (int) WizardStep::Connect);

    fired = -1;
    spine.clickStepForTest ((int) WizardStep::Level);     // Todo row
    CHECK (fired == (int) WizardStep::Level);
}

TEST_CASE("WizardSpine: row a11y titles carry step index and state word") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");

    const auto connectTitle = spine.rowTitleForTest ((int) WizardStep::Connect);
    CHECK (connectTitle.contains ("Step 1 of 4"));
    CHECK (connectTitle.contains ("Connect"));
    CHECK (connectTitle.contains ("done"));

    const auto measureTitle = spine.rowTitleForTest ((int) WizardStep::Measure);
    CHECK (measureTitle.contains ("Step 4 of 4"));
    CHECK (measureTitle.contains ("Measure"));
    CHECK (measureTitle.contains ("blocked"));

    const auto calTitle = spine.rowTitleForTest ((int) WizardStep::Calibrate);
    CHECK (calTitle.contains ("Step 2 of 4"));
}

TEST_CASE("WizardSpine: a DONE viewMeta overrides the machine reason") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");

    // Connect is Done with a viewMeta -> the meta line shows the device names, not a machine reason.
    CHECK (spine.rowMetaForTest ((int) WizardStep::Connect) == juce::String ("miniDSP EARS -> VB-Audio Cable"));
    // Calibrate (Active) has an empty viewMeta -> keeps its machine reason.
    CHECK (spine.rowMetaForTest ((int) WizardStep::Calibrate) == juce::String ("Load both ear calibration files"));
}

// The reference value is toned BY CONTENT: green (ok) only for a positive status; dimmed otherwise.
TEST_CASE("WizardSpine: reference value tone follows content (positive green, negative dimmed)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];

    // Positive: "matched" (and "learned") read as success -> ok() green.
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");
    CHECK (spine.refValueColourForTest() == eb::Theme::ok());
    spine.setState (makeSnapshot(), metas, "learned", "Windows Audio");
    CHECK (spine.refValueColourForTest() == eb::Theme::ok());

    // Negative: "not learned" must NOT be green (it is not a success) -> dimmed. "not " vetoes the
    // "learned" substring match.
    spine.setState (makeSnapshot(), metas, "not learned", "");
    CHECK (spine.refValueColourForTest() == eb::Theme::textDim());
    CHECK (spine.refValueColourForTest() != eb::Theme::ok());

    // Neutral: "n/a (hardware Dirac)" -> dimmed too.
    spine.setState (makeSnapshot(), metas, "n/a (hardware Dirac)", "");
    CHECK (spine.refValueColourForTest() == eb::Theme::textDim());
}

// M-1: the always-visible spine must NOT repaint on an identical re-render. refreshWizardView runs at
// 30 Hz, so an unconditional row->repaint() in setState would re-invalidate all four rows forever. A
// second setState with the SAME inputs must leave every row's repaint count unchanged; a DIFFERENT
// snapshot must repaint the rows whose paint-relevant state actually changed.
TEST_CASE("WizardSpine: an identical re-render does not repaint any row (M-1)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];

    // First render establishes the rows' state; a second identical render must be a no-op paint-wise.
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");
    int before[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) before[i] = spine.rowRepaintCountForTest (i);

    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");
    for (int i = 0; i < eb::kWizardStepCount; ++i)
        CHECK (spine.rowRepaintCountForTest (i) == before[i]);   // no churn on the identical second call

    // A genuine change (Calibrate Active -> Done) must repaint at least the rows that changed.
    WizardState changed = makeSnapshot();
    changed.steps[(int) WizardStep::Calibrate] = { StepState::Done, {} };
    changed.steps[(int) WizardStep::Level]     = { StepState::Active, juce::String ("Start monitoring and set the level") };
    changed.active = WizardStep::Level;
    spine.setState (changed, metas, "matched", "Windows Audio");
    CHECK (spine.rowRepaintCountForTest ((int) WizardStep::Calibrate) > before[(int) WizardStep::Calibrate]);
    CHECK (spine.rowRepaintCountForTest ((int) WizardStep::Level)     > before[(int) WizardStep::Level]);
}

// P2.9: the active row's "You are here" tag is the accent anchor. Pins the accentText() token on the
// active row's tag (was infoText() during the frozen-Theme era). Default WizardState -> Connect active,
// Todo -> the active pill + tag render, so tagColourForTest() reads the active tag's colour.
TEST_CASE("P2.9: the active-step tag is accentText (the accent anchor)") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    eb::Theme theme;
    WizardSpine spine;
    spine.setSize (spine.preferredWidth(), 560);
    WizardState ws;                                     // default: Connect active, all Todo
    juce::String metas[eb::kWizardStepCount];           // empty -> machine reasons (none set) keep as-is
    spine.setState (ws, metas, "not learned", "");
    // The tag label is private; assert through the tag-colour seam.
    CHECK (spine.tagColourForTest() == eb::Theme::accentText());
}

// Probe sanity: at the production width x 560 the given metas must not overflow any label.
TEST_CASE("WizardSpine: probe reports no text overflow at 248x560") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    WizardSpine spine;
    spine.setSize (248, 560);
    juce::String metas[eb::kWizardStepCount];
    for (int i = 0; i < eb::kWizardStepCount; ++i) metas[i] = kViewMetas[i];
    spine.setState (makeSnapshot(), metas, "matched", "Windows Audio");

    const auto json = hig::describeComponentTree (spine);
    const auto tree = juce::JSON::parse (json);
    const auto* elements = tree.getProperty ("elements", {}).getArray();
    REQUIRE (elements != nullptr);

    juce::StringArray overflowed;
    for (auto& e : *elements)
        if (e.getProperty ("textOverflows", false))
            overflowed.add (e.getProperty ("label", {}).toString());

    INFO ("overflowed labels: " << overflowed.joinIntoString (" | "));
    CHECK (overflowed.isEmpty());
}
