#include "platform/EndpointUid.h"

#if JUCE_WINDOWS

#include <objbase.h>
#include <mmdeviceapi.h>

namespace eb {

// WASAPI endpoint id via IMMDevice::GetId. Match on PKEY_Device_FriendlyName (the same string JUCE
// surfaces as the device name), then return the endpoint id, which is stable across replug/rename and
// independent of the EARS gain token embedded in the friendly name.
juce::String endpointUidForName (const juce::String& deviceName, bool isInput) {
    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14. Defined inline so this TU
    // doesn't need INITGUID / propsys linkage just for one property key.
    const PROPERTYKEY kFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);   // S_OK/S_FALSE added a ref to balance; RPC_E_CHANGED_MODE did not
    juce::String result;

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        IMMDeviceCollection* coll = nullptr;
        const EDataFlow flow = isInput ? eCapture : eRender;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            for (UINT i = 0; i < count && result.isEmpty(); ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                        PROPVARIANT v = {};   // VT_EMPTY
                        if (SUCCEEDED (store->GetValue (kFriendlyName, &v))
                            && v.vt == VT_LPWSTR && v.pwszVal != nullptr
                            && deviceName == juce::String (v.pwszVal)) {
                            LPWSTR id = nullptr;
                            if (SUCCEEDED (dev->GetId (&id)) && id != nullptr) {
                                result = juce::String (id);
                                CoTaskMemFree (id);
                            }
                        }
                        if (v.vt == VT_LPWSTR && v.pwszVal != nullptr) CoTaskMemFree (v.pwszVal);
                        store->Release();
                    }
                    dev->Release();
                }
            }
            coll->Release();
        }
        en->Release();
    }
    if (weInited) CoUninitialize();
    return result;
}

// #63: the batch form - ONE enumeration returns every (FriendlyName -> endpoint id) pair for the flow.
juce::StringPairArray endpointUidsForFlow (bool isInput) {
    const PROPERTYKEY kFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);
    juce::StringPairArray result;

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        IMMDeviceCollection* coll = nullptr;
        const EDataFlow flow = isInput ? eCapture : eRender;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                        PROPVARIANT v = {};
                        if (SUCCEEDED (store->GetValue (kFriendlyName, &v))
                            && v.vt == VT_LPWSTR && v.pwszVal != nullptr) {
                            LPWSTR id = nullptr;
                            if (SUCCEEDED (dev->GetId (&id)) && id != nullptr) {
                                // first-wins on duplicate friendly names (matches the per-name resolver's
                                // first-exact-match semantics; positional resolution is audit #28 / Phase C)
                                if (! result.containsKey (juce::String (v.pwszVal)))
                                    result.set (juce::String (v.pwszVal), juce::String (id));
                                CoTaskMemFree (id);
                            }
                        }
                        if (v.vt == VT_LPWSTR && v.pwszVal != nullptr) CoTaskMemFree (v.pwszVal);
                        store->Release();
                    }
                    dev->Release();
                }
            }
            coll->Release();
        }
        en->Release();
    }
    if (weInited) CoUninitialize();
    return result;
}

} // namespace eb

#elif ! JUCE_MAC   // Linux / other: no endpoint-id side channel wired (macOS impl lives in EndpointUid_mac.mm)

namespace eb { juce::String endpointUidForName (const juce::String&, bool) { return {}; } }

#endif

#if ! JUCE_WINDOWS
// #63: no batch enumeration off-Windows (macOS resolves per name in EndpointUid_mac.mm; Linux has no side
// channel). Empty -> DeviceManager::rescan falls back to the per-name resolver, exactly the old behaviour.
namespace eb { juce::StringPairArray endpointUidsForFlow (bool) { return {}; } }
#endif
