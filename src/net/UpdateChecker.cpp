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
        // #27: the URL is launched in the OS browser (ShellExecute honours ANY registered scheme - file://,
        // UNC, ms-msdt:, ...). Accept the response's html_url ONLY when it is a github.com https URL;
        // anything else (a tampered/malicious response body) falls back to the hardcoded releases page.
        info.releaseUrl = html.startsWithIgnoreCase ("https://github.com/")
            ? html
            : juce::String ("https://github.com/Elevatormusic/ears-bridge/releases");
    }
    return info;
}

void UpdateChecker::start (juce::String currentVersion, std::function<void (UpdateInfo)> onDone) {
    auto a = alive;
    juce::Thread::launch ([currentVersion, onDone = std::move (onDone), a]() mutable {
        UpdateInfo info;
        juce::URL url ("https://api.github.com/repos/Elevatormusic/ears-bridge/releases/latest");
        // Generous connection timeout: a cold DNS+TLS handshake to api.github.com can take
        // 10 s+ on the first call after launch (warm calls are <1 s). This runs on a background
        // thread and never blocks the UI, so a long timeout costs nothing — and a too-short one
        // silently fails the check on every cold launch (reachedServer stays false). Failure just
        // defers to the next launch, so erring long is strictly safer than erring short.
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (15000)
                        .withExtraHeaders ("User-Agent: EARS-Bridge/" + currentVersion
                                           + "\r\nAccept: application/vnd.github+json");
        if (auto stream = url.createInputStream (opts))
            info = parseRelease (stream->readEntireStreamAsString(), currentVersion);
        // else: offline / connect failed -> info.reachedServer stays false (retry next launch)

        juce::MessageManager::callAsync ([onDone = std::move (onDone), a, info]() mutable {
            if (a->load()) onDone (info);
        });
    });
}

} // namespace eb
