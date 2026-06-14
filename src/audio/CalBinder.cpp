#include "audio/CalBinder.h"
#include <algorithm>
#include <utility>

namespace eb {

const CalBinder::Binding* CalBinder::find (const juce::String& key) const {
    for (auto& bnd : bindings)
        if (bnd.deviceKey == key) return &bnd;
    return nullptr;
}

void CalBinder::bind (const DeviceId& dev, juce::String leftCalPath, juce::String rightCalPath) {
    const auto key = dev.key();
    for (auto& bnd : bindings) {
        if (bnd.deviceKey == key) {
            bnd.model = dev.model;
            bnd.leftCalPath = std::move (leftCalPath);
            bnd.rightCalPath = std::move (rightCalPath);
            return;
        }
    }
    bindings.push_back (Binding { key, dev.model, std::move (leftCalPath), std::move (rightCalPath) });
}

std::optional<CalBinder::Binding> CalBinder::rebind (const DeviceId& reEnumerated) const {
    const auto* bnd = find (reEnumerated.key());
    if (bnd == nullptr) return std::nullopt;
    // Guard against a key collision where the device class changed (EARS <-> EARS Pro):
    // the FIR is designed at the model's native rate, so a stale model must not be reused.
    if (bnd->model != reEnumerated.model) return std::nullopt;
    return *bnd;
}

bool CalBinder::hasValidBinding (const DeviceId& reEnumerated) const {
    return rebind (reEnumerated).has_value();
}

void CalBinder::forget (const DeviceId& dev) {
    const auto key = dev.key();
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(),
                        [&] (const Binding& b) { return b.deviceKey == key; }),
                    bindings.end());
}

} // namespace eb
