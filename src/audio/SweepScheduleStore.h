#pragma once
#include "audio/SweepSchedule.h"
#include <juce_core/juce_core.h>

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
    s.valid = s.segments.size() >= 2;
    return s;
}

} // namespace eb
