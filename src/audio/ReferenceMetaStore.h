#pragma once
#include "audio/LoopbackReference.h"   // eb::ReferenceMetadata
#include <juce_core/juce_core.h>
#include <cmath>

// Audit #5/#20 — persist the learned reference's INTEGRITY METADATA as a small text sidecar next to
// reference_L.f32 / reference_R.f32, so a reload can verify (a) the RATE the capture was really made at
// (the loader previously hardcoded 48 kHz while the capture asserted the user-selectable settings rate -
// a mislabeled reference defeated the grade-time rate guard in BOTH directions), and (b) that the two
// channel files on disk are the SAME GENERATION and UNTRUNCATED (nothing previously bound L to R, and the
// f32 writes were unchecked - a disk-full mid-write could install a mixed or cut-off pair that still
// cross-correlates and grades against garbage). Pure (juce_core String only) so the unit suite drives every
// branch; file IO + hashing of the disk bytes live in the caller (MainComponent).
//
// Format (one token-line each; the endpoint line is OPTIONAL — audit #34):
//   v1
//   rate <hz>
//   L <lengthSamples> <sha256hex>
//   R <lengthSamples> <sha256hex>
//   endpoint <id-or-name>            (the Dirac output the loopback was captured FROM; may contain spaces)
//
// The endpoint line is ADVISORY, not part of the integrity gate: a sidecar without it (v0.4.0) stays
// valid, and a mismatch never invalidates the pair — the caller surfaces a start-time "learned against
// a different device" warning instead (a device swap silently changed the comparison basis, #34).
// v0.4.0's parser ignores unknown trailing lines, so this addition is forward- AND backward-compatible.
namespace eb {

[[nodiscard]] inline juce::String serializeReferenceMeta (const ReferenceMetadata& l, const ReferenceMetadata& r,
                                                          const juce::String& endpoint = {}) {
    juce::String out;
    out << "v1\n"
        << "rate " << juce::String (l.rate, 3) << "\n"
        << "L " << l.lengthSamples << " " << l.contentHash << "\n"
        << "R " << r.lengthSamples << " " << r.contentHash << "\n";
    if (endpoint.isNotEmpty())
        out << "endpoint " << endpoint << "\n";
    return out;
}

struct ReferenceMetaCheck {
    bool         valid = false;
    double       rate  = 0.0;    // the learn-time capture rate (only meaningful when valid)
    juce::String endpoint;       // #34: the learn-time Dirac output id/name ("" = not recorded / legacy sidecar)
    juce::String reason;         // why the check failed (empty when valid)
};

// Verify a sidecar against metadata RECOMPUTED FROM THE BYTES ON DISK (the caller hashes what it just
// loaded). valid only when: v1, a plausible finite rate, and BOTH channels' length + SHA-256 match. Any
// mismatch names the failing part so the caller can route to the re-learn UX with an honest reason.
[[nodiscard]] inline ReferenceMetaCheck checkReferenceMeta (const juce::String& text,
                                                            const ReferenceMetadata& diskL,
                                                            const ReferenceMetadata& diskR) {
    ReferenceMetaCheck c;
    auto lines = juce::StringArray::fromLines (text);
    if (lines.size() < 4 || lines[0].trim() != "v1") { c.reason = "missing or unversioned sidecar"; return c; }

    auto rateToks = juce::StringArray::fromTokens (lines[1].trim(), " ", "");
    rateToks.removeEmptyStrings();
    if (rateToks.size() != 2 || rateToks[0] != "rate") { c.reason = "malformed rate line"; return c; }
    const double rate = rateToks[1].getDoubleValue();
    if (! std::isfinite (rate) || rate < 8000.0 || rate > 384000.0) { c.reason = "implausible rate"; return c; }

    const auto checkLine = [] (const juce::String& line, const char* tag,
                               const ReferenceMetadata& disk, juce::String& why) {
        auto t = juce::StringArray::fromTokens (line.trim(), " ", "");
        t.removeEmptyStrings();
        if (t.size() != 3 || t[0] != juce::String (tag)) { why = juce::String (tag) + " line malformed";       return false; }
        if (t[1].getIntValue() != disk.lengthSamples)    { why = juce::String (tag) + " length mismatch";      return false; }
        if (! t[2].equalsIgnoreCase (disk.contentHash))  { why = juce::String (tag) + " content-hash mismatch"; return false; }
        return true;
    };
    juce::String why;
    if (! checkLine (lines[2], "L", diskL, why)) { c.reason = why; return c; }
    if (! checkLine (lines[3], "R", diskR, why)) { c.reason = why; return c; }

    // #34: the OPTIONAL endpoint line (advisory — never gates validity). Scan the trailing lines so a
    // future field order can't break it; first match wins.
    for (int i = 4; i < lines.size(); ++i) {
        const auto t = lines[i].trim();
        if (t.startsWith ("endpoint ")) { c.endpoint = t.substring (9).trim(); break; }
    }

    c.valid = true;
    c.rate  = rate;
    return c;
}

} // namespace eb
