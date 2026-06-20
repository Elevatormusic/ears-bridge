#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// The shared-mode MIX-FORMAT sample rate the OS runs this endpoint at (Hz). In WASAPI shared mode the
// OS resampler (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) converts between OUR requested rate and this mix
// rate; if our requested rate equals this, the OS did not resample our stream (raw rails). Returns 0.0
// when it can't be resolved (unknown platform, name not matched, COM/CoreAudio failure) so the caller
// treats "unknown" as unverifiable, NOT verified. isInput selects capture (true) vs render (false) on
// Windows; CoreAudio devices are not flow-specific so macOS ignores it.
double endpointMixSampleRateForName (const juce::String& deviceName, bool isInput);

} // namespace eb
