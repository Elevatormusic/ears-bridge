#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"
namespace eb {

struct DeviceId {
    juce::String typeName, name, uid;
    bool      isVirtualSink = false;
    EarsModel model = EarsModel::Unknown;

    bool operator== (const DeviceId& other) const;
    // typeName + "|" + name + "|" + uid. Stability is bounded by `uid`: replug-stable only
    // where rescan() obtained a real stable endpoint id; otherwise uid==name and key() is
    // name-stable only (a known limitation — CalBinder keys are name-stable in that case).
    juce::String key() const;
};

} // namespace eb
