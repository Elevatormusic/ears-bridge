#include "gui/SystemA11y.h"
#include <atomic>

// Reduce Motion / Increase Contrast / Reduce Transparency, read from the OS (HIG accessibility audit).
// Same portable split as EndpointUid/EndpointFormat: the Windows path here, a safe no-reduction stub elsewhere.

namespace eb {
namespace {
    std::atomic<bool> g_reduceMotion       { false };
    std::atomic<bool> g_increaseContrast   { false };
    std::atomic<bool> g_reduceTransparency { false };
}

bool SystemA11y::reduceMotion()       { return g_reduceMotion.load (std::memory_order_relaxed); }
bool SystemA11y::increaseContrast()   { return g_increaseContrast.load (std::memory_order_relaxed); }
bool SystemA11y::reduceTransparency() { return g_reduceTransparency.load (std::memory_order_relaxed); }
} // namespace eb

#if defined (_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace eb {
bool SystemA11y::refresh() {
    bool changed = false;
    // Reduce Motion: SPI_GETCLIENTAREAANIMATION returns FALSE when the user has turned animations OFF.
    BOOL anim = TRUE;
    if (SystemParametersInfoW (SPI_GETCLIENTAREAANIMATION, 0, &anim, 0)) {
        const bool v = (anim == FALSE);
        changed |= (g_reduceMotion.exchange (v, std::memory_order_relaxed) != v);
    }
    // Increase Contrast: the Windows High Contrast theme.
    HIGHCONTRASTW hc; hc.cbSize = sizeof (hc); hc.dwFlags = 0; hc.lpszDefaultScheme = nullptr;
    if (SystemParametersInfoW (SPI_GETHIGHCONTRAST, sizeof (hc), &hc, 0)) {
        const bool v = ((hc.dwFlags & HCF_HIGHCONTRASTON) != 0);
        changed |= (g_increaseContrast.exchange (v, std::memory_order_relaxed) != v);
    }
    // Reduce Transparency: Personalize\EnableTransparency == 0 (no dedicated SPI on Windows).
    DWORD val = 1, sz = sizeof (val);
    if (RegGetValueW (HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &val, &sz) == ERROR_SUCCESS) {
        const bool v = (val == 0);
        changed |= (g_reduceTransparency.exchange (v, std::memory_order_relaxed) != v);
    }
    return changed;
}
} // namespace eb

#else  // non-Windows: keep the no-reduction defaults (TODO: macOS NSWorkspace accessibilityDisplayShould* in a _mac.mm).

namespace eb {
bool SystemA11y::refresh() { return false; }
} // namespace eb

#endif
