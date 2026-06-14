#include "cal/CalFile.h"

namespace eb {

CalFile CalFile::parse (const juce::String& text) {
    CalFile out;
    auto lines = juce::StringArray::fromLines (text);
    for (auto raw : lines) {
        auto line = raw.trim();
        if (line.isEmpty()) continue;
        if (line.startsWith ("*")) continue;          // comment
        if (line.startsWithChar ('"')) continue;       // header strings (handled in Task 3)

        auto toks = juce::StringArray::fromTokens (line, " \t", "");
        toks.removeEmptyStrings();
        if (toks.size() < 2) continue;                 // not a data row
        if (! toks[0].containsOnly ("0123456789.+-eE")) continue;

        CalPoint p;
        p.freqHz   = toks[0].getDoubleValue();
        p.splDb    = toks[1].getDoubleValue();
        p.phaseDeg = toks.size() >= 3 ? toks[2].getDoubleValue() : 0.0;
        out.points.push_back (p);
    }
    if (out.points.empty())
        throw CalParseError ("No calibration data rows found");
    return out;
}

} // namespace eb
