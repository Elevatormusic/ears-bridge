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

// macOS does not (yet) expose the full WASAPI-style mix-format struct here; the full read is a Windows
// feature. Return {valid=false} so callers treat it as "couldn't read", never as a good format. (The
// pure interpretMixFormat lives in the cross-platform body of EndpointFormat.cpp and is built on macOS.)
EndpointFormat readEndpointFormat (const juce::String&, bool) { return {}; }

} // namespace eb
#endif
