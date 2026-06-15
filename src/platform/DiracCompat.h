#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Dirac Live records the measurement mic in WASAPI EXCLUSIVE mode by default. The standard
// VB-Audio Virtual Cable's capture endpoint does NOT support exclusive mode (its driver advertises
// no exclusive formats), so Dirac fails to open it with "Recording device error 600007". Dirac's
// documented opt-out is the user env var DAUDIO_WASAPI_NON_EXCLUSIVE=ON, which makes it record in
// shared mode (which the standard cable supports). The recording device is opened by DiracLive.exe
// (the measurement app, confirmed via audio-session enumeration), which the user launches
// themselves -- so once the variable is set, only Dirac Live needs to be reopened (no reboot).
//
// These helpers read/set that User environment variable. Windows-only; no-ops elsewhere (the
// shared/exclusive WASAPI distinction is a Windows concept).

// True if DAUDIO_WASAPI_NON_EXCLUSIVE is already set to "ON" for the current user.
bool diracSharedModeEnabled();

// Set DAUDIO_WASAPI_NON_EXCLUSIVE=ON (User scope) and broadcast the environment change so freshly
// launched apps pick it up. Returns true on success; `messageOut` carries a user-facing note.
bool enableDiracSharedMode (juce::String& messageOut);

} // namespace eb
