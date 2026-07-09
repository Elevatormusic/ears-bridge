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
    void setTitleText (const juce::String& s) { if (title_.getText() != s) title_.setText (s, juce::dontSendNotification); }
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
    static constexpr int kCtaW = 240, kCtaBtnW = 200, kCtaBtnH = 34, kRunNoteH = 14;
private:
    juce::Label eyebrow_, title_, sub_, runNote_;
    juce::TextButton cta_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageHeader)
};
}
