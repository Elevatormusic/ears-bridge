#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include "platform/EndpointFormat.h"

// 48k-everywhere signal-chain config check (Reference-Based Measurement Monitor).
//
// The reference monitor REQUIRES 48 kHz on ALL THREE endpoints Dirac's measurement
// passes through: (1) the EARS INPUT, (2) the virtual CABLE (EARS Bridge's output ->
// Dirac records it), and (3) DIRAC'S OWN OUTPUT DEVICE (the sweep plays out of it).
// The user's literal failure was "Dirac's output device was at 44.1k" — invisible
// today because the app only ever read the INPUT rate. A rate-resampled chain can
// silently grade clean, so this check warns AND vetoes the green "verified" verdict.
//
// PURE + header-only so the verdict logic is fully unit-testable without a live
// endpoint (build the EndpointFormat inputs by hand — they're plain structs). The
// live three-endpoint reads (readEndpointFormat / readDiracOutputDeviceName) are the
// Windows/on-device half; this module only judges the structs they return.
//
// HONESTY: the gate is ABSOLUTE (mixRateHz == 48000), NOT the relative "request ==
// mix" — a uniform-44.1k chain must NOT pass. An endpoint that couldn't be read
// (valid == false) is "unknown", which is NOT a pass: all48k stays false and the
// summary says "couldn't read X". The RATE is the only GATE / green-veto.
//
// Channels + bit-depth are SECONDARY and ADVISORY ONLY — they NEVER veto green and
// NEVER gate Start. They populate `advisory` (a calm one-line INFO note shown only
// when the rate is fine): a mono INPUT is the one advisory WARN ("both earcups can't
// be measured"); a 16-bit INTEGER endpoint is an INFO note (24-bit+ recommended);
// a non-2ch cable/output is INFO. Only readable (valid) endpoints are spoken about.
namespace eb {

// The three endpoint reads that make up the chain (each from readEndpointFormat).
struct ChainConfig {
    eb::EndpointFormat input;        // the selected EARS input  (isInput=true)
    eb::EndpointFormat cable;        // the selected virtual cable / output (isInput=false)
    eb::EndpointFormat diracOutput;  // Dirac's own output device (readDiracOutputDeviceName, isInput=false)
};

struct ConfigVerdict {
    bool checked = false;   // false if NONE of the three could be read (don't false-alarm on an empty read)
    bool all48k  = false;   // THE GATE: every endpoint valid AND at 48 kHz (an unreadable one fails this)
    bool inputOk  = false;  // input.valid  && rate == 48000
    bool cableOk  = false;  // cable.valid  && rate == 48000
    bool diracOk  = false;  // diracOutput.valid && rate == 48000
    juce::String summary;   // short ONE-LINE title-bar warning naming the wrong/unreadable endpoint(s);
                            // "" iff all48k (or nothing was checked)

    // --- SECONDARY advisory (channels + bit-depth). These NEVER gate, NEVER veto green, NEVER gate Start.
    // RATE (above) is the sole hard gate. These are surfaced only as a calm INFO/advisory note when the
    // rate is fine. Computed only from READABLE (valid) endpoints — we never assert about one we couldn't read.
    bool inputChannelsLow = false;  // input.valid && channels < 2 — the EARS has L+R earcup mics, so a mono
                                    // input can't capture both ears. The one advisory WARN (still no green-veto).
    juce::String advisory;          // short ONE-LINE secondary note, assembled (most-important-first) from:
                                    // the input-channels warn, then any 16-bit-integer endpoint(s), then a
                                    // non-2ch cable/output (info). "" when there is nothing to note.
};

// True iff this endpoint was read AND its OS mix rate is within 1 Hz of 48 kHz.
[[nodiscard]] inline bool endpointIs48k (const eb::EndpointFormat& f) noexcept {
    return f.valid && std::abs (f.mixRateHz - 48000.0) <= 1.0;
}

// Pure verdict. checked = at least one endpoint read succeeded. Each *Ok = that
// endpoint is valid AND 48k. all48k = inputOk && cableOk && diracOk — so an
// INVALID/unreadable endpoint makes all48k false (it is "unknown", never a pass).
// The summary is a single short line that names the off-48k endpoint(s) (with the
// actual rate, e.g. "44.1k") and/or the unreadable one(s), or "" when all48k.
[[nodiscard]] inline ConfigVerdict checkChainConfig (const ChainConfig& c) {
    ConfigVerdict v;
    v.checked = c.input.valid || c.cable.valid || c.diracOutput.valid;
    v.inputOk = endpointIs48k (c.input);
    v.cableOk = endpointIs48k (c.cable);
    v.diracOk = endpointIs48k (c.diracOutput);
    v.all48k  = v.inputOk && v.cableOk && v.diracOk;

    // --- SECONDARY advisory: channels + bit-depth. Computed independently of the RATE gate (it must be
    // available even when all48k is true, so a clean-rate chain can still show a calm 16-bit/mono note).
    // Only ever speaks about READABLE endpoints. Never touches all48k / summary / the *Ok flags.
    // An INTEGER 16-bit endpoint is lower resolution; float (isFloat) or >=24-bit is transparent -> no note.
    auto is16BitInt = [] (const eb::EndpointFormat& f) {
        return f.valid && ! f.isFloat && f.bits > 0 && f.bits <= 16;
    };
    v.inputChannelsLow = c.input.valid && c.input.channels < 2;   // the one advisory WARN

    juce::StringArray adv;
    // 1) The input-channels WARN comes FIRST (it's the most important — both earcups can't be measured).
    if (v.inputChannelsLow)
        adv.add ("input is mono - both earcups can't be measured");
    // 2) Any 16-bit-integer endpoint(s): one compact note naming which side(s). INFO, never a gate.
    {
        juce::StringArray sixteen;
        if (is16BitInt (c.input))       sixteen.add ("input");
        if (is16BitInt (c.cable))       sixteen.add ("cable");
        if (is16BitInt (c.diracOutput)) sixteen.add ("Dirac output");
        if (! sixteen.isEmpty())
            adv.add (sixteen.joinIntoString (", ") + " 16-bit - 24-bit+ recommended for the cleanest measurement");
    }
    // 3) A non-2ch cable / Dirac output: INFO only (the input's channel count is handled by the WARN above).
    {
        juce::StringArray nonStereo;
        if (c.cable.valid       && c.cable.channels != 2)       nonStereo.add ("cable");
        if (c.diracOutput.valid && c.diracOutput.channels != 2) nonStereo.add ("Dirac output");
        if (! nonStereo.isEmpty())
            adv.add (nonStereo.joinIntoString (", ") + " not 2-channel");
    }
    v.advisory = adv.joinIntoString ("; ");

    if (! v.checked || v.all48k)
        return v;   // nothing to judge, or every endpoint is a clean 48k -> no RATE warning (advisory may still be set)

    // Compact a rate to a human label: 44100 -> "44.1k", 48000 -> "48k", 96000 -> "96k".
    auto rateLabel = [] (double hz) -> juce::String {
        const double k = hz / 1000.0;
        if (std::abs (k - std::round (k)) < 0.05)
            return juce::String ((int) std::round (k)) + "k";
        return juce::String (k, 1) + "k";
    };
    // One clause per endpoint that is NOT a clean 48k: either it read off-48k (name the rate)
    // or it couldn't be read at all (honesty: "couldn't read X", never implied good).
    auto clause = [&] (const eb::EndpointFormat& f, const char* label) -> juce::String {
        if (endpointIs48k (f)) return {};
        if (! f.valid)         return juce::String ("couldn't read the ") + label + " rate";
        return juce::String (label) + " is " + rateLabel (f.mixRateHz);
    };

    juce::StringArray parts;
    parts.addIfNotAlreadyThere (clause (c.input,       "input"));
    parts.addIfNotAlreadyThere (clause (c.cable,       "cable"));
    parts.addIfNotAlreadyThere (clause (c.diracOutput, "Dirac output"));
    parts.removeEmptyStrings();

    // Keep it to ONE short title-bar line. End off-48k cases with the fix ("- set it to 48k");
    // a pure "couldn't read" line gets no fix tail (there's nothing to set yet).
    const bool anyOff48k = (c.input.valid && ! v.inputOk)
                        || (c.cable.valid && ! v.cableOk)
                        || (c.diracOutput.valid && ! v.diracOk);
    v.summary = parts.joinIntoString (", ");
    if (anyOff48k) v.summary << " - set it to 48k";
    return v;
}

} // namespace eb
