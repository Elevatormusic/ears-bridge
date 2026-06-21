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

// The FULL shared-mode mix format of an endpoint (the OS ground truth in shared mode). Promoted from the
// eb_diag GetMixFormat read so the app can read any endpoint's rate/channels/bits/float/exclusive — not
// just the input's rate. `valid == false` means the read failed (endpoint not found, platform without a
// mix-format side channel, or a COM/CoreAudio failure); callers MUST treat that as "couldn't read", never
// as a good format.
struct EndpointFormat {
    bool   valid = false;        // false => couldn't read (don't treat as "good")
    double mixRateHz = 0.0;      // GetMixFormat rate (the OS ground truth in shared mode)
    int    channels = 0;
    int    bits = 0;             // mix-format bits
    bool   isFloat = false;      // WAVE_FORMAT_IEEE_FLOAT (or EXTENSIBLE float subformat)
    bool   exclusive48kSupported = false;  // IsFormatSupported(EXCLUSIVE, 48k/this-format)
};

// Read the full mix format of an endpoint. Prefer matching by stable UID (EndpointUid); fall back to a
// case-insensitive FriendlyName contains-match. isInput selects eCapture vs eRender. valid=false if not
// found. Non-Windows returns {valid=false}.
[[nodiscard]] EndpointFormat readEndpointFormat (const juce::String& nameOrUid, bool isInput);

// PURE, cross-platform interpreter of raw WAVEFORMATEX fields into an EndpointFormat. Extracted so the
// field logic (esp. float-vs-int derivation) can be unit-tested without a live endpoint or the Windows
// WAVEFORMATEX type. `formatTag` is the WAVEFORMATEX wFormatTag; `extensibleSubFormatIsFloat` is whether
// a WAVE_FORMAT_EXTENSIBLE blob's SubFormat is KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (ignored otherwise).
// isFloat is true when tag == WAVE_FORMAT_IEEE_FLOAT (3) OR (tag == WAVE_FORMAT_EXTENSIBLE (0xFFFE) AND
// extensibleSubFormatIsFloat). Always returns valid=true (the caller decides whether a read happened).
[[nodiscard]] EndpointFormat interpretMixFormat (unsigned formatTag, unsigned long rateHz,
                                                 unsigned channels, unsigned bits,
                                                 bool extensibleSubFormatIsFloat,
                                                 bool exclusive48kSupported);

} // namespace eb
