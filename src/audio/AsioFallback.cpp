#include "audio/AsioFallback.h"

namespace eb {

juce::StringArray AsioFallback::bridgeCapableTypeNames() {
    // WASAPI is exposed by JUCE under the type name "Windows Audio";
    // macOS CoreAudio is "CoreAudio". Both have hasSeparateInputsAndOutputs()==true.
    return { "Windows Audio", "CoreAudio" };
}

RoutingDecision AsioFallback::decide (const DeviceTypeCaps& preferred,
                                      const juce::Array<DeviceTypeCaps>& available) {
    RoutingDecision out;

    if (preferred.separateInputsAndOutputs) {
        out.mustFallback = false;
        out.chosenTypeName = preferred.typeName;
        return out;   // already bridge-capable; keep it
    }

    out.mustFallback = true;

    // Pick the first available bridge-capable type, in our preference order.
    for (auto& wanted : bridgeCapableTypeNames()) {
        for (auto& cap : available) {
            if (cap.typeName == wanted && cap.separateInputsAndOutputs) {
                out.chosenTypeName = cap.typeName;
                out.message =
                    "The '" + preferred.typeName + "' driver cannot route a capture device to a "
                    "different output (ASIO has no separate inputs/outputs). Falling back to '"
                    + cap.typeName + "'. The EARS Pro vendor ASIO driver must not be used here.";
                return out;
            }
        }
    }

    // No bridge-capable type present.
    out.chosenTypeName = juce::String();
    out.message =
        "The selected '" + preferred.typeName + "' driver cannot bridge two devices, and no "
        "WASAPI/CoreAudio driver was found. Install/enable the platform's shared audio driver.";
    return out;
}

} // namespace eb
