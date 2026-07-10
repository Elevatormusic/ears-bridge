#pragma once
#include <juce_core/juce_core.h>

namespace eb {

// P4 L10 microcopy constants (spec §7 [P4-refresh 2026-07-05]). ALL user-facing wizard copy routes
// its typography through these five, so ASCII and typographic forms can never mix. Frozen from the
// approved frames: spaced em dash for clause breaks, real ellipsis, middot tallies, arrow paths.
// Escapes only - never raw UTF-8 bytes in source (MSVC source-encoding safety).
// RULES: diagnostic-log lines (logLine) stay pure ASCII forever (grep-ability). Minus signs on
// numbers ("-18 dBFS") stay ASCII hyphen-minus. Component IDs / test names / file paths untouched.
inline const juce::String kEmDash   { juce::CharPointer_UTF8 ("\xe2\x80\x94") };    // "—"  bare: no-value metric placeholder
inline const juce::String kDash     { juce::CharPointer_UTF8 (" \xe2\x80\x94 ") };  // " — " the clause separator (frame-exact)
inline const juce::String kEllipsis { juce::CharPointer_UTF8 ("\xe2\x80\xa6") };    // "…"  progress + chooser verbs
inline const juce::String kMiddot   { juce::CharPointer_UTF8 (" \xc2\xb7 ") };      // " · " compact tallies/clusters (frame-exact)
inline const juce::String kArrow    { juce::CharPointer_UTF8 (" \xe2\x86\x92 ") };  // " → " signal-path summaries (spec §5.1)

} // namespace eb
