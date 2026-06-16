#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// Returns true iff `latestTag` is a strictly newer semantic version than `currentVersion`.
// Accepts an optional leading 'v'/'V' and ignores any pre-release/build suffix after patch
// (e.g. "v0.2.13-beta" -> 0.2.13). Malformed/empty `latestTag` -> false.
bool isNewer (juce::String currentVersion, juce::String latestTag);

} // namespace eb
