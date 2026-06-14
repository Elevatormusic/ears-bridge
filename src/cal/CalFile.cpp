#include "cal/CalFile.h"

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
    auto lines = juce::StringArray::fromLines (text);
    for (auto raw : lines) {
        auto line = raw.trim();
        if (line.isEmpty()) continue;

        if (line.startsWithChar ('"') || line.startsWith ("*")) {
            auto upper = line.toUpperCase();
            if (out.type == CalType::Unknown) out.type = detectType (upper);
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

        CalPoint p;
        p.freqHz   = toks[0].getDoubleValue();
        p.splDb    = toks[1].getDoubleValue();
        p.phaseDeg = toks.size() >= 3 ? toks[2].getDoubleValue() : 0.0;
        out.points.push_back (p);
    }

    if (out.points.empty())
        throw CalParseError ("No calibration data rows found");

    for (size_t i = 1; i < out.points.size(); ++i)
        if (out.points[i].freqHz <= out.points[i - 1].freqHz)
            throw CalParseError ("Calibration frequencies must increase monotonically");

    return out;
}

} // namespace eb
