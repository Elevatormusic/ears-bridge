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

// #63: resolve ALL of a flow's endpoints in ONE enumeration (FriendlyName -> endpoint id). rescan()
// previously called endpointUidForName per device - a complete enumeration + property-store read each,
// O(N^2) COM work on the message thread at launch and on every hot-plug event. Empty on non-Windows /
// failure - callers fall back to the per-name resolver.
juce::StringPairArray endpointUidsForFlow (bool isInput);

} // namespace eb
