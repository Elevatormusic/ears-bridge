#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include "gui/WizardState.h"

// StageHost — the single-child stage switcher (P1 Task 3 of the guided-wizard redesign).
// Spec: docs/superpowers/specs/2026-07-04-wizard-redesign-design.md §4 (construct-once/reparent-once,
// setVisible-only switching). It OWNS NOTHING: the four stages are MainComponent members reparented in
// once at construction via setStages(); navigation is setVisible ONLY (JUCE 8.0.4-verified: hidden
// components drop out of focus traversal + the a11y tree automatically — no per-navigation reparenting).
// Every stage is laid out to the host's FULL bounds; only one is visible at a time.

namespace eb {

class StageHost : public juce::Component {
public:
    StageHost() {
        // A11y: the host is a transparent container; each stage is its own keyboardFocusContainer.
        setInterceptsMouseClicks (false, true);
    }

    // Reparent the four stages in ONCE (construction only). Order = the WizardStep enum order.
    void setStages (std::array<juce::Component*, (size_t) kWizardStepCount> s) {
        stages_ = s;
        for (auto* st : stages_)
            if (st != nullptr) {
                addChildComponent (*st);   // added hidden; showStage() reveals exactly one
                st->setBounds (getLocalBounds());
            }
    }

    // Navigation: reveal the requested stage, hide the rest, and place focus on the new stage.
    // setVisible switching ONLY — nothing is reparented, constructed, or destroyed here.
    void showStage (WizardStep step) {
        shown_ = step;
        for (int i = 0; i < kWizardStepCount; ++i)
            if (auto* st = stages_[(size_t) i])
                st->setVisible (i == (int) step);
        // Explicit focus placement on the newly-shown stage's first focusable control (§4: a hidden
        // component gives focus away, so we place it deterministically rather than let it wander).
        if (auto* st = stages_[(size_t) step])
            st->grabKeyboardFocus();
    }

    WizardStep shown() const { return shown_; }

    void resized() override {
        for (auto* st : stages_)
            if (st != nullptr)
                st->setBounds (getLocalBounds());
    }

private:
    std::array<juce::Component*, (size_t) kWizardStepCount> stages_ { { nullptr, nullptr, nullptr, nullptr } };
    WizardStep shown_ = WizardStep::Connect;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageHost)
};

} // namespace eb
