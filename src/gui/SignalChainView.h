#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include "audio/DeviceConfigCheck.h"   // ChainConfig, EndpointFormat, endpointIs48k

// Pure presentation model for the "Signal chain" device-format panel. The panel shows the THREE measurement
// endpoints Dirac's sweep passes through — the EARS input, the virtual cable, and Dirac's own output device —
// each with its sample rate (the 48 kHz gate), channel count, and bit depth, plus a per-endpoint state.
//
// HIG: meaning is NEVER carried by colour alone — each row's STATE is an explicit enum the panel renders as a
// glyph + word ("48 kHz" reads as good on its own; "44.1 kHz" + a warning glyph reads as wrong; "?" + "couldn't
// read" reads as unknown). This header is pure + unit-testable (no JUCE GUI); the panel maps ChainRowState to a
// semantic Theme colour + an accessible label. Reuses checkChainConfig's RATE gate (endpointIs48k) verbatim, so
// the panel and the existing one-line title-bar verdict can never disagree.
namespace eb {

enum class ChainRowState { Good, Warn, Unknown };   // 48 kHz / off-rate / unreadable

struct ChainRow {
    juce::String  name;                         // "EARS input"
    juce::String  rate;                         // "48 kHz" / "44.1 kHz" / "?"
    juce::String  detail;                       // "2 ch, 24-bit float" / "couldn't read"
    ChainRowState state = ChainRowState::Unknown;
};

// Format ONE endpoint into a display row. Rate is the gate: 48 kHz -> Good, any other readable rate -> Warn, an
// unreadable endpoint -> Unknown (never silently "good"). Detail carries channels + bit depth for readable ones.
[[nodiscard]] inline ChainRow chainRow (const juce::String& name, const eb::EndpointFormat& f) {
    ChainRow r;
    r.name = name;
    if (! f.valid) {                            // unreadable -> honest unknown, not a pass
        r.rate   = "?";
        r.detail = "couldn't read";
        r.state  = ChainRowState::Unknown;
        return r;
    }
    const double k = f.mixRateHz / 1000.0;      // 48000 -> "48 kHz", 44100 -> "44.1 kHz", 96000 -> "96 kHz"
    r.rate  = (std::abs (k - std::round (k)) < 0.05 ? juce::String ((int) std::round (k)) : juce::String (k, 1)) + " kHz";
    r.state = eb::endpointIs48k (f) ? ChainRowState::Good : ChainRowState::Warn;
    juce::String detail = juce::String (f.channels) + (f.channels == 1 ? " ch (mono)" : " ch");
    if (f.bits > 0) detail += ", " + juce::String (f.bits) + "-bit" + (f.isFloat ? " float" : "");
    r.detail = detail;
    return r;
}

// The three rows in chain order (what the sweep traverses): EARS input -> EARS Bridge -> virtual cable -> Dirac
// records it; Dirac's own output device plays the sweep. Fixed order so the panel layout is stable.
[[nodiscard]] inline std::array<ChainRow, 3> signalChainRows (const eb::ChainConfig& c) {
    return { chainRow ("EARS input",    c.input),
             chainRow ("Virtual cable", c.cable),
             chainRow ("Dirac output",  c.diracOutput) };
}

} // namespace eb
