#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// P2.9: distill a raw Windows/CoreAudio endpoint name to a spine-meta-sized summary (W2 shows
// "EARS" + kArrow + "VB-Audio Cable", not the raw endpoint names (joined by MainComponent).
// Order matters: family checks run most-specific-first (Voicemeeter is ALSO "VB-Audio"). Pure.
inline juce::String distillDeviceName (const juce::String& raw) {
    const auto name = raw.trim();
    if (name.isEmpty()) return {};
    const auto lower = name.toLowerCase();
    // Whole-word for the bare token so "Shears Audio" never reads as the jig; the real device
    // enumerates as "E.A.R.S." (dotted) or with the miniDSP vendor string.
    if (lower.contains ("e.a.r.s") || lower.contains ("minidsp")
        || name.containsWholeWordIgnoreCase ("ears"))            return "EARS";
    if (lower.contains ("hi-fi cable"))                          return "Hi-Fi Cable";
    if (lower.contains ("voicemeeter"))                          return "Voicemeeter";
    if (lower.contains ("vb-audio"))                             return "VB-Cable";
    // Fallback: Windows names read "Endpoint form (Device)" - the parenthetical IS the device.
    juce::String core = name;
    const int open = name.indexOfChar ('(');
    const int close = name.lastIndexOfChar (')');
    if (open >= 0 && close > open)
        core = name.substring (open + 1, close).trim();
    if (core.isEmpty()) core = name;
    constexpr int kMax = 24;                                     // spine meta budget (~2 lines; kArrow joiner is 3 chars)
    return core.length() > kMax ? core.substring (0, kMax - 2).trimEnd() + ".." : core;
}

} // namespace eb
