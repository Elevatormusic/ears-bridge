#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

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
protected:
    void clicked() override;
private:
    juce::String summary_;
    bool locked_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisclosureRow)
};
}
