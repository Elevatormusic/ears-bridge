#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "gui/MotionRamp.h"

namespace eb {

// A one-row progressive-disclosure header ("Advanced FIR", "Not using Dirac?"): chevron +
// title + an always-visible dim summary (so a collapsed section can never HIDE a non-default
// setting - the summary states it). Toggling shows/hides the section the OWNER lays out
// beneath it; this row renders + reports state only. setLocked keeps it OPEN - honesty rule:
// a section holding an ACTIVE escape hatch may never be collapsible.
class DisclosureRow : public juce::Button {
public:
    explicit DisclosureRow (const juce::String& title);
    void setOpen (bool open);                      // no-op if unchanged; fires onOpenChanged
    bool isOpen() const { return getToggleState(); }
    void setLocked (bool locked);                  // locked -> force-open, collapse refused
    void setSummary (const juce::String& s);       // always-visible dim summary, right-aligned
    std::function<void (bool nowOpen)> onOpenChanged;
    void clickForTest();                           // documented seam: flip + clicked(), sync
    void paintButton (juce::Graphics&, bool over, bool down) override;
    int summaryAvailableWidth() const;             // width left for the summary after the measured title column (test seam)
    // P4 motion: the chevron eases 0<->90 degrees over MotionRamp::kMotionMs (frame-exact .15s-ish).
    // amount 0 = closed glyph, 1 = open glyph; mid-values only while the ramp runs.
    float chevronAmountForTest() const { return getToggleState() ? chevRamp_.value() : 1.0f - chevRamp_.value(); }
    MotionRamp& chevronRampForTest() { return chevRamp_; }
protected:
    void clicked() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    juce::Rectangle<int> layoutTitleColumn (juce::Rectangle<int>& content) const; // single source: carves the title column off content, returns the summary remainder
    juce::String summary_;
    MotionRamp chevRamp_;                          // P4: rotation ease, both directions (state supplies the direction)
    bool locked_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisclosureRow)
};
}
