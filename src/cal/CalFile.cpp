#include "cal/CalFile.h"
#include <juce_cryptography/juce_cryptography.h>   // juce::SHA256 for contentHash
#include <cmath>

namespace eb {

static CalType detectType (const juce::String& upper) {
    if (upper.contains ("HEQ")) return CalType::Heq;
    if (upper.contains ("HPN")) return CalType::Hpn;
    if (upper.contains ("IDF")) return CalType::Idf;
    if (upper.contains ("RAW")) return CalType::Raw;
    return CalType::Unknown;
}

// A side-delimiter is the start/end of the name or one of these separators. A single-letter L/R
// token only counts as a side marker when delimited on BOTH ends (so "realtek" / "L1234" don't).
static bool isSideDelim (juce::juce_wchar c) {
    return c == '_' || c == '-' || c == '.' || c == ' ';
}

// True iff an isolated (delimited-both-sides) single letter `letter` appears in the uppercased name.
static bool hasDelimitedLetter (const juce::String& upper, juce::juce_wchar letter) {
    const int n = upper.length();
    for (int i = 0; i < n; ++i) {
        if (upper[i] != letter) continue;
        const bool leftDelim  = (i == 0)     || isSideDelim (upper[i - 1]);
        const bool rightDelim = (i == n - 1) || isSideDelim (upper[i + 1]);
        if (leftDelim && rightDelim) return true;
    }
    return false;
}

CalSide sideFromFilename (const juce::String& fileName) {
    const auto upper = fileName.upToLastOccurrenceOf (".", false, false)   // drop the extension
                               .toUpperCase();
    if (upper.isEmpty()) return CalSide::Unknown;

    // 1) Whole words win. If both appear (a renamed/ambiguous name) it's Unknown, never a guess.
    const bool wordLeft  = upper.containsWholeWord ("LEFT");
    const bool wordRight = upper.containsWholeWord ("RIGHT");
    if (wordLeft != wordRight) return wordLeft ? CalSide::Left : CalSide::Right;
    if (wordLeft && wordRight) return CalSide::Unknown;

    // 2) Else a delimited single-letter L/R token (e.g. "L_HPN_...", "..._R", "0000000-R").
    const bool letterL = hasDelimitedLetter (upper, 'L');
    const bool letterR = hasDelimitedLetter (upper, 'R');
    if (letterL != letterR) return letterL ? CalSide::Left : CalSide::Right;

    return CalSide::Unknown;   // none, or both -> can't tell
}

CalFile CalFile::parse (const juce::String& text) {
    CalFile out;
    out.contentHash = juce::SHA256 (text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();

    auto lines = juce::StringArray::fromLines (text);
    bool sawDataRow = false;
    // #9: cap the row-warning accumulation. A file full of junk rows would otherwise grow one warning STRING
    // (holding the full line text) per row - unbounded memory on a pathological/huge file. Keep the first N.
    auto addWarning = [&out] (const juce::String& w) {
        constexpr int kMaxWarnings = 20;
        if      (out.parseWarnings.size() <  kMaxWarnings) out.parseWarnings.add (w);
        else if (out.parseWarnings.size() == kMaxWarnings) out.parseWarnings.add ("... (further row warnings suppressed)");
    };
    for (auto raw : lines) {
        auto line = raw.trim();
        if (line.isEmpty()) continue;

        // A header/comment line: explicitly marked (" / *), or any non-numeric
        // line before the first data row (best-effort side/serial extraction).
        const bool looksLikeComment = line.startsWithChar ('"') || line.startsWith ("*");
        const bool looksNumeric = line.substring (0, 1).containsOnly ("0123456789.+-");
        if (looksLikeComment || (! sawDataRow && ! looksNumeric)) {
            auto upper = line.toUpperCase();
            if (out.type == CalType::Unknown) out.type = detectType (upper);
            if (out.side == CalSide::Unknown) {
                // #8: resolve from the CHANNEL-bearing phrase, not bare word presence. The factory header
                // carries the channel AND a per-unit "sensitive side" in ONE line ("...on the RIGHT channel.
                // Your sensitive side is LEFT."), so a bare containsIgnoreCase("left") mis-sides every unit
                // whose sensitive side differs from the channel. Prefer "LEFT/RIGHT channel"; fall back to a
                // bare word only when unambiguous (both words present -> leave Unknown for the filename to fill).
                const bool chanLeft  = line.containsIgnoreCase ("left channel")  || line.containsIgnoreCase ("left ch ");
                const bool chanRight = line.containsIgnoreCase ("right channel") || line.containsIgnoreCase ("right ch ");
                if (chanLeft != chanRight) {
                    out.side = chanLeft ? CalSide::Left : CalSide::Right;
                } else if (! chanLeft && ! chanRight) {
                    const bool bareLeft  = line.containsIgnoreCase ("left");
                    const bool bareRight = line.containsIgnoreCase ("right");
                    if (bareLeft != bareRight) out.side = bareLeft ? CalSide::Left : CalSide::Right;
                }
            }
            auto idx = upper.indexOf ("SERIAL");
            if (out.serial.isEmpty() && idx >= 0) {
                auto after = line.substring (idx + 6).trim();
                auto tok = juce::StringArray::fromTokens (after, " ,\"", "")[0];
                out.serial = tok.trim();
            }
            continue;
        }

        auto toks = juce::StringArray::fromTokens (line, " \t", "");
        toks.removeEmptyStrings();
        if (toks.size() < 2) continue;
        if (! toks[0].containsOnly ("0123456789.+-eE")) continue;
        sawDataRow = true;

        // #26: validate the SPL (and phase) tokens too - not just the frequency. getDoubleValue() returns 0.0
        // for non-numeric text and stops at a comma, so "1000 junk" silently became 0.0 dB and a comma-decimal
        // locale export ("1000 -1,5") silently truncated to -1. Reject + record instead of accepting garbage.
        const char* numChars = "0123456789.+-eE";
        if (! toks[1].containsOnly (numChars) || (toks.size() >= 3 && ! toks[2].containsOnly (numChars))) {
            addWarning ("Skipped row with a non-numeric SPL/phase value (comma-decimal locale?): \"" + line + "\"");
            continue;
        }

        CalPoint p;
        p.freqHz   = toks[0].getDoubleValue();
        p.splDb    = toks[1].getDoubleValue();
        p.phaseDeg = toks.size() >= 3 ? toks[2].getDoubleValue() : 0.0;

        // Permissive but recording: drop a row that can't yield a usable point.
        if (! std::isfinite (p.freqHz) || ! std::isfinite (p.splDb) || ! std::isfinite (p.phaseDeg)
            || p.freqHz <= 0.0) {
            addWarning ("Skipped non-finite or non-positive-frequency row: \"" + line + "\"");
            continue;
        }

        if (! out.points.empty() && p.freqHz <= out.points.back().freqHz)
            addWarning ("Frequency not strictly ascending at "
                        + juce::String (p.freqHz, 3) + " Hz (previous "
                        + juce::String (out.points.back().freqHz, 3) + " Hz)");

        out.points.push_back (p);
    }

    if (out.points.empty())
        throw CalParseError ("No calibration data rows found");

    return out;
}

} // namespace eb
