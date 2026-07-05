#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "gui/DevicePicker.h"
#include "gui/DisclosureRow.h"
#include "gui/stages/StageHeader.h"

namespace eb {

// ConnectStage - P2 redesign (spec 5.1): StageHeader over a scrolled 560px column of three
// grouped surface cards - SIGNAL PATH (input -> output + hints/preflight/fix), FORMAT
// (combine, rate + bit), WIRING CHECK (the L/R verify) - and the "Not using Dirac?" disclosure
// hosting the override toggle. Cards are painted fills (design-notes: elevation step, no
// outline); the existing control eyebrows are the group labels. While the override is ON the
// disclosure is LOCKED open (an active escape hatch is never hideable).
class ConnectStage : public juce::Component {
public:
    ConnectStage();

    // Reparent the Connect controls in (construction only). References stay owned by MainComponent.
    void adopt (DevicePicker& inputPicker, juce::Label& inputGainHint,
                juce::Label& combineLabel, juce::ComboBox& combineBox, juce::Label& combineHint,
                DevicePicker& outputPicker, juce::Label& outputHint,
                juce::Label& preflightLabel, juce::Label& preflightInfo,
                juce::Label& diracCableHint, juce::TextButton& diracFixButton,
                juce::Label& rateLabel, juce::ComboBox& rateBox, juce::Label& rateWarn,
                juce::Label& bitLabel, juce::ComboBox& bitBox,
                juce::TextButton& verifyButton, juce::Label& verifyResultLabel,
                juce::ToggleButton& overrideToggle);

    juce::TextButton& continueButton() { return header_.continueButton(); }   // forwards to the header CTA
    std::function<void()> onContinue;

    // ---- P2 view feed (MainComponent renderWizardView; set-if-changed) ----
    void setRunNote (const juce::String& s) { header_.setRunNote (s); }
    void syncOverrideDisclosure();                     // lock-open + summary while the override is ON
    DisclosureRow& notUsingDiracForTest() { return notUsingDirac_; }
    // Task-7 review seam: the group-card rects the paint reads (content-local). A test asserts the
    // geometry stays FRESH after a relayout grows a card on a tall window (the stripe itself is
    // paint-path, only verifiable by render). Returns a copy - do not mutate the live layout state.
    std::vector<juce::Rectangle<int>> groupRectsForTest() const { return groupRects_; }
    // Task-7 review seam: the adopted dirac-cable-hint label (owned by MainComponent) that the SIGNAL
    // PATH card grows to host. A test forces it visible + relayouts to prove the card rect grows.
    juce::Label* diracCableHintForTest() { return diracCableHint_; }

    // T10: the gated no-scroll workflow states. KEEP THIS BESIDE layoutContent's conditional
    // branches - any new conditional row/state added there must appear here; the gate
    // static_asserts the count (fail-closed).
    enum class WorkflowState { Default, DiracHintFix, RateWarn, DiracHintFixRateWarn,
                               OverrideOpen, OverrideOpenWorst, Count };
    // Sentinel-derived: an enumerator added before Count bumps this automatically, so the gate's
    // static_asserts fail-close on an enum edit ALONE (no second hand-maintained number to forget).
    static constexpr int kWorkflowStateCount = (int) WorkflowState::Count;
    juce::Viewport& viewportForTest() { return viewport_; }

    // Re-apply theme-dependent colours to the stage's OWN header labels on a live light/dark flip
    // (MainComponent calls this from applyTextColours; adopted controls re-colour through their own applyTheme()).
    void applyTheme();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    // The scrolled content. Paints the group-card fills BEHIND the adopted children.
    struct Content : juce::Component {
        ConnectStage* owner = nullptr;
        void paint (juce::Graphics& g) override;
        void resized() override {}   // laid out explicitly by the parent
    };

    StageHeader    header_ { "STEP 1 OF 4", "Connect your devices",
                             "Pick the EARS input and the virtual cable output that feeds Dirac Live.",
                             "Continue to calibration" };
    juce::Viewport viewport_;
    Content        content_;
    DisclosureRow  notUsingDirac_ { "Not using Dirac?" };
    std::vector<juce::Rectangle<int>> groupRects_;     // the three card fills (in content-local coords)

    // Adopted (owned by MainComponent) - held as pointers for layout only.
    DevicePicker*    inputPicker_ = nullptr;
    juce::Label*     inputGainHint_ = nullptr;
    juce::Label*     combineLabel_ = nullptr;
    juce::ComboBox*  combineBox_ = nullptr;
    juce::Label*     combineHint_ = nullptr;
    DevicePicker*    outputPicker_ = nullptr;
    juce::Label*     outputHint_ = nullptr;
    juce::Label*     preflightLabel_ = nullptr;
    juce::Label*     preflightInfo_ = nullptr;
    juce::Label*     diracCableHint_ = nullptr;
    juce::TextButton* diracFixButton_ = nullptr;
    juce::Label*     rateLabel_ = nullptr;
    juce::ComboBox*  rateBox_ = nullptr;
    juce::Label*     rateWarn_ = nullptr;
    juce::Label*     bitLabel_ = nullptr;
    juce::ComboBox*  bitBox_ = nullptr;
    juce::TextButton* verifyButton_ = nullptr;
    juce::Label*     verifyResultLabel_ = nullptr;
    juce::ToggleButton* overrideToggle_ = nullptr;

    int layoutContent (int width);   // top-down pass inside content_; returns the total height
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConnectStage)
};

} // namespace eb
