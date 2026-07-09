#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/CaptureCard.h"
#include "gui/GradeMetricDotsView.h"
#include "gui/LevelMeter.h"
#include "gui/stages/StageHeader.h"

namespace eb {

// MeasureStage - P3 rebuild parts 1+2 (spec 5.4 [P3-refresh 2026-07-05]): StageHeader (live headline;
// the CTA slot IS this stage's transport - "Start listening"/"Stop"/"Measure again", §3.3) over a
// scrolled 560px column: ONE lead block (Reference card | the §2 instruction/wait block | the
// hardware-Dirac degraded block), the two-column CaptureCard grid (part 2: Waiting / Capturing /
// Failed per ear, Waiting lead only), the live status line + timeout hint, the live-meters strip,
// and the hardware-Dirac toggle. Part 3 adds the VerdictCards.
// TRANSITIONAL: the old per-ear status lines + quality dots stay adopted below the grid so no
// surface goes dark mid-branch; Task 7 retires them when the VerdictCards supersede.
class MeasureStage : public juce::Component {
public:
    MeasureStage();

    void adopt (juce::Label& statusLine, juce::Label& statusLineR,
                GradeMetricDotsView& gradeDotsL, GradeMetricDotsView& gradeDotsR,
                juce::TextButton& learnRefButton, juce::Label& learnRefResultLabel,
                juce::ToggleButton& hwDiracToggle);

    // ---- the §3.3 transport (the header CTA slot; ONE action, this stage's labels) ----
    juce::TextButton& transportButton() { return header_.continueButton(); }
    std::function<void()> onTransport;

    // ---- view feed (MainComponent::refreshMeasureView; all set-if-changed) ----
    enum class Lead { Reference, Waiting, HwDirac };
    void setLead (Lead l);
    void setHeadCopy (const juce::String& title, const juce::String& sub);
    void setWaitHint (const juce::String& s);              // "" = hidden (the §2 timeout hint)
    void setRunNote (const juce::String& s) { header_.setRunNote (s); }
    void feedLiveLevels (float l, bool clipL, float r, bool clipR, float out, bool clipOut);
    void setActiveEar (int ear);                           // -1 = none (AutoPerEar accent)
    // P3 Task 6: feed the per-ear capture grid (composed by MainComponent::refreshMeasureView from
    // the live capture tracking; each card is set-if-changed, so the 30 Hz feed repaints only on a
    // real change). The grid's visibility is LEAD-driven (Waiting lead only - layoutContent).
    void setCaptureModels (const CaptureCardModel& l, const CaptureCardModel& r);
    CaptureCard& captureCardForTest (int ear) { return ear == 1 ? capR_ : capL_; }

    // ---- pure copy rules (spec §2/§5.4/§7; headless-tested in test_wizardnav.cpp) ----
    struct HeadCopy { juce::String title, sub; };
    static HeadCopy measureHeadCopy (Lead lead, bool overrideOn, bool verdictShowing);
    static juce::String measureRunNote (bool blocked, const juce::String& machineReason,
                                        bool running, bool measureAgainShowing);
    static juce::String waitingHint (int armedSeconds, bool refEndpointMismatch, bool chainMismatch,
                                     const juce::String& chainSummary, bool silentInput);
    static constexpr int kArmedNoSweepHintSeconds = 75;    // §2 heuristic - ON-DEVICE TUNING OWED (#ledger)

    // T10 day-one: gated no-scroll workflow states (grew in Task 6; Task 7 grows it again - the
    // static_assert in the gate fails closed on each growth). KEEP beside layoutContent's branches.
    // Capturing/MidCaptureFailed are APPENDED after HwDirac so the existing gate indices stay stable.
    enum class WorkflowState { ReferenceNeeded, ArmedWaiting, TimeoutHint, HwDirac, Capturing, MidCaptureFailed, Count };
    static constexpr int kWorkflowStateCount = (int) WorkflowState::Count;
    juce::Viewport& viewportForTest() { return viewport_; }
    StageHeader&    headerForTest()   { return header_; }

    void applyTheme();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Content : juce::Component {
        MeasureStage* owner = nullptr;
        void paint (juce::Graphics& g) override;           // the lead/ref/meters card fills
        void resized() override {}
    };
    int layoutContent (int width);

    StageHeader    header_ { "STEP 4 OF 4", "Now run the measurement in Dirac Live",
                             "In Dirac Live, choose CABLE Input (VB-Audio) as the recording device and click Start measurement. EARS Bridge listens and grades each earcup as it lands.",
                             "Start listening" };
    juce::Viewport viewport_;
    Content        content_;
    Lead           lead_ = Lead::Waiting;

    // Reference lead block (a hairline card around the adopted learn controls).
    juce::Label refLead_;                                  // why-copy inside the reference card
    // Wait block: the adopted statusLine renders the ladder line; waitHint_ is the §2 timeout hint.
    juce::Label waitHint_;
    // Hardware-Dirac degraded block.
    juce::Label hwLead_;
    // P3 Task 6: the two-column per-ear capture grid (Waiting lead only; views - grading untouched).
    CaptureCard capL_, capR_;
    // Live-meters strip (this stage's OWN instances - components are single-parent; same data truth).
    juce::Label meterTitle_, meterLegend_;
    LevelMeter  liveL_ { "L" }, liveR_ { "R" }, liveOut_ { "Out" };
    std::vector<juce::Rectangle<int>> cardRects_;          // lead card + meters card fills

    juce::Label*         statusLine_ = nullptr;
    juce::Label*         statusLineR_ = nullptr;           // transitional (retires in Task 7)
    GradeMetricDotsView* gradeDotsL_ = nullptr;            // transitional (retires in Task 7)
    GradeMetricDotsView* gradeDotsR_ = nullptr;            // transitional (retires in Task 7)
    juce::TextButton*    learnRefButton_ = nullptr;
    juce::Label*         learnRefResultLabel_ = nullptr;
    juce::ToggleButton*  hwDiracToggle_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasureStage)
};

} // namespace eb
