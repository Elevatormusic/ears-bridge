#include "platform/OutputActivity.h"

#if JUCE_WINDOWS

#include <objbase.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>   // IAudioMeterInformation::GetPeakValue

namespace eb {

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0},14 (the string JUCE shows). Declared
// inline so this TU needs no INITGUID / propsys linkage just for one property key (matches EndpointFormat.cpp).
static const PROPERTYKEY kFriendlyNameKeyOA = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// Enumerate active RENDER endpoints and match `deviceName` by FriendlyName in TWO TIERS (#57, mirroring
// captureLoopback): an EXACT case-insensitive match anywhere in the enumeration wins; otherwise the first
// contains-match is the fallback. A bare contains() metered the FIRST endpoint whose name merely contained
// the stored string ("Speakers" matches both "Speakers (Realtek)" and "Speakers (USB DAC)", order-dependent).
// Activate IAudioMeterInformation and read GetPeakValue (0..1).
// Returns -1.0f on no match / any HRESULT failure — the "unknown" sentinel that must NOT read as silence.
float outputRenderPeakForName (const juce::String& deviceName) {
    if (deviceName.isEmpty()) return -1.0f;

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);
    float exactPeak = -1.0f, containsPeak = -1.0f;   // unknown until a successful read (0.0 = readable silence)

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED (en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            for (UINT i = 0; i < count && exactPeak < 0.0f; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    bool exact = false, contains = false;
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                        PROPVARIANT nm = {};
                        if (SUCCEEDED (store->GetValue (kFriendlyNameKeyOA, &nm))
                            && nm.vt == VT_LPWSTR && nm.pwszVal != nullptr) {
                            const juce::String friendly (nm.pwszVal);
                            exact    = friendly.equalsIgnoreCase (deviceName);
                            contains = exact || friendly.containsIgnoreCase (deviceName);
                        }
                        if (nm.vt == VT_LPWSTR && nm.pwszVal != nullptr) CoTaskMemFree (nm.pwszVal);
                        store->Release();
                    }
                    if (exact || (contains && containsPeak < 0.0f)) {
                        IAudioMeterInformation* meter = nullptr;
                        if (SUCCEEDED (dev->Activate (__uuidof (IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                                      (void**) &meter)) && meter != nullptr) {
                            float peak = 0.0f;
                            if (SUCCEEDED (meter->GetPeakValue (&peak)))
                                (exact ? exactPeak : containsPeak) = peak;
                            meter->Release();
                        }
                    }
                    dev->Release();
                }
            }
            coll->Release();
        }
        en->Release();
    }
    if (weInited) CoUninitialize();
    return exactPeak >= 0.0f ? exactPeak : containsPeak;
}

} // namespace eb

#else   // non-Windows (macOS / Linux): no PC-render side channel -> always "unknown". The toggle path still works.

namespace eb { float outputRenderPeakForName (const juce::String&) { return -1.0f; } }

#endif
