#pragma once
#include <juce_core/juce_core.h>
#include "gui/Copy.h"   // P4 T6: typography constants (juce_core only)
#include "audio/EngineTypes.h"
#include "gui/RawRailStatus.h"

namespace eb {

// Result of assembling the post-Start notes shown in the left rail. Split by tone so the GUI can
// render each correctly:
//   - warnings: genuinely cautionary lines (a real OS resample, an unverifiable endpoint rate).
//               These keep the yellow Theme::warn() colour.
//   - info:     a single calm, COMPLETE fact line (the 32-bit-float / shared-mode note). WASAPI
//               shared mode is always 32-bit float, so a granted-vs-selected bit-depth difference is
//               NORMAL, not a problem. Rendered neutral (Theme::textDim()), never as a warning, and
//               short enough to fit one rail line (no trailing clause that would clip).
struct StartNotes {
    juce::StringArray warnings;
    juce::String      info;
};

// Pure, header-only (mirrors gui/RawRailStatus.h / gui/ClipStatus.h) so it unit-tests without a
// window. Builds the post-Start notes from the granted-vs-selected device format + the raw-rail state.
//   grantedRate / selRate : the rate WASAPI granted vs the user's selection (a real mismatch = resample).
//   grantedBits / selBits : the granted output bit-depth vs the stored preference (a difference = normal).
//   rail                  : the D2 raw-rail verification state (unverified => a cautionary caveat).
[[nodiscard]] inline StartNotes buildStartNotes (double grantedRate, double selRate,
                                                 int grantedBits, int selBits,
                                                 const RawRailState& rail) {
    StartNotes n;

    // A genuine OS resample: the granted rate differs from what the user picked -> cautionary.
    if (selRate > 0.0 && grantedRate > 0.0 && std::abs (grantedRate - selRate) > 0.5)
        n.warnings.add ("Running at " + juce::String (grantedRate / 1000.0, 1) + " kHz, not the selected "
                        + juce::String (selRate / 1000.0, 1) + " kHz (resampled).");

    // The bit-depth fact: shared mode is always 32-bit float, so a difference from the stored
    // preference is NORMAL. SHORT + complete (no trailing "preference only" clause that would clip).
    if (grantedBits > 0 && grantedBits != selBits)
        n.info = "Output: " + juce::String (grantedBits) + "-bit float (shared mode)" + kDash + "normal.";

    // The raw-rail (D2) caveat stays cautionary if the endpoint rate could not be confirmed.
    const auto railNote = rawRailNote (rail);
    if (railNote.isNotEmpty())
        n.warnings.add (railNote);

    return n;
}

} // namespace eb
