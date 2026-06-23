#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// The current RENDER peak (0..1) of the output endpoint whose FriendlyName CONTAINS `deviceName`, via
// WASAPI IAudioMeterInformation::GetPeakValue. Returns -1.0f when UNKNOWN (non-Windows / no match / COM
// failure) — distinct from 0.0f (a readable, silent output). Runs OFF the audio thread (COM + device
// enumeration), like platform/InputVolume / platform/EndpointFormat.
//
// Used by the hardware-Dirac detector: software Dirac's sweep IS a PC render stream (peak > 0); a hardware
// box generates its sweep internally, so its PC output stays silent (peak ~ 0) even while the mic hears the
// sweep. The -1 "unknown" must never be read as "silent" (see HardwareDiracDetect::sweepWasInternal).
[[nodiscard]] float outputRenderPeakForName (const juce::String& deviceName);

} // namespace eb
