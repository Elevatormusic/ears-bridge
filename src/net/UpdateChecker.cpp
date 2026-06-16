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

UpdateInfo parseRelease (const juce::String& jsonBody, const juce::String& currentVersion) {
    UpdateInfo info;
    info.reachedServer = true;                      // we have a response body
    auto json = juce::JSON::parse (jsonBody);
    auto tag  = json.getProperty ("tag_name", juce::var()).toString();
    if (tag.isEmpty()) return info;                 // error/rate-limit JSON has no tag_name
    if (isNewer (currentVersion, tag)) {
        info.updateAvailable = true;
        info.latestVersion = tag.startsWithIgnoreCase ("v") ? tag.substring (1) : tag;
        auto html = json.getProperty ("html_url", juce::var()).toString();
        info.releaseUrl = html.isNotEmpty()
            ? html
            : juce::String ("https://github.com/Elevatormusic/ears-bridge/releases");
    }
    return info;
}

} // namespace eb
