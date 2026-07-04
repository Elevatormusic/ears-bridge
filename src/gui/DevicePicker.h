#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "audio/DeviceId.h"     // eb::DeviceId, eb::EarsModel (Plan 2 SPINE)
#include <vector>
#include <functional>
#include <optional>

namespace eb {

// A labelled ComboBox bound to a list of DeviceId. Each row shows the device name and,
// for inputs, the detected EARS / EARS Pro model tag; virtual sinks are tagged
// "(virtual)" for outputs. Selecting a row fires onDeviceChosen with the DeviceId.
class DevicePicker : public juce::Component {
public:
    explicit DevicePicker (juce::String caption);

    // Repopulate from a device list, preselecting the device whose key() matches
    // selectedKey (if present). Does NOT fire onDeviceChosen.
    void setDevices (const std::vector<DeviceId>& devices, const juce::String& selectedKey = {});

    std::optional<DeviceId> selectedDevice() const;
    void applyTheme();   // re-apply theme-dependent colours (live light/dark switch)

    std::function<void (const DeviceId&)> onDeviceChosen;

    void resized() override;

    // HIG H2: the inner ComboBox (the control a screen reader actually focuses) carries its own accessible
    // name, derived from the caption in the ctor — the eyebrow Label is not auto-associated to the combo.
    // Exposed read-only for the headless a11y test (test_wizardnav.cpp).
    juce::String comboTitleForTest() const { return combo.getTitle(); }

private:
    static juce::String rowText (const DeviceId&);
    // Human accessible name for the inner combo, derived from the picker caption ("INPUT" -> "Input device",
    // "OUTPUT (VIRTUAL CABLE)" -> "Output virtual cable"). Static + pure so it is testable in isolation.
    static juce::String comboTitleFromCaption (const juce::String& caption);

    juce::Label label;
    juce::ComboBox combo;
    std::vector<DeviceId> items;   // index i -> combo item id (i+1)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DevicePicker)
};

} // namespace eb
