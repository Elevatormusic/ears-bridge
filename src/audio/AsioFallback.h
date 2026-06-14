#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Minimal, mockable view of an AudioIODeviceType for the routing decision.
struct DeviceTypeCaps {
    juce::String typeName;                 // e.g. "ASIO", "Windows Audio" (WASAPI), "CoreAudio"
    bool         separateInputsAndOutputs = true;  // AudioIODeviceType::hasSeparateInputsAndOutputs()
};

struct RoutingDecision {
    bool         mustFallback = false;     // chosen type can't bridge two devices
    juce::String chosenTypeName;           // the type the engine should actually use
    juce::String message;                  // user-facing explanation ("" if no fallback)
};

// Given the preferred type and the available types, decide whether to fall back to a
// cross-device-capable type (WASAPI on Windows, CoreAudio on macOS). Pure; no devices.
class AsioFallback {
public:
    // The cross-device-capable type names we accept, in preference order.
    static juce::StringArray bridgeCapableTypeNames();

    static RoutingDecision decide (const DeviceTypeCaps& preferred,
                                   const juce::Array<DeviceTypeCaps>& available);
};

} // namespace eb
