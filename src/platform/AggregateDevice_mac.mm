#include "platform/AggregateDevice_mac.h"

#if JUCE_MAC

 #include <CoreAudio/CoreAudio.h>
 #include <CoreAudio/AudioHardware.h>
 #include <CoreFoundation/CoreFoundation.h>

 // Apple renamed "Master"->"Main" for the clock-master sub-device key (SDK 12 / macOS Monterey).
 // On a pre-SDK-12 toolchain only the older "Master" spelling is declared; alias it so the
 // create() body below can always use the "Main" name. Both refer to the clock master.
 #ifndef kAudioAggregateDeviceMainSubDeviceKey
  #define kAudioAggregateDeviceMainSubDeviceKey kAudioAggregateDeviceMasterSubDeviceKey
 #endif

namespace eb {

// Look up an AudioDeviceID from its UID string (the JUCE device "uid").
static AudioDeviceID deviceIdForUid (const juce::String& uidStr, bool& ok) {
    ok = false;
    CFStringRef cfUid = CFStringCreateWithCString (kCFAllocatorDefault,
                                                   uidStr.toRawUTF8(), kCFStringEncodingUTF8);
    if (cfUid == nullptr) return kAudioObjectUnknown;

    AudioValueTranslation tr;
    AudioDeviceID dev = kAudioObjectUnknown;
    tr.mInputData = &cfUid;  tr.mInputDataSize  = sizeof (cfUid);
    tr.mOutputData = &dev;   tr.mOutputDataSize = sizeof (dev);

    AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyDeviceForUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof (tr);
    OSStatus st = AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &size, &tr);
    CFRelease (cfUid);
    ok = (st == noErr && dev != kAudioObjectUnknown);
    return dev;
}

static CFDictionaryRef makeSubDeviceDict (const juce::String& uid, bool driftCorrect) {
    CFStringRef cfUid = CFStringCreateWithCString (kCFAllocatorDefault,
                                                   uid.toRawUTF8(), kCFStringEncodingUTF8);
    int drift = driftCorrect ? 1 : 0;
    CFNumberRef cfDrift = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &drift);

    const void* keys[]   = { CFSTR (kAudioSubDeviceUIDKey), CFSTR (kAudioSubDeviceDriftCompensationKey) };
    const void* values[] = { cfUid, cfDrift };
    CFDictionaryRef dict = CFDictionaryCreate (kCFAllocatorDefault, keys, values, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease (cfUid); CFRelease (cfDrift);
    return dict;
}

bool AggregateDevice::create (const juce::String& earsInputUid,
                              const juce::String& virtualOutputUid,
                              juce::String& errorOut) {
    destroy();   // ensure no prior aggregate leaks

    // Build the sub-device list: EARS (clock master) first, then the virtual output.
    // The clock master must NOT be drift-corrected — it provides the reference clock; only the
    // FOLLOWER (the virtual output) gets resampled to track it. So: EARS=false, output=true.
    CFDictionaryRef earsSub = makeSubDeviceDict (earsInputUid, false);   // master: no drift correction
    CFDictionaryRef virtSub = makeSubDeviceDict (virtualOutputUid, true); // follower: drift-corrected
    const void* subDevices[] = { earsSub, virtSub };
    CFArrayRef subList = CFArrayCreate (kCFAllocatorDefault, subDevices, 2, &kCFTypeArrayCallBacks);

    // Aggregate description dictionary.
    juce::String aggUid = "eb.aggregate." + juce::String (juce::Time::currentTimeMillis());
    CFStringRef cfName = CFSTR ("EARS Bridge Aggregate");
    CFStringRef cfUid  = CFStringCreateWithCString (kCFAllocatorDefault,
                                                    aggUid.toRawUTF8(), kCFStringEncodingUTF8);
    CFStringRef cfMaster = CFStringCreateWithCString (kCFAllocatorDefault,
                                                      earsInputUid.toRawUTF8(), kCFStringEncodingUTF8);
    int isPrivate = 1;  // private: not shown in Audio MIDI Setup, torn down with the process
    CFNumberRef cfPrivate = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &isPrivate);

    const void* keys[] = {
        CFSTR (kAudioAggregateDeviceNameKey),
        CFSTR (kAudioAggregateDeviceUIDKey),
        CFSTR (kAudioAggregateDeviceIsPrivateKey),
        CFSTR (kAudioAggregateDeviceSubDeviceListKey),
        CFSTR (kAudioAggregateDeviceMainSubDeviceKey)   // clock master = EARS
    };
    const void* values[] = { cfName, cfUid, cfPrivate, subList, cfMaster };
    CFDictionaryRef desc = CFDictionaryCreate (kCFAllocatorDefault, keys, values, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);

    AudioDeviceID newId = kAudioObjectUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice (desc, &newId);

    CFRelease (desc); CFRelease (subList);
    CFRelease (earsSub); CFRelease (virtSub);
    CFRelease (cfUid); CFRelease (cfMaster); CFRelease (cfPrivate);

    if (st != noErr || newId == kAudioObjectUnknown) {
        errorOut = "AudioHardwareCreateAggregateDevice failed (OSStatus " + juce::String ((int) st) + ")";
        valid = false;
        return false;
    }

    deviceId = (unsigned) newId;
    uid = aggUid;
    valid = true;
    return true;
}

void AggregateDevice::destroy() {
    if (! valid) return;
    AudioHardwareDestroyAggregateDevice ((AudioDeviceID) deviceId);
    valid = false; deviceId = 0; uid = {};
}

} // namespace eb

#endif // JUCE_MAC
