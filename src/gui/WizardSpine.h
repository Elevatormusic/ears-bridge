#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/WizardState.h"

// WizardSpine — the persistent, navigable step rail (P1 Task 2 of the guided-wizard redesign).
// Spec: docs/superpowers/specs/2026-07-04-wizard-redesign-design.md §4 / §5.5. Visual reference:
// docs/gui-redesign/redesign-final.html (the .spine block). Colours come ONLY from eb::Theme tokens
// (the HIG audit standard: zero raw colour literals outside Theme).
//
// It is a pure VIEW of a WizardState snapshot: setState() paints the four step nodes/titles/metas,
// the active highlight, the connectors, and the Reference footer, and refreshes each row's a11y
// title. Navigation is a user act carried out of the component via onStepClicked — the spine never
// decides to move the user. No animation whatsoever in Phase 1 (Reduce-Motion safe by construction;
// the active-node pulse arrives with the Phase 3 scenes).

namespace eb {

class WizardSpine : public juce::Component {
public:
    WizardSpine();
    ~WizardSpine() override;

    // Fires ONLY for navigable steps (Done/Error/Todo/Active). Blocked rows are inert.
    std::function<void (WizardStep)> onStepClicked;

    // Render a state snapshot: updates node states/metas/active highlight + a11y titles.
    // viewMetas = per-step summaries the view owns (device names, serials). A non-empty entry
    // OVERRIDES the machine reason for that step's meta line; an empty entry keeps the machine
    // reason. refLine1/refLine2 feed the Reference footer (Phase 1: e.g. "matched" / "Windows Audio").
    void setState (const WizardState& ws, const juce::String viewMetas[kWizardStepCount],
                   const juce::String& refLine1, const juce::String& refLine2);

    int  preferredWidth() const { return 248; }

    void resized() override;
    void paint (juce::Graphics&) override;

    // ---- test-only seams (documented; used by tests/test_wizardspine.cpp) --------------------
    // These expose the private row state so tests can assert the render contract without a peer.
    int          rowCountForTest() const;
    bool         rowEnabledForTest (int step) const;
    juce::String rowTitleForTest (int step) const;
    juce::String rowMetaForTest (int step) const;
    // The Reference footer value's current text colour — tests pin the content-driven tone
    // (ok() for a positive status, textDim() for "not learned"/"n/a").
    juce::Colour refValueColourForTest() const { return refValue.findColour (juce::Label::textColourId); }
    // P2.9: the active row's "You are here" tag colour (pins the accent-anchor token).
    juce::Colour tagColourForTest() const;
    // Drive a row's click path programmatically (mirrors a Return/Space/mouse activation). A Blocked
    // row is inert, so this is a no-op there — exactly what a real click would do.
    void         clickStepForTest (int step);
    // Repaint discipline (M-1): the count of repaints a row has issued. An identical second setState
    // must leave this unchanged (the always-visible spine must not re-invalidate at 30 Hz).
    int          rowRepaintCountForTest (int step) const;

private:
    struct StepRow;                 // node circle + title + meta labels; clickable; focusable
    juce::OwnedArray<StepRow> rows;

    // Reference footer (a hairline + two small label rows), fed by setState.
    juce::Label refLabel, refValue;     // row 1: "Reference" -> value
    juce::Label refLabel2, refValue2;   // row 2: "Learned from" -> value

    void rowActivated (int step);       // the single click seam every row funnels through

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WizardSpine)
};

} // namespace eb
