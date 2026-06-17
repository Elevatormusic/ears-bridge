#pragma once
#include "audio/EngineTypes.h"

namespace eb {

// Maps the flags of an INVALID capture (cleanCapture == false) to an honest, specific message.
// Order = most actionable first. Pure + header-only so it is unit-testable without the GUI.
[[nodiscard]] inline const char* invalidMeasurementMessage (HealthFlag flags) noexcept {
    if (any (flags & HealthFlag::ClipConfirmed))
        return "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.";
    if (any (flags & HealthFlag::NonFinite))
        return "Measurement invalidated by a corrupted audio sample.";
    return "Dropouts detected - this measurement is invalid.";
}

} // namespace eb
