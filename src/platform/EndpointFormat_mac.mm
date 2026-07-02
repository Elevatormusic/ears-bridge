#include "platform/EndpointFormat.h"
#if JUCE_MAC
#include <CoreAudio/CoreAudio.h>

namespace eb {

// CoreAudio: the device's nominal sample rate. (The aggregate sub-device's true hardware rate; if the
// requested rate differs, CoreAudio is converting.) Matches the device whose name equals deviceName.
double endpointMixSampleRateForName (const juce::String& deviceName, bool /*isInput*/) {
    AudioObjectPropertyAddress devicesAddr {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &size) != noErr)
        return 0.0;
    const int n = (int) (size / sizeof (AudioDeviceID));
    if (n <= 0) return 0.0;
    juce::HeapBlock<AudioDeviceID> ids (n);
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &size, ids) != noErr)
        return 0.0;

    for (int i = 0; i < n; ++i) {
        AudioObjectPropertyAddress nameAddr {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef cfName = nullptr; UInt32 ns = sizeof (cfName);
        if (AudioObjectGetPropertyData (ids[i], &nameAddr, 0, nullptr, &ns, &cfName) != noErr || cfName == nullptr)
            continue;
        const juce::String nm = juce::String::fromCFString (cfName);
        CFRelease (cfName);
        if (nm != deviceName) continue;

        AudioObjectPropertyAddress rateAddr {
            kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        Float64 rate = 0.0; UInt32 rs = sizeof (rate);
        if (AudioObjectGetPropertyData (ids[i], &rateAddr, 0, nullptr, &rs, &rate) == noErr)
            return (double) rate;
    }
    return 0.0;
}

// #55: RATE-ONLY read. macOS resolves the device's nominal sample rate (above) but not the full
// WASAPI-style mix format. The old unconditional {valid=false} made the whole 48k chain gate INERT
// on macOS: all three endpoints "unreadable" -> ConfigVerdict.checked == false -> total silence, the
// exact fail-open the audit flagged. A rate-only VALID format lets the RATE gate (the one hard gate)
// run on macOS; channels/bits stay 0 = "not reported", and the advisory layer skips zero fields
// (DeviceConfigCheck guards on channels > 0 / bits > 0) so no false mono/16-bit notes appear.
EndpointFormat readEndpointFormat (const juce::String& nameOrUid, bool isInput) {
    const double rate = endpointMixSampleRateForName (nameOrUid, isInput);
    if (rate <= 0.0) return {};   // couldn't resolve: honest "couldn't read", never a pass
    EndpointFormat f;
    f.valid     = true;
    f.mixRateHz = rate;
    return f;                     // channels/bits/isFloat left 0/false: "not reported" on this path
}

} // namespace eb
#endif
