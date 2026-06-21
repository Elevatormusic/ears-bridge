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
