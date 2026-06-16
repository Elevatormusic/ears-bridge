#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>   // juce::MessageManager (callback marshalling)
#include <atomic>
#include <functional>
#include <memory>

namespace eb {

// Returns true iff `latestTag` is a strictly newer semantic version than `currentVersion`.
// Accepts an optional leading 'v'/'V' and ignores any pre-release/build suffix after patch
// (e.g. "v0.2.13-beta" -> 0.2.13). Malformed/empty `latestTag` -> false.
bool isNewer (juce::String currentVersion, juce::String latestTag);

// Result of an update check.
struct UpdateInfo {
    bool reachedServer   = false;   // true once GitHub returned any response body
    bool updateAvailable = false;
    juce::String latestVersion;     // e.g. "0.2.13" (no leading 'v')
    juce::String releaseUrl;        // the release's html_url, for the browser
};

// Parse a GitHub `releases/latest` JSON body and decide whether it is newer than
// `currentVersion`. Pure (no network). A body with no "tag_name" (e.g. a rate-limit
// error) or unparseable JSON yields { reachedServer=true, updateAvailable=false }.
UpdateInfo parseRelease (const juce::String& jsonBody, const juce::String& currentVersion);

// One-shot, cancel-safe update check. start() fetches GitHub's releases/latest on a
// background thread and invokes onDone on the MESSAGE thread. If this object is destroyed
// before the fetch returns, the callback is silently dropped (so a destroyed owner is never
// called). Fire-and-forget: safe to keep as a value member.
class UpdateChecker {
public:
    UpdateChecker() : alive (std::make_shared<std::atomic<bool>> (true)) {}
    ~UpdateChecker() { alive->store (false); }

    void start (juce::String currentVersion, std::function<void (UpdateInfo)> onDone);

private:
    std::shared_ptr<std::atomic<bool>> alive;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace eb
