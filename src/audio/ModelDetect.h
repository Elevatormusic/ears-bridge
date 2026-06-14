#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"
#include <vector>
namespace eb {

// Detect the EARS model from a device display name and an optional USB id string
// (VID:PID or hardware-id, as the OS reports it). Pure; no device access.
EarsModel detectEarsModel (const juce::String& deviceName,
                           const juce::String& usbId = {});

// Per-model native whitelists. Unknown -> empty (caller must not offer rates).
std::vector<double> nativeSampleRates (EarsModel model);   // EARS -> {48000}; EARS Pro -> {44100,48000,88200,96000,176400,192000}
std::vector<int>    nativeBitDepths   (EarsModel model);   // EARS -> {24}; EARS Pro -> {16,24,32}

} // namespace eb
