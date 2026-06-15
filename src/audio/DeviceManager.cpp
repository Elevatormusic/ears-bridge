#include "audio/DeviceManager.h"
#include "audio/ModelDetect.h"
#include <algorithm>
namespace eb {

DeviceManager::DeviceManager() {
    // getAvailableDeviceTypes() lazily creates and OWNS the internal type list on first call.
    // Listen on each type for OS hot-plug (device add/remove) so the cached lists stay current.
    // (NOTE: createAudioDeviceTypes(localList) would populate a CALLER list, not the manager's.)
    for (auto* t : adm.getAvailableDeviceTypes())
        t->addListener (this);
}

DeviceManager::~DeviceManager() {
    for (auto* t : adm.getAvailableDeviceTypes())
        t->removeListener (this);
}

void DeviceManager::audioDeviceListChanged() {
    // Called on the message thread by the driver type when devices are added/removed.
    rescan();
    if (onListChanged) onListChanged();
}

juce::String DeviceManager::preferredTypeName() {
   #if JUCE_WINDOWS
    return "Windows Audio";   // WASAPI; never the ASIO type for the cross-device split
   #elif JUCE_MAC
    return "CoreAudio";
   #else
    return "ALSA";
   #endif
}

bool DeviceManager::typeSupportsSeparateIO (const juce::AudioIODeviceType& type) {
    return type.hasSeparateInputsAndOutputs();
}

bool DeviceManager::looksLikeVirtualSink (const juce::String& name) {
    const auto n = name.toLowerCase();
    static const char* tokens[] = { "cable", "vb-audio", "voicemeeter", "blackhole",
                                    "loopback", "virtual", "soundflower" };
    for (auto* t : tokens) if (n.contains (t)) return true;
    return false;
}

juce::AudioIODeviceType* DeviceManager::findPreferredType() {
    // An explicit override (from setCurrentType(), e.g. the ASIO->WASAPI fallback) wins.
    const auto want = forcedTypeName.isNotEmpty() ? forcedTypeName : preferredTypeName();
    for (auto* t : adm.getAvailableDeviceTypes())
        if (t->getTypeName() == want) return t;
    // Fallback: first type that can pair separate in/out (skip ASIO).
    for (auto* t : adm.getAvailableDeviceTypes())
        if (t->hasSeparateInputsAndOutputs()) return t;
    return nullptr;
}

std::vector<DeviceManager::TypeCaps> DeviceManager::availableTypeCaps() {
    std::vector<TypeCaps> caps;
    for (auto* t : adm.getAvailableDeviceTypes())
        caps.push_back ({ t->getTypeName(), t->hasSeparateInputsAndOutputs() });
    return caps;
}

DeviceManager::TypeCaps DeviceManager::currentTypeCaps() {
    const auto want = forcedTypeName.isNotEmpty() ? forcedTypeName : preferredTypeName();
    for (auto* t : adm.getAvailableDeviceTypes())
        if (t->getTypeName() == want)
            return { t->getTypeName(), t->hasSeparateInputsAndOutputs() };
    return { want, false };   // not found: report as not-separate so the fallback path engages
}

void DeviceManager::setCurrentType (const juce::String& typeName) {
    forcedTypeName = typeName;   // honored by findPreferredType() on the next open
}

// Best-effort stable endpoint id for a device. JUCE's AudioIODeviceType exposes only
// display names (getDeviceNames); it does NOT surface a persistent endpoint GUID/UID
// through this API on Windows ("Windows Audio") or macOS ("CoreAudio"). So today this
// returns the name itself and uid==name. That is a KNOWN LIMITATION: key() is therefore
// name-stable, NOT replug-stable, and CalBinder keys are name-stable only. If a future
// build resolves a real endpoint id (e.g. via a WASAPI IMMDevice GetId() or the CoreAudio
// kAudioDevicePropertyDeviceUID side-channel), populate it here and the key() upgrades to
// replug-stable with no other change. Do NOT claim replug-stability while this returns name.
static juce::String stableUidFor (const juce::String& name) {
    return name;   // name-stable only; see the limitation above
}

void DeviceManager::rescan() {
    inputList.clear(); outputList.clear();
    auto* type = findPreferredType();
    if (type == nullptr) return;
    type->scanForDevices();
    const auto typeName = type->getTypeName();

    for (auto& name : type->getDeviceNames (true)) {   // true => input devices
        DeviceId d; d.typeName = typeName; d.name = name; d.uid = stableUidFor (name);
        d.model = detectEarsModel (name);
        inputList.push_back (d);
    }
    for (auto& name : type->getDeviceNames (false)) {  // false => output devices
        DeviceId d; d.typeName = typeName; d.name = name; d.uid = stableUidFor (name);
        d.isVirtualSink = looksLikeVirtualSink (name);
        outputList.push_back (d);
    }
}

std::vector<double> DeviceManager::queryDeviceRates (const DeviceId& id) {
    // Already-open matching device: ask it directly.
    if (inDev != nullptr && inDev->getName() == id.name) {
        auto a = inDev->getAvailableSampleRates();
        return std::vector<double> (a.begin(), a.end());
    }
    // Otherwise create a transient (un-opened) device just to read its supported rates. This is
    // what lets ANY input device report real rates, not only a recognised EARS model.
    auto* type = findPreferredType();
    if (type == nullptr) return {};
    type->scanForDevices();
    std::unique_ptr<juce::AudioIODevice> tmp (type->createDevice ({}, id.name));   // input-only
    if (tmp == nullptr) return {};
    auto a = tmp->getAvailableSampleRates();
    return std::vector<double> (a.begin(), a.end());
}

std::vector<double> DeviceManager::nativeRatesFor (const DeviceId& id) {
    const auto whitelist = nativeSampleRates (id.model);   // curated per-model set (may be empty)
    const auto real      = queryDeviceRates (id);          // what the device actually reports

    if (! real.empty()) {
        if (whitelist.empty())
            return real;   // unrecognised device: offer exactly the rates it supports
        std::vector<double> out;                            // recognised model: curated set,
        for (double r : whitelist)                          // narrowed to what the device truly supports
            if (std::any_of (real.begin(), real.end(),
                             [r](double a){ return std::abs (a - r) < 1.0; }))
                out.push_back (r);
        return out.empty() ? real : out;                    // disagreement -> trust the device
    }
    return whitelist;   // couldn't create/query (headless/test) = model whitelist (may be empty)
}

std::vector<int> DeviceManager::nativeBitDepthsFor (const DeviceId& id) const {
    return nativeBitDepths (id.model);
}

juce::String DeviceManager::openInput (const DeviceId& id, double sampleRate, int bufferSize) {
    auto* type = findPreferredType();
    if (type == nullptr) return "No suitable audio driver type (WASAPI/CoreAudio) found";
    type->scanForDevices();
    inDev.reset (type->createDevice ({}, id.name));   // input-only
    if (inDev == nullptr) return "Could not create input device: " + id.name;
    juce::BigInteger inCh; inCh.setRange (0, 2, true);     // 2 ears
    juce::BigInteger noOut;
    auto err = inDev->open (inCh, noOut, sampleRate, bufferSize);
    if (err.isNotEmpty()) { inDev.reset(); return "Open input failed: " + err; }
    return {};
}

juce::String DeviceManager::openOutput (const DeviceId& id, double sampleRate, int bufferSize,
                                        int requestedOutputBits) {
    // Record the requested depth (16/24/32). This is a BEST-EFFORT request: JUCE's
    // AudioIODevice::open() negotiates the render format itself and, in WASAPI/CoreAudio
    // shared mode, typically delivers float buffers — it does NOT accept an arbitrary
    // integer bit depth as an open parameter. We therefore (a) store what was asked for,
    // (b) open at the requested sample rate, and (c) let the device fall back to its
    // nearest supported format. After open, getCurrentBitDepth() reports what was actually
    // granted; callers must treat requestedOutputBitDepth() as a request, not a guarantee.
    requestedOutBits = (requestedOutputBits == 16 || requestedOutputBits == 24
                        || requestedOutputBits == 32) ? requestedOutputBits : 24;

    auto* type = findPreferredType();
    if (type == nullptr) return "No suitable audio driver type (WASAPI/CoreAudio) found";
    type->scanForDevices();
    outDev.reset (type->createDevice (id.name, {}));   // output-only render side
    if (outDev == nullptr) return "Could not create output device: " + id.name;
    juce::BigInteger noIn;
    juce::BigInteger outCh; outCh.setRange (0, 2, true);   // dual-mono L=R
    // open() negotiates the format; the device picks the nearest supported depth. There is
    // no JUCE API to force an integer render depth here, so the request is honored only to
    // the extent the driver's shared-mode format allows (verify with getCurrentBitDepth()).
    auto err = outDev->open (noIn, outCh, sampleRate, bufferSize);
    if (err.isNotEmpty()) { outDev.reset(); return "Open output failed: " + err; }
    return {};
}

void DeviceManager::closeAll() {
    if (inDev)  { inDev->stop();  inDev->close();  inDev.reset(); }
    if (outDev) { outDev->stop(); outDev->close(); outDev.reset(); }
}

} // namespace eb
