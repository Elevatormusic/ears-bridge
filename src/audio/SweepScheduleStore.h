#pragma once
#include "audio/SweepSchedule.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

// AutoPerEar hardening (P0-06) Task 4 — persist a learned SweepSchedule as a small text sidecar, KEYED to a
// hash of the reference it was learned from, so a STALE schedule can never pair with a re-learned reference.
// The reference itself (reference_L/R.f32) is persisted separately; the schedule must travel with it and be
// dropped the moment the reference changes. Pure (juce_core String only) so the unit suite drives every
// branch; the file IO + the reference hashing live in the caller (MainComponent).
//
// Format (one token-line each):
//   v1
//   <referenceHash>
//   seg <ear:0|1> <durationSec>   (repeated, in time order -> firstEar/order/trailing-ref all preserved)
//   gap <sec>                     (repeated; size == segments-1)
namespace eb {

[[nodiscard]] inline juce::String serializeSchedule (const SweepSchedule& s, const juce::String& referenceHash) {
    juce::String out;
    out << "v1\n" << referenceHash << "\n";
    for (const auto& seg : s.segments) out << "seg " << (int) seg.ear << " " << juce::String (seg.durationSec, 6) << "\n";
    for (double g : s.gapsSec)         out << "gap " << juce::String (g, 6) << "\n";
    return out;
}

// Parse a sidecar. Returns a VALID schedule ONLY when: the text parses, recovers >= 2 segments, AND its
// stored referenceHash == currentReferenceHash (the STALE GUARD). Any mismatch / empty current hash / parse
// failure -> { valid=false } so the caller installs nothing and the router falls back to the mic envelope.
[[nodiscard]] inline SweepSchedule deserializeSchedule (const juce::String& text, const juce::String& currentReferenceHash) {
    SweepSchedule s;
    auto lines = juce::StringArray::fromLines (text);
    if (lines.size() < 2 || lines[0].trim() != "v1") return s;                         // bad / old format
    if (currentReferenceHash.isEmpty() || lines[1].trim() != currentReferenceHash) return s;  // STALE / no reference -> drop
    for (int i = 2; i < lines.size(); ++i) {
        auto toks = juce::StringArray::fromTokens (lines[i].trim(), " ", "");
        toks.removeEmptyStrings();
        if (toks.size() == 3 && toks[0] == "seg")
            s.segments.push_back ({ toks[1].getIntValue() == 1 ? Ear::Right : Ear::Left, toks[2].getDoubleValue() });
        else if (toks.size() == 2 && toks[0] == "gap")
            s.gapsSec.push_back (toks[1].getDoubleValue());
    }
    // #70: reject a corrupt / hand-edited sidecar rather than installing garbage. A non-finite/<=0 segment
    // duration or a non-finite/<0 gap (getDoubleValue() -> 0.0 on junk) would wedge ScheduledEarRouter into
    // permanent "routing ambiguous". Leaving valid=false engages the mic-envelope fallback (the safe default).
    const bool durationsOk = std::all_of (s.segments.begin(), s.segments.end(),
                                          [] (const SweepSegment& seg) { return std::isfinite (seg.durationSec) && seg.durationSec > 0.0; });
    const bool gapsOk      = std::all_of (s.gapsSec.begin(), s.gapsSec.end(),
                                          [] (double g) { return std::isfinite (g) && g >= 0.0; });
    s.valid = s.segments.size() >= 2 && durationsOk && gapsOk
           && s.gapsSec.size() == s.segments.size() - 1;   // exactly one gap between each adjacent segment pair
    return s;
}

} // namespace eb
