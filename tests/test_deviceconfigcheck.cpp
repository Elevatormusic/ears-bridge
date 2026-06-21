// 48k-everywhere signal-chain config check (Reference-Based Measurement Monitor).
//
// Tests the PURE verdict logic in src/audio/DeviceConfigCheck.h — the gate that says
// whether the EARS input, the virtual cable, and DIRAC'S OUTPUT device are all at
// 48 kHz, and that VETOES the green "verified" status when they're not. The live
// endpoint reads are on-device; the EndpointFormat inputs here are built by hand
// (they're plain structs), so the whole verdict is exercised headless.
//
// The negative cases the gap-finder scoped:
//   - uniform 48k                     -> all48k, no warning
//   - Dirac output 44.1k              -> flagged, names Dirac  (the user's literal failure)
//   - cable 44.1k                     -> flagged, names cable
//   - uniform 44.1k                   -> flagged (the relative-vs-absolute trap; NOT a pass)
//   - one endpoint unreadable         -> not all48k, "couldn't read" (honesty, never "good")
//   - all endpoints unreadable        -> checked=false, no false alarm

#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceConfigCheck.h"

using eb::EndpointFormat;
using eb::ChainConfig;

// A valid endpoint read at a given rate (channels/bits/float are INFO-only in v1 but
// set to plausible shared-mode values so nothing reads as a degenerate format).
static EndpointFormat fmt (double rateHz, int ch = 2, int bits = 32, bool isFloat = true) {
    EndpointFormat f;
    f.valid = true;
    f.mixRateHz = rateHz;
    f.channels = ch;
    f.bits = bits;
    f.isFloat = isFloat;
    f.exclusive48kSupported = false;
    return f;
}

// An endpoint that could NOT be read (read failed / name not matched). valid=false.
static EndpointFormat unreadable() { return EndpointFormat{}; }

TEST_CASE ("uniform 48k passes with no warning", "[deviceconfigcheck]") {
    ChainConfig c { fmt (48000.0), fmt (48000.0), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.checked);
    REQUIRE (v.all48k);
    REQUIRE (v.inputOk);
    REQUIRE (v.cableOk);
    REQUIRE (v.diracOk);
    REQUIRE (v.summary.isEmpty());   // "" -> the green "verified" branch is NOT vetoed
}

TEST_CASE ("48000 +/- 1 Hz still counts as 48k", "[deviceconfigcheck]") {
    // The gate is "within 1 Hz of 48000" so a 47999.5/48000.5 mix-format read isn't a false alarm.
    ChainConfig c { fmt (47999.4), fmt (48000.6), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);
    REQUIRE (v.summary.isEmpty());
}

TEST_CASE ("Dirac output at 44.1k is flagged and named (the user's literal failure)", "[deviceconfigcheck]") {
    ChainConfig c { fmt (48000.0), fmt (48000.0), fmt (44100.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.checked);
    REQUIRE_FALSE (v.all48k);          // the VETO: a wrong-rate chain must never read green
    REQUIRE (v.inputOk);
    REQUIRE (v.cableOk);
    REQUIRE_FALSE (v.diracOk);
    REQUIRE (v.summary.containsIgnoreCase ("Dirac"));
    REQUIRE (v.summary.contains ("44.1k"));
    REQUIRE (v.summary.contains ("48k"));   // the fix tail
}

TEST_CASE ("cable at 44.1k is flagged and named", "[deviceconfigcheck]") {
    ChainConfig c { fmt (48000.0), fmt (44100.0), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE_FALSE (v.all48k);
    REQUIRE (v.inputOk);
    REQUIRE_FALSE (v.cableOk);
    REQUIRE (v.diracOk);
    REQUIRE (v.summary.containsIgnoreCase ("cable"));
    REQUIRE (v.summary.contains ("44.1k"));
}

TEST_CASE ("input at 44.1k is flagged and named", "[deviceconfigcheck]") {
    ChainConfig c { fmt (44100.0), fmt (48000.0), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE_FALSE (v.all48k);
    REQUIRE_FALSE (v.inputOk);
    REQUIRE (v.summary.containsIgnoreCase ("input"));
}

TEST_CASE ("uniform 44.1k is NOT a pass (the relative-vs-absolute trap)", "[deviceconfigcheck]") {
    // The whole point of the absolute gate: a chain that is internally consistent at 44.1k
    // would read "verified" under a relative request==mix check. It must FAIL the 48k gate.
    ChainConfig c { fmt (44100.0), fmt (44100.0), fmt (44100.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.checked);
    REQUIRE_FALSE (v.all48k);
    REQUIRE_FALSE (v.inputOk);
    REQUIRE_FALSE (v.cableOk);
    REQUIRE_FALSE (v.diracOk);
    REQUIRE (v.summary.isNotEmpty());
    REQUIRE (v.summary.contains ("44.1k"));
}

TEST_CASE ("an unreadable endpoint is NOT treated as good (honesty)", "[deviceconfigcheck]") {
    // Cable read failed: all48k must be false (unknown != pass) and the summary must say
    // "couldn't read", never imply the cable is fine.
    ChainConfig c { fmt (48000.0), unreadable(), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.checked);               // input + Dirac read OK -> we DID judge something
    REQUIRE_FALSE (v.all48k);          // the unread cable blocks the pass
    REQUIRE (v.inputOk);
    REQUIRE_FALSE (v.cableOk);
    REQUIRE (v.diracOk);
    REQUIRE (v.summary.containsIgnoreCase ("couldn't read"));
    REQUIRE (v.summary.containsIgnoreCase ("cable"));
    REQUIRE_FALSE (v.summary.containsIgnoreCase ("good"));
}

TEST_CASE ("all endpoints unreadable -> checked=false, no false alarm", "[deviceconfigcheck]") {
    ChainConfig c { unreadable(), unreadable(), unreadable() };
    const auto v = eb::checkChainConfig (c);
    REQUIRE_FALSE (v.checked);         // nothing was read -> don't warn, don't veto
    REQUIRE_FALSE (v.all48k);
    REQUIRE (v.summary.isEmpty());     // no false alarm when we know nothing
}

TEST_CASE ("two endpoints wrong are both named on one line", "[deviceconfigcheck]") {
    // Dirac 44.1k AND cable unreadable: one short title-bar line must mention both.
    ChainConfig c { fmt (48000.0), unreadable(), fmt (44100.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE_FALSE (v.all48k);
    REQUIRE (v.summary.containsIgnoreCase ("Dirac"));
    REQUIRE (v.summary.containsIgnoreCase ("cable"));
    REQUIRE_FALSE (v.summary.containsChar ('\n'));   // strictly one line
}

// ---- SECONDARY advisory: channels + bit-depth (INFO/warn, never a green-veto, never a gate) ----------

TEST_CASE ("a mono input is the advisory WARN, but does NOT veto the 48k pass", "[deviceconfigcheck]") {
    // Input is 1ch (can't capture both earcups) but every endpoint is 48k. The RATE gate still passes
    // (all48k stays true, summary empty -> green not vetoed); the advisory carries the input-channels warn.
    ChainConfig c { fmt (48000.0, /*ch*/ 1), fmt (48000.0), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);                  // rate gate UNAFFECTED -> green not vetoed by a channel count
    REQUIRE (v.summary.isEmpty());       // the rate summary (the only veto) stays empty
    REQUIRE (v.inputChannelsLow);        // the one advisory WARN
    REQUIRE (v.advisory.isNotEmpty());
    REQUIRE (v.advisory.containsIgnoreCase ("input"));
    REQUIRE_FALSE (v.advisory.containsChar ('\n'));   // strictly one line
}

TEST_CASE ("a 16-bit integer Dirac output is an INFO note, not a warn-veto", "[deviceconfigcheck]") {
    // 16-bit INTEGER (non-float) Dirac output at 48k: lower resolution -> an INFO note, never a gate.
    ChainConfig c { fmt (48000.0), fmt (48000.0), fmt (48000.0, /*ch*/ 2, /*bits*/ 16, /*isFloat*/ false) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);                  // rate fine -> green not vetoed
    REQUIRE (v.summary.isEmpty());
    REQUIRE_FALSE (v.inputChannelsLow);  // not the input warn
    REQUIRE (v.advisory.contains ("16-bit"));
    REQUIRE (v.advisory.containsIgnoreCase ("Dirac output"));
}

TEST_CASE ("a 32-bit float cable gets NO bit-depth note (float is transparent)", "[deviceconfigcheck]") {
    // 32-bit float everywhere, 2ch, 48k: nothing to note at all.
    ChainConfig c { fmt (48000.0), fmt (48000.0, /*ch*/ 2, /*bits*/ 32, /*isFloat*/ true), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);
    REQUIRE_FALSE (v.advisory.contains ("16-bit"));
    REQUIRE (v.advisory.isEmpty());
}

TEST_CASE ("a 24-bit integer endpoint gets NO bit-depth note (>=24-bit is transparent)", "[deviceconfigcheck]") {
    // 24-bit integer is high enough resolution -> no note (only 16-bit integer triggers the INFO).
    ChainConfig c { fmt (48000.0), fmt (48000.0, /*ch*/ 2, /*bits*/ 24, /*isFloat*/ false), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);
    REQUIRE (v.advisory.isEmpty());
}

TEST_CASE ("a mono cable is an INFO note, NOT the input-channels warn", "[deviceconfigcheck]") {
    // Cable at 1ch: an INFO note about the cable, but inputChannelsLow stays false (the input is fine).
    ChainConfig c { fmt (48000.0), fmt (48000.0, /*ch*/ 1), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);
    REQUIRE_FALSE (v.inputChannelsLow);                 // NOT the input warn
    REQUIRE (v.advisory.isNotEmpty());
    REQUIRE (v.advisory.containsIgnoreCase ("cable"));
    REQUIRE (v.advisory.containsIgnoreCase ("2-channel"));
}

TEST_CASE ("an all-clean chain (48k, 2ch, float) has an empty advisory and no input warn", "[deviceconfigcheck]") {
    ChainConfig c { fmt (48000.0), fmt (48000.0), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);
    REQUIRE (v.summary.isEmpty());
    REQUIRE_FALSE (v.inputChannelsLow);
    REQUIRE (v.advisory.isEmpty());
}

TEST_CASE ("an all-clean chain with 24-bit integer endpoints also has an empty advisory", "[deviceconfigcheck]") {
    // 48k, 2ch, 24-bit integer everywhere: transparent -> nothing to note.
    ChainConfig c { fmt (48000.0, 2, 24, false), fmt (48000.0, 2, 24, false), fmt (48000.0, 2, 24, false) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);
    REQUIRE (v.advisory.isEmpty());
}

TEST_CASE ("the advisory never asserts about an unreadable endpoint", "[deviceconfigcheck]") {
    // An unreadable input must NOT be claimed as mono, and an unreadable cable must NOT be claimed 16-bit.
    ChainConfig c { unreadable(), unreadable(), fmt (48000.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE_FALSE (v.inputChannelsLow);   // unreadable input -> we say nothing about its channels
    REQUIRE (v.advisory.isEmpty());       // only the readable Dirac output is clean -> nothing to note
}

TEST_CASE ("input-channels warn leads when multiple notes apply (most-important-first)", "[deviceconfigcheck]") {
    // Mono input AND a 16-bit Dirac output: ONE line, the input warn first, the 16-bit note after.
    ChainConfig c { fmt (48000.0, /*ch*/ 1), fmt (48000.0), fmt (48000.0, 2, 16, false) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE (v.all48k);                  // still a rate pass
    REQUIRE (v.inputChannelsLow);
    REQUIRE (v.advisory.containsIgnoreCase ("input"));
    REQUIRE (v.advisory.contains ("16-bit"));
    // The input warn is assembled first, so it appears before the 16-bit clause on the one line.
    REQUIRE (v.advisory.indexOfIgnoreCase ("input") < v.advisory.indexOf ("16-bit"));
    REQUIRE_FALSE (v.advisory.containsChar ('\n'));
}

TEST_CASE ("the advisory is independent of a RATE veto (a wrong-rate chain still computes it)", "[deviceconfigcheck]") {
    // Dirac output 44.1k (rate veto) AND a mono input. The rate summary still vetoes; the advisory is still
    // computed (the UI shows the rate veto first and only appends the advisory when the rate is clean).
    ChainConfig c { fmt (44100.0, /*ch*/ 1), fmt (48000.0), fmt (44100.0) };
    const auto v = eb::checkChainConfig (c);
    REQUIRE_FALSE (v.all48k);             // the RATE veto is unchanged
    REQUIRE (v.summary.isNotEmpty());     // rate summary present (the only green-veto)
    REQUIRE (v.inputChannelsLow);         // advisory fields still populated
    REQUIRE (v.advisory.containsIgnoreCase ("input"));
}
