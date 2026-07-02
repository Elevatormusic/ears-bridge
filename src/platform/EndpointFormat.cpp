#include "platform/EndpointFormat.h"

namespace eb {

// PURE field interpreter — no platform types, compiles + unit-tests everywhere. WAVE_FORMAT_PCM (1),
// WAVE_FORMAT_IEEE_FLOAT (3), WAVE_FORMAT_EXTENSIBLE (0xFFFE) are the relevant tags; for EXTENSIBLE the
// SubFormat (passed in as extensibleSubFormatIsFloat) decides float-vs-int, since the tag alone is opaque.
EndpointFormat interpretMixFormat (unsigned formatTag, unsigned long rateHz, unsigned channels,
                                   unsigned bits, bool extensibleSubFormatIsFloat,
                                   bool exclusive48kSupported) {
    constexpr unsigned kFormatIeeeFloat  = 3;       // WAVE_FORMAT_IEEE_FLOAT
    constexpr unsigned kFormatExtensible = 0xFFFE;  // WAVE_FORMAT_EXTENSIBLE

    EndpointFormat f;
    f.valid     = true;
    f.mixRateHz = (double) rateHz;
    f.channels  = (int) channels;
    f.bits      = (int) bits;
    f.isFloat   = (formatTag == kFormatIeeeFloat)
               || (formatTag == kFormatExtensible && extensibleSubFormatIsFloat);
    f.exclusive48kSupported = exclusive48kSupported;
    return f;
}

} // namespace eb

#if JUCE_WINDOWS

#include "platform/EndpointUid.h"   // endpointUidForName: the stable UID we key the lookup on
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>            // IAudioClient::GetMixFormat / IsFormatSupported
#include <mmreg.h>                  // WAVEFORMATEX / WAVEFORMATEXTENSIBLE
#include <ksmedia.h>                // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, SPEAKER_* masks

namespace eb {

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0},14 (the string JUCE shows). Declared
// inline so this TU needs no INITGUID / propsys linkage just for one property key (matches EndpointUid.cpp).
static const PROPERTYKEY kFriendlyNameKey = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// Build a WAVEFORMATEXTENSIBLE for the IsFormatSupported(EXCLUSIVE) probe (same shape as eb_diag's makeWfx).
static void makeWfx (WAVEFORMATEXTENSIBLE& w, int rate, int ch, int bits, bool isFloat) {
    ZeroMemory (&w, sizeof (w));
    w.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels       = (WORD) ch;
    w.Format.nSamplesPerSec  = (DWORD) rate;
    w.Format.wBitsPerSample  = (WORD) bits;
    w.Format.nBlockAlign     = (WORD) (ch * bits / 8);
    w.Format.nAvgBytesPerSec = (DWORD) (rate * w.Format.nBlockAlign);
    w.Format.cbSize          = 22;
    w.Samples.wValidBitsPerSample = (WORD) bits;
    w.dwChannelMask = (ch == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    w.SubFormat     = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

// Read the active endpoint matching `nameOrUid` (UID-keyed via IMMDevice::GetId, else FriendlyName
// contains-match), activate an IAudioClient, GetMixFormat for the shared-mode ground truth, and probe
// EXCLUSIVE support at 48k in that same format. Returns {valid=false} on any miss/failure.
EndpointFormat readEndpointFormat (const juce::String& nameOrUid, bool isInput) {
    // #29/#33: ONE enumeration. The old pre-resolve (endpointUidForName) ran a complete SECOND enumeration
    // with a property-store read per device on every call - at pollChainConfig's 1 Hz x 3 endpoints, a
    // steady message-thread COM storm for the app's lifetime. The hint is matched IN-LOOP instead: as a
    // UID (IMMDevice::GetId - covers callers passing the stable id), else FriendlyName contains (the
    // original fallback tier, same first-match semantics).
    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInited = SUCCEEDED (co);
    EndpointFormat result;   // valid=false until a successful read

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator), (void**) &en)) && en != nullptr) {
        IMMDeviceCollection* coll = nullptr;
        const EDataFlow flow = isInput ? eCapture : eRender;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) && coll != nullptr) {
            UINT count = 0; coll->GetCount (&count);
            for (UINT i = 0; i < count && ! result.valid; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED (coll->Item (i, &dev)) && dev != nullptr) {
                    // Match this endpoint: the caller's hint as a stable UID (IMMDevice::GetId); else fall
                    // back to a case-insensitive FriendlyName contains-match on the raw hint.
                    bool match = false;
                    LPWSTR id = nullptr;
                    if (SUCCEEDED (dev->GetId (&id)) && id != nullptr) {
                        if (nameOrUid == juce::String (id)) match = true;
                        CoTaskMemFree (id);
                    }
                    if (! match) {
                        IPropertyStore* store = nullptr;
                        if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &store)) && store != nullptr) {
                            PROPVARIANT nm = {};
                            if (SUCCEEDED (store->GetValue (kFriendlyNameKey, &nm))
                                && nm.vt == VT_LPWSTR && nm.pwszVal != nullptr
                                && juce::String (nm.pwszVal).containsIgnoreCase (nameOrUid))
                                match = true;
                            if (nm.vt == VT_LPWSTR && nm.pwszVal != nullptr) CoTaskMemFree (nm.pwszVal);
                            store->Release();
                        }
                    }

                    if (match) {
                        IAudioClient* ac = nullptr;
                        if (SUCCEEDED (dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                                      (void**) &ac)) && ac != nullptr) {
                            WAVEFORMATEX* mix = nullptr;
                            if (SUCCEEDED (ac->GetMixFormat (&mix)) && mix != nullptr) {
                                bool extFloat = false;
                                if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
                                    auto* wx = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (mix);
                                    extFloat = (wx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                                }
                                // #29: the EXCLUSIVE probe is skipped - nothing downstream consumes
                                // exclusive48kSupported (checkChainConfig never reads it), yet the
                                // IsFormatSupported call ran per endpoint per poll second. The field stays
                                // in the struct (tests pin its pass-through); a future consumer re-enables
                                // the probe here.
                                const bool excl48k = false;
                                result = interpretMixFormat (mix->wFormatTag, mix->nSamplesPerSec,
                                                             mix->nChannels, mix->wBitsPerSample,
                                                             extFloat, excl48k);
                                CoTaskMemFree (mix);
                            }
                            ac->Release();
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
    return result;
}

double endpointMixSampleRateForName (const juce::String& deviceName, bool isInput) {
    // Delegate to the full read so there is one Windows mix-format code path. readEndpointFormat keys on
    // the stable UID (then FriendlyName contains) — strictly more robust than the old exact-name match,
    // and it returns 0.0/{valid=false} on any miss, preserving the documented "unknown => 0.0" contract.
    const EndpointFormat f = readEndpointFormat (deviceName, isInput);
    return f.valid ? f.mixRateHz : 0.0;
}

} // namespace eb

#elif ! JUCE_MAC   // Linux / other: no mix-rate side channel (macOS impl in EndpointFormat_mac.mm)

namespace eb {
double endpointMixSampleRateForName (const juce::String&, bool) { return 0.0; }
EndpointFormat readEndpointFormat (const juce::String&, bool) { return {}; }   // valid=false
}

#endif
