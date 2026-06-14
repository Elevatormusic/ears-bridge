#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "audio/DeviceId.h"
#include <memory>
#include <vector>
namespace eb {

// Enumerates and opens the two device contexts. The render device is the master clock;
// the capture device feeds the ClockBridge. Uses WASAPI ("Windows Audio") on Windows and
// CoreAudio on macOS — never ASIO (cannot pair separate in/out devices).
class DeviceManager {
public:
    DeviceManager();

    // Preferred cross-device driver type name for the current OS.
    static juce::String preferredTypeName();   // "Windows Audio" / "CoreAudio"

    // Returns true if the named device type can pair a capture device with a different
    // render device (i.e. NOT ASIO). Pure check on the type object.
    static bool typeSupportsSeparateIO (const juce::AudioIODeviceType& type);

    // ---- ASIO-fallback seam (consumed by Plan 4's AsioFallback path) ----
    // Capability view of one driver type: its name and whether it can pair separate in/out
    // devices. Mirrors Plan 4's eb::DeviceTypeCaps shape without depending on that header.
    struct TypeCaps { juce::String typeName; bool separateInputsAndOutputs = true; };
    // All driver types JUCE enumerated (name + separate-I/O capability). Lets the engine build
    // the AsioFallback decision without re-walking AudioDeviceManager internals. Non-const
    // because JUCE's AudioDeviceManager::getAvailableDeviceTypes() is itself non-const.
    std::vector<TypeCaps> availableTypeCaps();
    // Caps for the type the engine would currently use (preferredTypeName(), or the override).
    TypeCaps currentTypeCaps();
    // Force the active driver type by name for the open path (used when AsioFallback decides to
    // fall back off ASIO to "Windows Audio"/"CoreAudio"). Persists until the next call/reset.
    void setCurrentType (const juce::String& typeName);

    // Heuristic: is this output device name a known/likely virtual sink?
    static bool looksLikeVirtualSink (const juce::String& name);

    void rescan();                                   // populate the cached lists
    std::vector<DeviceId> inputs()  const { return inputList; }
    std::vector<DeviceId> outputs() const { return outputList; }

    // Native rates/bit-depths for an input: model whitelist (ModelDetect) intersected with
    // the device's actually-supported rates when the device can be queried; falls back to
    // the pure whitelist when no live device is available (headless/test).
    std::vector<double> nativeRatesFor   (const DeviceId&) const;
    std::vector<int>    nativeBitDepthsFor (const DeviceId&) const;

    // Open the input (capture) and output (render) devices on the preferred type.
    // Returns empty string on success, else an error message. Channels: input opens
    // 2 channels (L/R ears); output opens 2 channels (dual-mono). Does NOT start callbacks.
    // requestedOutputBits (16/24/32) is requested best-effort on the render format; JUCE
    // shared-mode float may not honor an arbitrary integer depth, so the open falls back to
    // the nearest supported format. requestedOutputBitDepth() returns what was asked for.
    juce::String openInput  (const DeviceId&, double sampleRate, int bufferSize);
    juce::String openOutput (const DeviceId&, double sampleRate, int bufferSize,
                             int requestedOutputBits = 24);
    int requestedOutputBitDepth() const { return requestedOutBits; }

    juce::AudioIODevice* inputDevice()  const { return inDev.get(); }
    juce::AudioIODevice* outputDevice() const { return outDev.get(); }

    void closeAll();

private:
    juce::AudioDeviceManager adm;     // owns the type list / scanning
    std::vector<DeviceId> inputList, outputList;
    std::unique_ptr<juce::AudioIODevice> inDev, outDev;
    int requestedOutBits = 24;        // last bit depth requested on openOutput (best-effort)
    juce::String forcedTypeName;      // set by setCurrentType() to override the preferred type

    juce::AudioIODeviceType* findPreferredType();    // the "Windows Audio"/"CoreAudio" type
};

} // namespace eb
