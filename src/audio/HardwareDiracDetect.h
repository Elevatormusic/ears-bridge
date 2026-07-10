#pragma once
#include <juce_core/juce_core.h>   // juce::String (for the honest-copy helpers; the decisions are juce-free)
#include "gui/Copy.h"   // P4 T6: typography constants (juce_core only)

// Hardware-Dirac detection (pure). When Dirac Live runs on a hardware miniDSP processor (DDRC-24 / SHD /
// Flex) instead of Dirac Live SOFTWARE on the PC, the per-ear measurement still works (it is entirely
// mic-side), but the reference-monitor GRADE cannot: the box generates its sweep INTERNALLY, so the WASAPI
// loopback captures no digital reference. We detect that case BEHAVIORALLY (no device names / VIDs — the
// EARS jig is itself a miniDSP USB device, so a VID list would not even work) and degrade the grade to a
// calm "grading off" state instead of a never-green grade.
namespace eb {

// A real PC render stream is present iff the metered render peak clears a small floor. The -1.0f "unknown"
// sentinel (an unreadable output endpoint) is NOT a render — it is the ABSENCE of information, so it fails.
[[nodiscard]] inline bool outputRendered (float renderPeak, float floor = 0.0015f) noexcept {
    return renderPeak > floor;   // -1 and 0 both fail; the floor rejects idle dither
}

// The hardware-Dirac signature: the EARS mic clearly heard a sweep, BUT Dirac's PC output endpoint was
// READABLE and showed NO render, in a valid Windows-Audio (shared) mode. Readability is MANDATORY: an
// unreadable output cannot confirm silence (it might just be unreadable), so it must never auto-suggest -
// a false positive would silently kill a software-Dirac user's grade. The toggle is the deterministic
// override; this only ever SUGGESTS.
[[nodiscard]] inline bool sweepWasInternal (bool micHeardSweep, bool outputRenderedReadable,
                                            bool outputDidRender, bool validMode) noexcept {
    return micHeardSweep && outputRenderedReadable && ! outputDidRender && validMode;
}

// The honest suggestion (shown when auto-detect fires + the toggle isn't set) and the calm grading-off line.
[[nodiscard]] inline juce::String hardwareDiracSuggestion() {
    return "Looks like Dirac is running on a hardware processor" + kDash + "its sweep reached the mic but never the PC "
           "output. Turn on \"Dirac runs on a hardware processor\" in Advanced to silence the reference grade.";
}
[[nodiscard]] inline juce::String hardwareDiracGradingOff() {
    return "Reference grading off" + kDash + "Dirac runs on a hardware processor. Your per-ear calibration is still active.";
}

} // namespace eb
