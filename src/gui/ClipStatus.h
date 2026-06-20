#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"

namespace eb {

// Maps the flags of an INVALID capture (cleanCapture == false) to an honest, specific message.
// Order = most actionable first. Pure + header-only so it is unit-testable without the GUI.
// Returns juce::String (not const char*) because the confirmed-clip branch may append a D2 caveat.
[[nodiscard]] inline juce::String invalidMeasurementMessage (HealthFlag flags) {
    if (any (flags & HealthFlag::ClipConfirmed)) {
        juce::String msg = "Input reached digital full scale - this measurement is invalid. "
                           "Lower the level and repeat.";
        if (any (flags & HealthFlag::OsResampled)) msg += " (OS-resampled - approximate)";
        return msg;
    }
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    if (any (flags & HealthFlag::SweepRetimed))           // D6: clock-retiming is a more specific cause than generic drift
        return "Clock drift retimed the sweep - this measurement is invalid. Repeat the measurement.";
    if (any (flags & HealthFlag::FormatChanged))          // D8: device-format renegotiation is more specific than generic drift
        return "Audio device format changed mid-run - this measurement is invalid. Prevent sleep/wake during capture.";
    if (any (flags & HealthFlag::ExcessDrift))            // preserved from the clipping-review-fixes slice
        return "Sample-clock drift detected - this measurement is invalid.";
    return "Dropouts detected - this measurement is invalid.";
}

} // namespace eb
