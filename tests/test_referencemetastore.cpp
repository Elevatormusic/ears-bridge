#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/ReferenceMetaStore.h"
using Catch::Matchers::WithinAbs;

// Audit #5/#20: the reference metadata sidecar binds rate + per-channel length/hash to the .f32 pair, so a
// reload can never mislabel the rate (the old 48k hardcode) nor silently accept a truncated/mixed-generation
// pair. These drive the pure serialize/check core; the file IO lives in MainComponent.

namespace {
eb::ReferenceMetadata md (double rate, int len, const char* hash) {
    eb::ReferenceMetadata m; m.rate = rate; m.lengthSamples = len; m.contentHash = hash; return m;
}
}

TEST_CASE("ReferenceMetaStore: round-trips and returns the learn-time rate", "[refmeta]") {
    const auto L = md (96000.0, 480000, "aabbcc"), R = md (96000.0, 480123, "ddeeff");
    const auto c = eb::checkReferenceMeta (eb::serializeReferenceMeta (L, R), L, R);
    REQUIRE (c.valid);
    CHECK_THAT (c.rate, WithinAbs (96000.0, 1e-3));   // the REAL capture rate, not an assumed 48k
}

TEST_CASE("ReferenceMetaStore: a content-hash mismatch (tampered/mixed-generation channel) fails, naming the side", "[refmeta]") {
    const auto L = md (48000.0, 480000, "aabbcc"), R = md (48000.0, 480000, "ddeeff");
    const auto txt = eb::serializeReferenceMeta (L, R);
    auto c = eb::checkReferenceMeta (txt, L, md (48000.0, 480000, "00ff00"));   // R bytes differ on disk
    CHECK_FALSE (c.valid);
    CHECK (c.reason.containsIgnoreCase ("R"));
}

TEST_CASE("ReferenceMetaStore: a length mismatch (truncated write) fails", "[refmeta]") {
    const auto L = md (48000.0, 480000, "aabbcc"), R = md (48000.0, 480000, "ddeeff");
    const auto txt = eb::serializeReferenceMeta (L, R);
    CHECK_FALSE (eb::checkReferenceMeta (txt, md (48000.0, 123456, "aabbcc"), R).valid);
}

TEST_CASE("ReferenceMetaStore: absent/garbage/implausible-rate sidecars never validate", "[refmeta]") {
    const auto L = md (48000.0, 480000, "aabbcc"), R = md (48000.0, 480000, "ddeeff");
    CHECK_FALSE (eb::checkReferenceMeta ({},                                              L, R).valid);  // absent
    CHECK_FALSE (eb::checkReferenceMeta ("garbage",                                      L, R).valid);  // not v1
    CHECK_FALSE (eb::checkReferenceMeta ("v1\nrate nonsense\nL 480000 aabbcc\nR 480000 ddeeff\n", L, R).valid);
    CHECK_FALSE (eb::checkReferenceMeta ("v1\nrate 1.0\nL 480000 aabbcc\nR 480000 ddeeff\n",      L, R).valid);
}
