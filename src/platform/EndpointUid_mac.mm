#include "platform/EndpointUid.h"

#if JUCE_MAC

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <vector>

namespace eb {

// CoreAudio device UID via kAudioDevicePropertyDeviceUID. A CoreAudio device carries both input and
// output streams under one AudioDeviceID, so we match by name only (isInput is ignored). The returned
// UID is what the aggregate's kAudioSubDeviceUIDKey / main-sub-device key actually require, and it is
// gain- and replug-stable. NOTE: inspection-only on the Windows dev host -- validate on a Mac (Gate 7);
// returns {} on any failure so callers fall back to the name with no regression.
juce::String endpointUidForName (const juce::String& deviceName, bool /*isInput*/) {
    AudioObjectPropertyAddress devs {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devs, 0, nullptr, &size) != noErr || size == 0)
        return {};
    std::vector<AudioDeviceID> ids (size / sizeof (AudioDeviceID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &devs, 0, nullptr, &size, ids.data()) != noErr)
        return {};

    for (auto id : ids) {
        CFStringRef cfName = nullptr; UInt32 ns = sizeof (cfName);
        AudioObjectPropertyAddress nameAddr {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData (id, &nameAddr, 0, nullptr, &ns, &cfName) != noErr || cfName == nullptr)
            continue;
        const juce::String devName = juce::String::fromCFString (cfName);
        CFRelease (cfName);
        if (devName != deviceName) continue;

        CFStringRef cfUid = nullptr; UInt32 us = sizeof (cfUid);
        AudioObjectPropertyAddress uidAddr {
            kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData (id, &uidAddr, 0, nullptr, &us, &cfUid) != noErr || cfUid == nullptr)
            continue;
        const juce::String uid = juce::String::fromCFString (cfUid);
        CFRelease (cfUid);
        return uid;
    }
    return {};
}

} // namespace eb

#endif // JUCE_MAC
