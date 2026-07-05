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
    // The owner names its own run-note element (findable in the layout probe / tests). SHARED class:
    // each stage instance must pick a UNIQUE id or a second stage would duplicate the first's (a
    // Calibrate-prefixed "calRunNote" would collide with e.g. Connect's own run-note - #T7 defuse).
    void setRunNoteComponentID (const juce::String& id) { runNote_.setComponentID (id); }
    void applyTheme();
    void resized() override;
    static constexpr int kHeight = 124;
    static constexpr int kCtaW = 240, kCtaBtnW = 200, kCtaBtnH = 34, kRunNoteH = 14;
private:
    juce::Label eyebrow_, title_, sub_, runNote_;
    juce::TextButton cta_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageHeader)
};
}
