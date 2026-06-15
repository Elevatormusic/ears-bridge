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
    label.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")).withExtraKerningFactor (0.07f));
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
    // Model fallback: device keys are name-stable only (the EARS name carries its gain-DIP setting,
    // e.g. "...Gain: 18dB", so moving the DIP changes the name and the saved key no longer matches).
    // When there WAS a saved selection that didn't match, but exactly one recognised EARS is present,
    // re-select it so the user's input survives a gain change. (Outputs carry no model, so this is a
    // no-op for the output picker.) Full replug-stable endpoint UIDs remain a native-platform follow-up.
    if (selectId == 0 && selectedKey.isNotEmpty()) {
        int modelMatches = 0, modelId = 0;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].model != EarsModel::Unknown) { ++modelMatches; modelId = (int) i + 1; }
        if (modelMatches == 1) selectId = modelId;
    }
    // Output fallback: cables carry no model, so the EARS fallback above never fires for them. If a
    // saved output cable's name changed (rename / driver reinstall) and exactly one virtual sink is
    // present, re-select it -- the output-side mirror of the EARS gain-DIP recovery.
    if (selectId == 0 && selectedKey.isNotEmpty()) {
        int sinkMatches = 0, sinkId = 0;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].isVirtualSink) { ++sinkMatches; sinkId = (int) i + 1; }
        if (sinkMatches == 1) selectId = sinkId;
    }
    if (selectId != 0) combo.setSelectedId (selectId, juce::dontSendNotification);
}

std::optional<DeviceId> DevicePicker::selectedDevice() const {
    const int idx = combo.getSelectedId() - 1;
    if (idx >= 0 && idx < (int) items.size()) return items[(size_t) idx];
    return std::nullopt;
}

void DevicePicker::applyTheme() {
    label.setColour (juce::Label::textColourId, Theme::textDim());
    repaint();
}

void DevicePicker::resized() {
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    r.removeFromTop (6);
    combo.setBounds (r.removeFromTop (40));
}

} // namespace eb
