#include "platform/EndpointFormat.h"

#if JUCE_WINDOWS

#include <objbase.h>
#include <mmdeviceapi.h>
#include <mmreg.h>          // WAVEFORMATEX

namespace eb {

double endpointMixSampleRateForName (const juce::String& deviceName, bool isInput) {
    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0},14 (same string JUCE shows).
    const PROPERTYKEY kFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    // PKEY_AudioEngine_DeviceFormat = {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0 -> the device's shared
    // mix format (a WAVEFORMATEX blob). Its nSamplesPerSec is the rate the OS mixer runs the endpoint at.
    const PROPERTYKEY kDeviceFormat = {
        { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);
    double result = 0.0;

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        IMMDeviceCollection* coll = nullptr;
        const EDataFlow flow = isInput ? eCapture : eRender;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            for (UINT i = 0; i < count && result == 0.0; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                        PROPVARIANT name = {};
                        if (SUCCEEDED (store->GetValue (kFriendlyName, &name))
                            && name.vt == VT_LPWSTR && name.pwszVal != nullptr
                            && deviceName == juce::String (name.pwszVal)) {
                            PROPVARIANT fmt = {};
                            if (SUCCEEDED (store->GetValue (kDeviceFormat, &fmt))
                                && fmt.vt == VT_BLOB && fmt.blob.pBlobData != nullptr
                                && fmt.blob.cbSize >= sizeof (WAVEFORMATEX)) {
                                auto* wfx = reinterpret_cast<const WAVEFORMATEX*> (fmt.blob.pBlobData);
                                result = (double) wfx->nSamplesPerSec;
                            }
                            PropVariantClear (&fmt);
                        }
                        if (name.vt == VT_LPWSTR && name.pwszVal != nullptr) CoTaskMemFree (name.pwszVal);
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

#elif ! JUCE_MAC   // Linux / other: no mix-rate side channel (macOS impl in EndpointFormat_mac.mm)

namespace eb { double endpointMixSampleRateForName (const juce::String&, bool) { return 0.0; } }

#endif
