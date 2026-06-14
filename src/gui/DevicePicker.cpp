#include "gui/DevicePicker.h"
#include "gui/Theme.h"

namespace eb {

static juce::String modelTag (EarsModel m) {
    switch (m) {
        case EarsModel::Ears:    return "  [EARS]";
        case EarsModel::EarsPro: return "  [EARS Pro]";
        default:                 return {};
    }
}

DevicePicker::DevicePicker (juce::String caption) {
    label.setText (caption, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, Theme::textDim());
    label.setFont (juce::Font (11.0f, juce::Font::bold));
    addAndMakeVisible (label);

    combo.setTextWhenNoChoicesAvailable ("no devices found");
    combo.setTextWhenNothingSelected ("select a device");
    addAndMakeVisible (combo);

    combo.onChange = [this] {
        const int idx = combo.getSelectedId() - 1;
        if (idx >= 0 && idx < (int) items.size() && onDeviceChosen)
            onDeviceChosen (items[(size_t) idx]);
    };
}

juce::String DevicePicker::rowText (const DeviceId& d) {
    juce::String t = d.name;
    t += modelTag (d.model);
    if (d.isVirtualSink) t += "  (virtual)";
    return t;
}

void DevicePicker::setDevices (const std::vector<DeviceId>& devices, const juce::String& selectedKey) {
    items = devices;
    combo.clear (juce::dontSendNotification);
    int selectId = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const int id = (int) i + 1;
        combo.addItem (rowText (items[i]), id);
        if (selectedKey.isNotEmpty() && items[i].key() == selectedKey) selectId = id;
    }
    if (selectId != 0) combo.setSelectedId (selectId, juce::dontSendNotification);
}

std::optional<DeviceId> DevicePicker::selectedDevice() const {
    const int idx = combo.getSelectedId() - 1;
    if (idx >= 0 && idx < (int) items.size()) return items[(size_t) idx];
    return std::nullopt;
}

void DevicePicker::resized() {
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    combo.setBounds (r.removeFromTop (28));
}

} // namespace eb
