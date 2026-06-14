#include "platform/AggregateDevice_mac.h"

// Portable translation unit, compiled on ALL platforms (Plan 4 Task 7 link split).
//
//  - On Windows (and any non-macOS target) the CoreAudio .mm is excluded from the build, so the
//    create()/destroy() stubs below provide the only definitions — without them eb_tests /
//    EarsBridge / eb_diag / diag_clockbridge would fail to link with unresolved AggregateDevice
//    externals.
//  - On macOS the real create()/destroy() come from AggregateDevice_mac.mm (#if JUCE_MAC); only the
//    destructor (always defined here, on every platform) is provided by this TU.
namespace eb {

#if ! JUCE_MAC

bool AggregateDevice::create (const juce::String&, const juce::String&, juce::String& errorOut) {
    errorOut = "Aggregate device is macOS-only";
    return false;
}

void AggregateDevice::destroy() { valid = false; }

#endif // ! JUCE_MAC

// Defined unconditionally on every platform: on macOS the body calls the .mm's destroy(); on
// Windows it calls the non-mac destroy() stub above.
AggregateDevice::~AggregateDevice() { destroy(); }

} // namespace eb
