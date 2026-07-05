#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include "gui/CalSlotComponent.h"
#include "gui/DisclosureRow.h"
#include "gui/stages/StageHeader.h"
#include "cal/CalFile.h"

namespace eb {

// CalibrateStage - P2 redesign (spec 5.2 + the firstrun frame): a FIXED StageHeader (eyebrow /
// title / lead / CTA cluster) over a scrolled body: the two cal slots as a side-by-side drop
// grid, ONE info-toned stage caption (the HEQ/IDF guidance - it used to render once per card),
// the unity path (Task 6), and the collapsible Advanced-FIR disclosure. Every failure surface
// keeps a home - see the plan's Failure-surface map: swap/serial/type rejects and RAW/Unknown/
// parse cautions stay per-card; the FIR-building state rides the run-note + spine meta.
class CalibrateStage : public juce::Component {
public:
    CalibrateStage();

    // Reparent the Calibrate controls in (construction only). P2: no calEyebrow any more -
    // the stage header replaced it.
    void adopt (CalSlotComponent& leftCal, CalSlotComponent& rightCal,
                juce::ToggleButton& complexPhaseToggle,
                juce::Label& firLenLabel, juce::ComboBox& firLenBox,
                juce::Label& trimLabel, juce::Slider& trimSlider);

    juce::TextButton& continueButton() { return header_.continueButton(); }
    std::function<void()> onContinue;

    // ---- P2 view feed (MainComponent renderWizardView; all set-if-changed) ----
    void setRunNote (const juce::String& s) { header_.setRunNote (s); }
    void setStageCaption (const juce::String& s);
    void setAdvancedSummary (const juce::String& s) { advancedFir_.setSummary (s); }
    void setAdvancedOpen (bool open);              // also the gate/harness seam
    bool advancedOpenForTest() const { return advancedFir_.isOpen(); }
    // T10: the Advanced-FIR disclosure row itself, for the displacement-honesty gate to assert
    // the collapse affordance stays fully inside the viewport with the section disclosed.
    DisclosureRow& advancedFirForTest() { return advancedFir_; }

    // Unity path (spec 5.2): visible only while BOTH slots are empty; `accepted` swaps the
    // wording and retires the button (the header CTA takes over). MainComponent owns the flag.
    void setUnityState (bool bothSlotsEmpty, bool accepted);
    std::function<void()> onContinueWithoutCal;
    juce::TextButton& unityButtonForTest() { return unityBtn_; }

    // T10: the gated no-scroll workflow states (see ConnectStage's twin note - keep beside
    // layoutContent; the gate static_asserts the count).
    enum class WorkflowState { EmptyUnity, LoadedCleanPair, LoadedWorstPair,
                               EmptyUnityAdvancedOpen, LoadedWorstAdvancedOpen,
                               EmptyErrorStripAdvancedOpen, Count };
    // Sentinel-derived: an enumerator added before Count bumps this automatically, so the gate's
    // static_asserts fail-close on an enum edit ALONE (no second hand-maintained number to forget).
    static constexpr int kWorkflowStateCount = (int) WorkflowState::Count;
    juce::Viewport& viewportForTest() { return viewport_; }

    // Pure copy rules (headlessly tested in test_calibratestage.cpp):
    static juce::String stageCaptionFor (std::optional<CalType> left, std::optional<CalType> right);
    static juce::String advancedFirSummary (bool complexPhase, int firLength /*0=auto*/, double trimDb);

    void applyTheme();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Content : juce::Component { void resized() override {} };   // laid out by the parent
    // The info-toned caption box (frame .help): rounded infoBg + a painted (i) + a wrapping label.
    struct InfoCaption : juce::Component {
        InfoCaption();
        juce::Label text;
        void applyTheme();
        void resized() override;
        void paint (juce::Graphics&) override;
    };

    StageHeader    header_ { "STEP 2 OF 4", "Load your ear calibrations",
                             "Drop each earcup's file - the per-ear filter that corrects the measurement.",
                             "Continue to level" };
    juce::Viewport viewport_;
    Content        content_;
    InfoCaption    caption_;
    juce::Label    unityHint_;
    juce::TextButton unityBtn_ { "Continue without calibration" };
    DisclosureRow  advancedFir_ { "Advanced FIR" };

    CalSlotComponent*   leftCal_ = nullptr;
    CalSlotComponent*   rightCal_ = nullptr;
    juce::ToggleButton* complexPhaseToggle_ = nullptr;
    juce::Label*        firLenLabel_ = nullptr;
    juce::ComboBox*     firLenBox_ = nullptr;
    juce::Label*        trimLabel_ = nullptr;
    juce::Slider*       trimSlider_ = nullptr;

    int layoutContent (int width);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalibrateStage)
};

} // namespace eb
