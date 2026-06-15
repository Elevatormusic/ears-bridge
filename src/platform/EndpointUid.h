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

} // namespace eb
