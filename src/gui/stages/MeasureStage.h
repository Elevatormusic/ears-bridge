#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/GradeMetricDotsView.h"

// MeasureStage — the terminal "Measure" wizard stage (P1 Task 3, spec §5.4). Hosts the two per-ear
// status lines + the quality dots + the Learn-reference flow + the hardware-Dirac toggle, re-homed from
// the title bar / rail. Rendering is UNCHANGED (the same labels/dots, now in the stage body — the
// verdict card replaces them in Phase 3). No Continue button: Measure is the terminal step (§5.4).

namespace eb {

class MeasureStage : public juce::Component {
public:
    MeasureStage();

    void adopt (juce::Label& statusLine, juce::Label& statusLineR,
                GradeMetricDotsView& gradeDotsL, GradeMetricDotsView& gradeDotsR,
                juce::TextButton& learnRefButton, juce::Label& learnRefResultLabel,
                juce::ToggleButton& hwDiracToggle);

    void applyTheme();   // re-colour the stage's own labels on a live light/dark flip

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::Label eyebrow_;   // "MEASURE"
    juce::Label leadLabel_; // one-line lead ("Run the measurement in Dirac Live; each earcup is graded.")

    juce::Label*         statusLine_ = nullptr;
    juce::Label*         statusLineR_ = nullptr;
    GradeMetricDotsView* gradeDotsL_ = nullptr;
    GradeMetricDotsView* gradeDotsR_ = nullptr;
    juce::TextButton*    learnRefButton_ = nullptr;
    juce::Label*         learnRefResultLabel_ = nullptr;
    juce::ToggleButton*  hwDiracToggle_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasureStage)
};

} // namespace eb
