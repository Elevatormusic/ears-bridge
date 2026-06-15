#include "audio/ModelDetect.h"
namespace eb {

EarsModel detectEarsModel (const juce::String& deviceName, const juce::String& usbId) {
    const auto name = deviceName.toLowerCase();
    const auto usb  = usbId.toUpperCase();

    // USB identity is the most reliable signal. miniDSP VID = 2752 (0x0AC0).
    // EARS Pro is the XMOS UAC2 interface; original EARS is the UAC1 interface.
    if (usb.contains ("VID_2752")) {
        if (usb.contains ("PID_0046") || usb.contains ("PRO")) return EarsModel::EarsPro;
        return EarsModel::Ears;
    }

    // Name-based fallback. Check "pro" first so "EARS Pro" never falls through to Ears.
    // Some Windows drivers expose the unit as "E.A.R.S" (dotted) — e.g.
    // "Microphone (E.A.R.S Gain: 18dB)" — so strip '.' before matching, or "ears" never hits.
    const auto noDots = name.removeCharacters (".");
    const bool mentionsEars = noDots.contains ("ears");
    if (mentionsEars && (noDots.contains ("ears pro")
                          || noDots.replace (" ", "").contains ("earspro")))
        return EarsModel::EarsPro;
    if (mentionsEars)
        return EarsModel::Ears;

    return EarsModel::Unknown;
}

std::vector<double> nativeSampleRates (EarsModel model) {
    switch (model) {
        case EarsModel::Ears:    return {48000.0};
        case EarsModel::EarsPro: return {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
        case EarsModel::Unknown: default: return {};
    }
}

std::vector<int> nativeBitDepths (EarsModel model) {
    switch (model) {
        case EarsModel::Ears:    return {24};
        case EarsModel::EarsPro: return {16, 24, 32};
        case EarsModel::Unknown: default: return {};
    }
}

} // namespace eb
