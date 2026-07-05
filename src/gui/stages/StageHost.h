#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include "gui/WizardState.h"
#include "gui/Theme.h"
#include "gui/Glyphs.h"

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

        // Regression banner (§3.1): a one-line attention label + a "Fix in <step>" jump, spanning the top
        // of the stage area. Hidden until setBanner() is fed a nonempty line; when visible, the stages shift
        // down kBannerH so the banner never overlaps stage content.
        bannerLabel_.setJustificationType (juce::Justification::centredLeft);
        bannerLabel_.setMinimumHorizontalScale (1.0f);
        addChildComponent (bannerLabel_);
        bannerFix_.onClick = [this] { if (onBannerFix_) onBannerFix_(); };
        addChildComponent (bannerFix_);
    }

    // Feed the regression banner. Empty text hides it (stages reclaim the full height). `fixStepName` is the
    // jump-destination label ("Connect"); onFix fires the user-invoked jump (never auto-navigation, §3.1).
    void setBanner (const juce::String& text, const juce::String& fixStepName, std::function<void()> onFix) {
        // setBanner runs at 30 Hz (from refreshWizardView). minor-6: only re-assign the onFix std::function
        // when the jump TARGET actually changes — reallocating a std::function 30x/s is pure churn.
        if (fixStepName != fixStepName_) {
            onBannerFix_ = std::move (onFix);
            fixStepName_ = fixStepName;
        }
        const bool show = text.isNotEmpty();
        bool changed = false;
        if (text != bannerLabel_.getText()) { bannerLabel_.setText (text, juce::dontSendNotification); changed = true; }
        const juce::String btn = fixStepName.isNotEmpty() ? ("Fix in " + fixStepName) : juce::String();
        if (btn != bannerFix_.getButtonText()) { bannerFix_.setButtonText (btn); changed = true; }
        if (show != bannerLabel_.isVisible()) {
            bannerLabel_.setVisible (show);
            bannerFix_.setVisible (show && btn.isNotEmpty());
            changed = true;
        }
        // minor-6: re-apply the banner colour unconditionally (outside the `changed` guard) so a live
        // light/dark theme flip re-tints it — a flip changes Theme::warn() but not the text, so the old
        // guard left a stale colour. This is a cheap idempotent setColour (Label only repaints on a delta).
        bannerLabel_.setColour (juce::Label::textColourId, Theme::warn());
        if (changed)
            resized();   // re-flow: the stages shift down/up as the banner appears/disappears
    }
    bool bannerVisibleForTest() const { return bannerLabel_.isVisible(); }

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
        // Focus discipline: refreshWizardView() runs on EVERY updateStartGate, so showStage is called on
        // unrelated events (e.g. an override-toggle click). If the requested stage is already the shown,
        // visible one this is a no-op re-render — grabbing focus here would yank it back to the stage's
        // first control (stealing it from whatever the user just clicked). Only act on a REAL switch.
        if (step == shown_)
            if (auto* cur = stages_[(size_t) step]; cur != nullptr && cur->isVisible())
                return;

        shown_ = step;
        for (int i = 0; i < kWizardStepCount; ++i)
            if (auto* st = stages_[(size_t) i])
                st->setVisible (i == (int) step);
        // Explicit focus placement on the newly-shown stage's first focusable control (§4: a hidden
        // component gives focus away, so we place it deterministically rather than let it wander).
        // Guard on isShowing(): JUCE asserts (jassert isShowing(), juce_Component.cpp:2694) if you grab
        // focus on a component not yet on screen (construction/headless). Release compiles the jassert
        // out; debug builds abort on launch. A not-yet-showing stage takes focus on its next paint anyway.
        if (auto* st = stages_[(size_t) step]; st != nullptr && st->isShowing())
            st->grabKeyboardFocus();
    }

    WizardStep shown() const { return shown_; }

    void resized() override {
        auto area = getLocalBounds();
        if (bannerLabel_.isVisible()) {
            auto strip = area.removeFromTop (kBannerH).reduced (16, 4);
            bannerIconArea_ = strip.removeFromLeft (15).toFloat();   // warning-triangle gutter (P2.9)
            strip.removeFromLeft (7);
            if (bannerFix_.isVisible())
                bannerFix_.setBounds (strip.removeFromRight (110));
            bannerLabel_.setBounds (strip);
        }
        for (auto* st : stages_)
            if (st != nullptr)
                st->setBounds (area);
    }

    void paint (juce::Graphics& g) override {
        if (bannerLabel_.isVisible() && ! bannerIconArea_.isEmpty())
            eb::glyph::drawWarning (g, bannerIconArea_.withSizeKeepingCentre (15.0f, 15.0f), Theme::warn());
    }

    static constexpr int kBannerH = 28;   // the regression-banner strip height (§ Task 4: stages shift 28px)

private:
    std::array<juce::Component*, (size_t) kWizardStepCount> stages_ { { nullptr, nullptr, nullptr, nullptr } };
    WizardStep shown_ = WizardStep::Connect;
    juce::Label      bannerLabel_;
    juce::TextButton bannerFix_;
    std::function<void()> onBannerFix_;
    juce::String     fixStepName_;   // the current jump target's name; onBannerFix_ is re-bound only on a change
    juce::Rectangle<float> bannerIconArea_;   // P2.9: warning-triangle gutter reserved by resized()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageHost)
};

} // namespace eb
