#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/LevelMeter.h"
#include "gui/stages/StageHeader.h"

namespace eb {

// LevelStage - P3 rebuild (spec 5.3 [P3-refresh 2026-07-05]): StageHeader over a fixed 560px centered
// column - the re-homed engine transport (ONE toggling button, "Start monitoring"/"Stop" - the §3.3
// per-stage label of the single engine action), the LEVELS hairline card (three W2 meter rows +
// target legend), the Dirac Master-output guidance line, the mic-gain hint, and the input-clip
// warning. No Viewport: every workflow state fits at 900x720 (the fit gate pins it fail-closed).
class LevelStage : public juce::Component {
public:
    LevelStage();

    void adopt (juce::TextButton& transport,
                juce::Label& levelsEyebrow, juce::Label& levelsHint, juce::Label& diracMicGainHint,
                LevelMeter& meterL, LevelMeter& meterR, LevelMeter& meterOut,
                juce::Label& inputClipHint);

    juce::TextButton& continueButton() { return header_.continueButton(); }
    std::function<void()> onContinue;
    void setRunNote (const juce::String& s) { header_.setRunNote (s); }
    StageHeader& headerForTest() { return header_; }

    // §3.2 soft-gate caution (pure copy rule, headless-tested): Blocked -> the machine reason;
    // un-latched -> the honest caution; latched -> nothing.
    static juce::String levelRunNote (bool blocked, bool latched, const juce::String& machineReason);

    // T10 day-one: the gated no-scroll workflow states. KEEP beside resized()'s conditional branches -
    // any new conditional row added there must appear here; the gate static_asserts the count.
    enum class WorkflowState { Default, ClipWarning, Count };
    static constexpr int kWorkflowStateCount = (int) WorkflowState::Count;

    void applyTheme();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    StageHeader header_ { "STEP 3 OF 4", "Set your level",
                          "Start monitoring, then raise Dirac's output until both meters sit in the green band.",
                          "Continue to measure" };
    juce::Label legend_;                    // "green target -18 to -12 dBFS" (card-head right)
    juce::Rectangle<int> cardBounds_;
    juce::TextButton* transport_ = nullptr;
    juce::Label*  levelsEyebrow_ = nullptr;
    juce::Label*  levelsHint_ = nullptr;
    juce::Label*  diracMicGainHint_ = nullptr;
    LevelMeter*   meterL_ = nullptr; LevelMeter* meterR_ = nullptr; LevelMeter* meterOut_ = nullptr;
    juce::Label*  inputClipHint_ = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelStage)
};

} // namespace eb
