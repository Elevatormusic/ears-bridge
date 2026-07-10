#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/CombineMode.h"   // P3 T7 ruling: the waiting sub speaks the combine-mode truth
#include "gui/MotionRamp.h"      // P4: the capturing border's one-shot ease-in

namespace eb {

// One ear's capture-state card (spec §5.4 [P3-refresh]): Waiting (dim, no bar), Capturing (accent
// border + DATA-DRIVEN progress - elapsed over the LEARNED reference duration, labeled approximate),
// Failed (danger border - the §3.1 mid-capture regression owns the moment). Motion (P4 Task 3): the
// ONE ease here is the accent border fading in on Waiting -> Capturing; the Failed/danger border
// SNAPS always (frozen decision 3), and progress stays DATA-driven — the bar moves only on model
// changes, never interpolated. All tone-bearing text is Labels (RULE2 walker).
struct CaptureCardModel {
    enum class State { Waiting, Capturing, Failed };
    State state = State::Waiting;
    juce::String ear, badge, title, sub, foot;
    float progress = -1.0f;                     // 0..1 draws the bar; < 0 draws an EMPTY track

    // P3 Task 7 ruling (user): the waiting sub is MODE-AWARE - the Auto copy on a Two-pass/Combined
    // configuration described a routine that isn't running (mode-wrong copy is a trust bug). No
    // default arg on purpose: every caller must state whose truth it renders.
    static CaptureCardModel waiting (const char* ear, CombineMode mode);
    static CaptureCardModel capturing (const char* ear, float fraction, int sweepSeconds);
    static CaptureCardModel failed (const char* ear);
    bool operator== (const CaptureCardModel& o) const {
        return state == o.state && ear == o.ear && badge == o.badge && title == o.title
            && sub == o.sub && foot == o.foot && juce::approximatelyEqual (progress, o.progress);
    }
};

class CaptureCard : public juce::Component {
public:
    CaptureCard();
    void setModel (const CaptureCardModel& m);  // set-if-changed (30 Hz feed - no repaint churn)
    // Elapsed GUI ticks (30 Hz) over the learned per-ear sweep duration, clamped [0,1]; 0 when the
    // duration is unknown (never claim progress the app can't measure). Pure, headless-tested.
    static float captureFraction (int elapsedTicks, double sweepSeconds);
    juce::Label& titleForTest() { return title_; }
    juce::Label& badgeForTest() { return badge_; }   // P3 Task 7: the theme re-tone pin (applyModelTone)
    MotionRamp&  borderRampForTest() { return borderRamp_; }   // P4: the capturing-border ease seam
    void applyTheme();
    void paint (juce::Graphics&) override;
    void resized() override;
private:
    // State-mapped tone (badge/title colours). Shared by setModel and applyTheme: a theme flip changes
    // the palette while the MODEL is unchanged, so re-routing applyTheme through the set-if-changed
    // setModel would no-op and strand the old palette's danger/accent on the card (brief defect, fixed).
    void applyModelTone();
    CaptureCardModel model_;
    MotionRamp borderRamp_;      // P4: eases ONLY the Capturing accent border in (danger never ramps)
    juce::Label ear_, badge_, title_, sub_, foot_;
    juce::Rectangle<int> progressArea_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureCard)
};

} // namespace eb
