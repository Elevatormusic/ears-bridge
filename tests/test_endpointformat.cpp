#include <catch2/catch_test_macros.hpp>
#include "platform/EndpointFormat.h"

// These exercise the PURE interpreter eb::interpretMixFormat (no live endpoint, no Windows WAVEFORMATEX),
// so they run on the dev machine / CI on every platform. The live eb::readEndpointFormat is on-device and
// is verified by running eb_diag (its mixFormat: line). The two WAVEFORMATEX tags under test:
//   1      = WAVE_FORMAT_PCM
//   3      = WAVE_FORMAT_IEEE_FLOAT
//   0xFFFE = WAVE_FORMAT_EXTENSIBLE (float-vs-int decided by the SubFormat flag we pass in)

TEST_CASE ("interpretMixFormat: PCM 16-bit is not float") {
    const auto f = eb::interpretMixFormat (/*tag*/ 1, /*rate*/ 44100, /*ch*/ 2, /*bits*/ 16,
                                           /*extFloat*/ false, /*excl48k*/ false);
    CHECK (f.valid);
    CHECK (f.mixRateHz == 44100.0);
    CHECK (f.channels == 2);
    CHECK (f.bits == 16);
    CHECK_FALSE (f.isFloat);
}

TEST_CASE ("interpretMixFormat: IEEE float 32-bit is float") {
    const auto f = eb::interpretMixFormat (/*tag*/ 3, /*rate*/ 48000, /*ch*/ 2, /*bits*/ 32,
                                           /*extFloat*/ false, /*excl48k*/ false);
    CHECK (f.valid);
    CHECK (f.mixRateHz == 48000.0);
    CHECK (f.channels == 2);
    CHECK (f.bits == 32);
    CHECK (f.isFloat);
}

TEST_CASE ("interpretMixFormat: EXTENSIBLE + float subformat is float") {
    const auto f = eb::interpretMixFormat (/*tag*/ 0xFFFE, /*rate*/ 48000, /*ch*/ 8, /*bits*/ 32,
                                           /*extFloat*/ true, /*excl48k*/ false);
    CHECK (f.valid);
    CHECK (f.channels == 8);
    CHECK (f.bits == 32);
    CHECK (f.isFloat);
}

TEST_CASE ("interpretMixFormat: EXTENSIBLE + PCM subformat is not float") {
    const auto f = eb::interpretMixFormat (/*tag*/ 0xFFFE, /*rate*/ 48000, /*ch*/ 2, /*bits*/ 24,
                                           /*extFloat*/ false, /*excl48k*/ false);
    CHECK (f.valid);
    CHECK (f.bits == 24);
    CHECK_FALSE (f.isFloat);
}

TEST_CASE ("interpretMixFormat: exclusive48kSupported passes through") {
    CHECK (eb::interpretMixFormat (3, 48000, 2, 32, false, true).exclusive48kSupported);
    CHECK_FALSE (eb::interpretMixFormat (3, 48000, 2, 32, false, false).exclusive48kSupported);
}

// ==================================================================================================
// #29 follow-up (cold-verifier MAJOR): eb::pickEndpointMatch — the PURE two-tier endpoint match the
// live Windows read keys on. The one-enumeration refactor dropped the exact-FriendlyName tier, so the
// FIRST endpoint whose name merely CONTAINED the caller's hint won, order-dependent ("Speakers" hint
// selecting "Speakers (USB DAC)"). These pin: exact (UID or whole-name, case-insensitive) ALWAYS beats
// contains regardless of enumeration order; contains stays as the fallback tier; no match = -1.
// ==================================================================================================
TEST_CASE ("pickEndpointMatch: a later exact name beats an earlier contains-match") {
    juce::StringArray uids  { "{uid-a}", "{uid-b}" };
    juce::StringArray names { "Speakers (USB DAC)", "Speakers" };
    CHECK (eb::pickEndpointMatch (uids, names, "Speakers") == 1);
}

TEST_CASE ("pickEndpointMatch: exact display-name match is case-insensitive") {
    juce::StringArray uids  { "{uid-a}", "{uid-b}" };
    juce::StringArray names { "CABLE Input (VB-Audio)", "speakers" };
    CHECK (eb::pickEndpointMatch (uids, names, "Speakers") == 1);
}

TEST_CASE ("pickEndpointMatch: UID match is exact tier") {
    juce::StringArray uids  { "{0.0.0.00000000}.{aaaa}", "{0.0.0.00000000}.{bbbb}" };
    juce::StringArray names { "Speakers hint-ish", "Other" };
    CHECK (eb::pickEndpointMatch (uids, names, "{0.0.0.00000000}.{bbbb}") == 1);
}

TEST_CASE ("pickEndpointMatch: contains is the fallback when no exact name exists") {
    juce::StringArray uids  { "{uid-a}", "{uid-b}" };
    juce::StringArray names { "Line Out", "CABLE Input (VB-Audio Virtual Cable)" };
    CHECK (eb::pickEndpointMatch (uids, names, "CABLE Input") == 1);
}

TEST_CASE ("pickEndpointMatch: first contains-match wins within the fallback tier") {
    juce::StringArray uids  { "{uid-a}", "{uid-b}" };
    juce::StringArray names { "A CABLE x", "B CABLE y" };
    CHECK (eb::pickEndpointMatch (uids, names, "CABLE") == 0);
}

TEST_CASE ("pickEndpointMatch: no match returns -1") {
    juce::StringArray uids  { "{uid-a}" };
    juce::StringArray names { "Speakers" };
    CHECK (eb::pickEndpointMatch (uids, names, "Headphones") == -1);
    CHECK (eb::pickEndpointMatch ({}, {}, "anything") == -1);
}
