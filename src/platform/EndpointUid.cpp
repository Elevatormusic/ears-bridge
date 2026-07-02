#include "platform/EndpointUid.h"

#if JUCE_WINDOWS

#include <objbase.h>
#include <mmdeviceapi.h>

namespace eb {

// #63/#28: the batch form - ONE enumeration returns every (JUCE display name -> endpoint id) pair for
// the flow. The names are keyed exactly as JUCE displays them (jucifyEndpointNames: default endpoint
// first, then appendNumbersToDuplicates), so duplicate friendly names resolve POSITIONALLY to the same
// endpoints JUCE labels "Name" / "Name (2)" - the old first-exact-match keying attributed the wrong
// endpoint's UID for every duplicate set. (Assumption, shared with JUCE: EnumAudioEndpoints order is
// stable for an unchanged device set, which MMDevice provides.)
juce::StringPairArray endpointUidsForFlow (bool isInput) {
    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14. Defined inline so this TU
    // doesn't need INITGUID / propsys linkage just for one property key.
    const PROPERTYKEY kFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);   // S_OK/S_FALSE added a ref to balance; RPC_E_CHANGED_MODE did not
    juce::StringPairArray result;

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        const EDataFlow flow = isInput ? eCapture : eRender;

        // The flow's DEFAULT endpoint id (eMultimedia - the SAME role JUCE's device list keys on).
        juce::String defaultId;
        {
            IMMDevice* def = nullptr;
            if (SUCCEEDED (en->GetDefaultAudioEndpoint (flow, eMultimedia, &def)) && def != nullptr) {
                LPWSTR id = nullptr;
                if (SUCCEEDED (def->GetId (&id)) && id != nullptr) {
                    defaultId = juce::String (id);
                    CoTaskMemFree (id);
                }
                def->Release();
            }
        }

        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            juce::StringArray names, ids;   // index-parallel, enumeration order
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    juce::String name, id;
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                        PROPVARIANT v = {};   // VT_EMPTY
                        if (SUCCEEDED (store->GetValue (kFriendlyName, &v))
                            && v.vt == VT_LPWSTR && v.pwszVal != nullptr)
                            name = juce::String (v.pwszVal);
                        if (v.vt == VT_LPWSTR && v.pwszVal != nullptr) CoTaskMemFree (v.pwszVal);
                        store->Release();
                    }
                    LPWSTR rawId = nullptr;
                    if (SUCCEEDED (dev->GetId (&rawId)) && rawId != nullptr) {
                        id = juce::String (rawId);
                        CoTaskMemFree (rawId);
                    }
                    dev->Release();
                    if (name.isNotEmpty() && id.isNotEmpty()) { names.add (name); ids.add (id); }
                }
            }
            coll->Release();

            // Mirror JUCE's ordering (#28): move the default endpoint to the front of BOTH parallel
            // lists, then apply JUCE's duplicate numbering to the names.
            const int defIdx = defaultId.isNotEmpty() ? ids.indexOf (defaultId) : -1;
            if (defIdx > 0) {
                const juce::String id = ids[defIdx];
                ids.remove (defIdx); ids.insert (0, id);
            }
            const juce::StringArray display = jucifyEndpointNames (names, defIdx);
            for (int i = 0; i < display.size() && i < ids.size(); ++i)
                result.set (display[i], ids[i]);
        }
        en->Release();
    }
    if (weInited) CoUninitialize();
    return result;
}

// WASAPI endpoint id via IMMDevice::GetId, keyed by the JUCE display name. #28: delegates to the batch
// map so the per-name path shares the positional duplicate resolution ("Name (2)" finds the SECOND
// endpoint, and bare "Name" can no longer alias onto the wrong one of a duplicate pair).
juce::String endpointUidForName (const juce::String& deviceName, bool isInput) {
    return endpointUidsForFlow (isInput).getValue (deviceName, {});
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
