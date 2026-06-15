#include "audio/DeviceId.h"
namespace eb {

// The EARS endpoint name carries its gain-DIP setting, e.g. "Microphone (E.A.R.S Gain: 18dB)".
// Moving the DIP re-enumerates the device under a new name, which historically changed key() and
// made the saved input selection stop matching. Drop the "Gain: NdB" token from an EARS key so the
// identity is stable across gain changes (the displayed name still shows the gain). Non-EARS names
// are left untouched. Belt-and-braces with the model fallback in DevicePicker::setDevices().
static juce::String stripEarsGain (const juce::String& s) {
    const int g = s.toLowerCase().indexOf ("gain:");
    if (g < 0) return s;
    const int dbRel = s.substring (g).toLowerCase().indexOf ("db");   // end of the "...NdB" token
    if (dbRel < 0) return s;
    return (s.substring (0, g) + s.substring (g + dbRel + 2)).trim();
}

juce::String DeviceId::key() const {
    // A real platform endpoint id (uid != name, resolved by DeviceManager via WASAPI/CoreAudio) is
    // replug-, rename- AND gain-DIP-stable, so key on it alone -- the name (with its gain token and
    // rename churn) drops out of the identity entirely.
    if (uid.isNotEmpty() && uid != name)
        return typeName + "|" + uid;
    // Fallback: no endpoint id resolved (uid == name), so the key is name-stable only. Strip the EARS
    // gain token so at least a gain-DIP change survives.
    if (model != EarsModel::Unknown)
        return typeName + "|" + stripEarsGain (name);
    return typeName + "|" + name;
}

bool DeviceId::operator== (const DeviceId& other) const {
    return typeName == other.typeName
        && name == other.name
        && uid == other.uid
        && isVirtualSink == other.isVirtualSink
        && model == other.model;
}

} // namespace eb
