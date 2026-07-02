#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Resolve a device's STABLE platform endpoint id from the display name JUCE enumerates it under.
//   Windows: the WASAPI IMMDevice id (IMMDevice::GetId) -- stable across replug, rename, and the
//            EARS gain-DIP name change.
//   macOS:   kAudioDevicePropertyDeviceUID.
// Returns {} when it can't resolve (unknown platform, name not matched, or COM/CoreAudio failure) so
// callers fall back to the name (name-stable only). `isInput` picks the capture (true) vs render
// (false) endpoint flow on Windows; CoreAudio devices aren't flow-specific so macOS ignores it.
juce::String endpointUidForName (const juce::String& deviceName, bool isInput);

// #63: resolve ALL of a flow's endpoints in ONE enumeration (display name -> endpoint id). rescan()
// previously called endpointUidForName per device - a complete enumeration + property-store read each,
// O(N^2) COM work on the message thread at launch and on every hot-plug event. Empty on non-Windows /
// failure - callers fall back to the per-name resolver.
// #28: the map is keyed by the JUCE DISPLAY name (jucifyEndpointNames below), not the raw FriendlyName,
// so duplicate-named endpoints resolve to the RIGHT id ("Name" vs "Name (2)").
juce::StringPairArray endpointUidsForFlow (bool isInput);

// #28 (pure, unit-tested): reproduce EXACTLY how JUCE builds the display names it shows for a flow's
// endpoints (juce_WASAPI_windows.cpp), so a JUCE device name keys the SAME endpoint here:
//   1. the flow's DEFAULT endpoint (GetDefaultAudioEndpoint(flow, eMultimedia)) is MOVED TO THE FRONT
//      (JUCE inserts it at index 0; the rest keep their enumeration order);
//   2. duplicate FriendlyNames get appendNumbersToDuplicates(false, false): the first instance keeps
//      the bare name, later instances become "Name (2)", "Name (3)", ...
// The old first-exact-match keying attributed the WRONG endpoint's UID whenever Windows exposed two
// devices with one friendly name - JUCE shows "Name" and "Name (2)", and BOTH matched bare "Name".
// `defaultIndex` = the default endpoint's index in `names` (enumeration order; -1 = none/unknown).
// Returns display names index-parallel to the REORDERED list: element 0 is the default endpoint's
// display name when defaultIndex >= 0 (callers must reorder their id list the same way).
[[nodiscard]] inline juce::StringArray jucifyEndpointNames (juce::StringArray names, int defaultIndex) {
    if (defaultIndex > 0 && defaultIndex < names.size()) {
        const juce::String def = names[defaultIndex];
        names.remove (defaultIndex);
        names.insert (0, def);
    }
    names.appendNumbersToDuplicates (false, false);   // the SAME juce call the WASAPI device list uses
    return names;
}

} // namespace eb
