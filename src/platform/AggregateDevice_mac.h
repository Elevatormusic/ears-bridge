#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Owns a CoreAudio aggregate device wrapping the EARS input + a virtual output, with drift
// correction on the FOLLOWER and the EARS as clock master. macOS only; on other platforms
// create() returns false and the engine falls back to the ClockBridge FIFO+ASRC path.
//
// Portability split (Plan 4 Task 7):
//   - This header is plain C++ (no Objective-C); it is includeable on ALL platforms.
//   - The real CoreAudio create()/destroy() bodies live in AggregateDevice_mac.mm (#if JUCE_MAC,
//     compiled only on APPLE).
//   - The non-macOS create()/destroy() stubs AND the destructor (on every platform) live in the
//     portable AggregateDevice.cpp, which is compiled on ALL platforms. This guarantees the
//     symbols resolve at link time on Windows (where the .mm is excluded from the build).
class AggregateDevice {
public:
    AggregateDevice() = default;
    ~AggregateDevice();                            // tears down if still alive (AggregateDevice.cpp)

    AggregateDevice (const AggregateDevice&) = delete;
    AggregateDevice& operator= (const AggregateDevice&) = delete;

    // Create an aggregate from the two sub-device CoreAudio UIDs (as reported by the OS / JUCE).
    // earsInputUid is the clock master. Returns false on any platform != macOS or on failure;
    // errorOut carries the reason. On success, aggregateUid() is the UID to open with JUCE.
    bool create (const juce::String& earsInputUid,
                 const juce::String& virtualOutputUid,
                 juce::String& errorOut);

    void destroy();                                // idempotent

    bool         isValid() const noexcept { return valid; }
    juce::String aggregateUid() const     { return uid; }

    // #10: the DISPLAY name registered in the creation dictionary (kAudioAggregateDeviceNameKey in
    // AggregateDevice_mac.mm — the CFSTR literal there MUST match this string). JUCE's CoreAudio device
    // list resolves createDevice() by DISPLAY NAME, not UID, so THIS is the open key for DeviceManager —
    // opening by aggregateUid() returned nullptr on every mac Start, and the aggregate path silently
    // never engaged (Gate-7 would have validated the two-clock fallback while believing it tested the
    // aggregate). Static so the engine can name it without a live instance.
    static juce::String aggregateName()   { return "EARS Bridge Aggregate"; }

private:
    bool         valid = false;
    juce::String uid;          // generated aggregate UID (the CoreAudio identity; NOT JUCE's open key)
    unsigned     deviceId = 0; // AudioDeviceID as unsigned (avoid CoreAudio types in the header)
};

} // namespace eb
