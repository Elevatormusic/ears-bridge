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

CalFile CalFile::parse (const juce::String& text) {
    CalFile out;
    out.contentHash = juce::SHA256 (text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();

    auto lines = juce::StringArray::fromLines (text);
    bool sawDataRow = false;
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
                if (line.containsIgnoreCase ("left"))       out.side = CalSide::Left;
                else if (line.containsIgnoreCase ("right")) out.side = CalSide::Right;
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

        CalPoint p;
        p.freqHz   = toks[0].getDoubleValue();
        p.splDb    = toks[1].getDoubleValue();
        p.phaseDeg = toks.size() >= 3 ? toks[2].getDoubleValue() : 0.0;

        // Permissive but recording: drop a row that can't yield a usable point.
        if (! std::isfinite (p.freqHz) || ! std::isfinite (p.splDb) || ! std::isfinite (p.phaseDeg)
            || p.freqHz <= 0.0) {
            out.parseWarnings.add ("Skipped non-finite or non-positive-frequency row: \"" + line + "\"");
            continue;
        }

        if (! out.points.empty() && p.freqHz <= out.points.back().freqHz)
            out.parseWarnings.add ("Frequency not strictly ascending at "
                                   + juce::String (p.freqHz, 3) + " Hz (previous "
                                   + juce::String (out.points.back().freqHz, 3) + " Hz)");

        out.points.push_back (p);
    }

    if (out.points.empty())
        throw CalParseError ("No calibration data rows found");

    return out;
}

} // namespace eb
