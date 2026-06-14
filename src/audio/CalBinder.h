#pragma once
#include <juce_core/juce_core.h>
#include "audio/DeviceId.h"
#include <vector>
#include <optional>

namespace eb {

// Pure logic: associates per-ear cal file paths with a device by DeviceId::key(),
// surviving re-enumeration. No file I/O, no JUCE devices.
class CalBinder {
public:
    struct Binding {
        juce::String deviceKey;     // DeviceId::key()
        EarsModel    model = EarsModel::Unknown;
        juce::String leftCalPath;   // absolute path of the L cal .txt (may be empty)
        juce::String rightCalPath;  // absolute path of the R cal .txt (may be empty)
    };

    // Remember/overwrite the cal association for a device.
    void bind (const DeviceId& dev, juce::String leftCalPath, juce::String rightCalPath);

    // After re-enumeration, find the stored binding for a freshly enumerated device.
    // Returns nullopt if no binding exists, or if a binding exists but the MODEL changed
    // (stale association must not be reused with a different device class).
    std::optional<Binding> rebind (const DeviceId& reEnumerated) const;

    // True if a binding exists for this key AND the model still matches.
    bool hasValidBinding (const DeviceId& reEnumerated) const;

    void forget (const DeviceId& dev);
    void clear() { bindings.clear(); }
    int  size() const { return static_cast<int> (bindings.size()); }

private:
    std::vector<Binding> bindings;
    const Binding* find (const juce::String& key) const;
};

} // namespace eb
