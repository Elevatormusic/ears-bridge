#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"

namespace eb {

// Honest post-start note for the raw-rail (D2) state. SILENT when verified or before a run (so a clean
// run leaves the warning label blank); speaks only when the OS resampled our stream or the endpoint
// rate could not be confirmed. Header-only + pure (mirrors gui/ClipStatus.h). See AUDIT D2.
[[nodiscard]] inline juce::String rawRailNote (const RawRailState& rr) {
    if (rr.verified) return {};                       // no SRC on our stream -> no warning
    if (rr.requestedRate <= 0.0) return {};           // no run yet
    if (rr.mixRate > 0.0)
        return "OS-resampled to " + juce::String (rr.mixRate / 1000.0, 1)
             + " kHz (endpoint mix rate) vs the requested " + juce::String (rr.requestedRate / 1000.0, 1)
             + " kHz - clip detection approximate.";
    return "Could not confirm the EARS endpoint rate - clip detection approximate.";
}

} // namespace eb
