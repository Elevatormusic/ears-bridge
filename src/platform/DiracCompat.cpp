#include "platform/DiracCompat.h"

#if JUCE_WINDOWS
 #include <windows.h>

namespace eb {

static const char* kKey = "HKEY_CURRENT_USER\\Environment\\DAUDIO_WASAPI_NON_EXCLUSIVE";

bool diracSharedModeEnabled() {
    return juce::WindowsRegistry::getValue (kKey).trim().equalsIgnoreCase ("ON");
}

bool enableDiracSharedMode (juce::String& messageOut) {
    if (! juce::WindowsRegistry::setValue (kKey, juce::String ("ON"))) {
        messageOut = "Couldn't write the setting (no permission?).";
        return false;
    }
    // Tell already-running apps (notably Explorer, which spawns Start-menu launches) that the
    // environment changed, so the NEXT Dirac Live launch inherits the variable -- no reboot needed.
    // #56: broadcast SYNCHRONOUSLY with a short per-window timeout. The old detached-thread broadcast was
    // fire-and-forget: quitting inside its window killed the thread before Explorer processed the message,
    // so the env change silently failed to propagate while the UI claimed success. ~100 ms per hung window
    // is fine for a click handler, and returning AFTER the broadcast makes the success message honest.
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW (HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) L"Environment",
                           SMTO_ABORTIFHUNG, 100, &result);
    messageOut = "Now fully close and reopen Dirac Live (no reboot needed).";
    return true;
}

} // namespace eb

#else  // ---- non-Windows: shared/exclusive WASAPI doesn't apply ----

namespace eb {
bool diracSharedModeEnabled() { return false; }
bool enableDiracSharedMode (juce::String& messageOut) { messageOut = {}; return false; }
}

#endif
