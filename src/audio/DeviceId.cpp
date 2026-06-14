#include "audio/DeviceId.h"
namespace eb {

juce::String DeviceId::key() const {
    return typeName + "|" + name + "|" + uid;
}

bool DeviceId::operator== (const DeviceId& other) const {
    return typeName == other.typeName
        && name == other.name
        && uid == other.uid
        && isVirtualSink == other.isVirtualSink
        && model == other.model;
}

} // namespace eb
