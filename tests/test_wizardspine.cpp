#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/WizardSpine.h"
#include "gui/juce_design_probe.h"

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
