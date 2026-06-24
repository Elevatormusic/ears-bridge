#pragma once

namespace eb {

// System accessibility preferences read from the OS, cached behind atomics. Call refresh() periodically
// (the MainComponent theme tick does) and read the flags from paint()/layout. Windows is implemented;
// other platforms return the no-reduction defaults until a native impl (macOS NSWorkspace) is added, so
// the app simply behaves as before there - never a regression, just the feature staying inactive.
struct SystemA11y {
    static bool reduceMotion();        // OS "show animations" is OFF  -> snap meters/transitions instead of easing
    static bool increaseContrast();    // OS High Contrast / Increase Contrast is ON -> stronger separators + borders
    static bool reduceTransparency();  // OS transparency effects OFF -> opaque instead of translucent chip/overlay fills
    static bool refresh();             // re-read the OS settings (cheap; message thread). True if any flag changed.
};

} // namespace eb
