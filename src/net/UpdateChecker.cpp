#include "net/UpdateChecker.h"

namespace eb {

namespace {
    // Parse up to three integer components from "x.y.z[-suffix]" (leading 'v' stripped).
    // juce::String::getIntValue() reads the leading integer and ignores trailing non-digits,
    // so "13-beta" -> 13 and "garbage" -> 0.
    void parseVersion (juce::String s, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        s = s.trim();
        if (s.startsWithIgnoreCase ("v")) s = s.substring (1);
        auto parts = juce::StringArray::fromTokens (s, ".", "");
        for (int i = 0; i < 3 && i < parts.size(); ++i)
            out[i] = parts[i].getIntValue();
    }
}

bool isNewer (juce::String currentVersion, juce::String latestTag) {
    if (latestTag.trim().isEmpty()) return false;
    int cur[3], latest[3];
    parseVersion (currentVersion, cur);
    parseVersion (latestTag, latest);
    for (int i = 0; i < 3; ++i)
        if (latest[i] != cur[i]) return latest[i] > cur[i];
    return false;
}

} // namespace eb
