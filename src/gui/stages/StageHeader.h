#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace eb {
// The shared frame-style stage head (.stage-head): eyebrow / title / lead left, CTA capsule +
// run-note cluster right. FIXED above each stage's viewport so the CTA is always reachable.
class StageHeader : public juce::Component {
public:
    StageHeader (const juce::String& eyebrow, const juce::String& title,
                 const juce::String& sub, const juce::String& ctaText);
    juce::TextButton& continueButton() { return cta_; }
    void setRunNote (const juce::String& s);       // set-if-changed; 11px textDim, right-aligned
    // P3: Measure's headline is LIVE (armed/verdict/reference/hw variants) - set-if-changed setters.
    // setTitleText relayouts on a real change: the title slot is MEASURED (1 vs 2 rows, resized()),
    // so a title change can change the slot even though the header's own bounds are constant.
    void setTitleText (const juce::String& s) {
        if (title_.getText() == s) return;
        title_.setText (s, juce::dontSendNotification);
        resized();
    }
    void setSubText   (const juce::String& s) { if (sub_.getText()   != s) sub_.setText   (s, juce::dontSendNotification); }
    // The owner names its own run-note element (findable in the layout probe / tests). SHARED class:
    // each stage instance must pick a UNIQUE id or a second stage would duplicate the first's (a
    // Calibrate-prefixed "calRunNote" would collide with e.g. Connect's own run-note - #T7 defuse).
    void setRunNoteComponentID (const juce::String& id) { runNote_.setComponentID (id); }
    void applyTheme();
    void resized() override;
    static constexpr int kHeight = 96;   // T10 ledger: 12+16+4+26+4+34
    // P3 Task 5 (measured): the frozen Measure §2 subs lay out at 39px (3 lines of 13px) in the
    // 294px sub box - the 34px slot clips the third line. The sub row is now FLEXIBLE (it takes
    // whatever height remains, == 34 at kHeight, so Connect/Calibrate/Level are pixel-identical);
    // a stage whose copy needs 3 lines reserves kHeight + kSubExtraRow for its header area.
    static constexpr int kSubExtraRow = 12;
    // P3 Task 6 (routed ruling, Task 5's review): 2-line title mode. The title's horizontal scale is
    // pinned 1.0 (house law - the hero instruction never squishes; the armed Measure title rendered
    // at 0.73 before). resized() text-measures the title: wider than the title box -> it takes a
    // second 26px row. A stage whose copy can go wide reserves kHeight + kTitleExtraRow; one-line
    // heads keep today's geometry (the sub row stays flexible-last either way).
    static constexpr int kTitleExtraRow = 26;
    static constexpr int kCtaW = 240, kCtaBtnW = 200, kCtaBtnH = 34, kRunNoteH = 14;
    juce::Label& titleForTest() { return title_; }   // the fit gate pins the 1.0 scale + the 2-line mode
private:
    juce::Label eyebrow_, title_, sub_, runNote_;
    juce::TextButton cta_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageHeader)
};
}
