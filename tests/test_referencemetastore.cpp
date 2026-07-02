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

// ==================================================================================================
// #34: the OPTIONAL endpoint binding. Advisory only - it must round-trip when present, default empty
// when absent (legacy v0.4.0 sidecars), and NEVER gate validity (the integrity gate is rate+len+hash).
// ==================================================================================================
TEST_CASE("ReferenceMetaStore: the endpoint line round-trips and stays advisory [#34]") {
    eb::ReferenceMetadata l; l.rate = 48000.0; l.lengthSamples = 1000; l.contentHash = "aa11";
    eb::ReferenceMetadata r; r.rate = 48000.0; r.lengthSamples = 2000; r.contentHash = "bb22";

    const auto withEp = eb::serializeReferenceMeta (l, r, "{0.0.0.00000000}.{abcd-1234}");
    auto c = eb::checkReferenceMeta (withEp, l, r);
    CHECK (c.valid);
    CHECK (c.endpoint == "{0.0.0.00000000}.{abcd-1234}");

    // A display-name fallback with SPACES survives (the id is the line remainder, not one token).
    const auto named = eb::serializeReferenceMeta (l, r, "Speakers (VB-Audio Virtual Cable)");
    CHECK (eb::checkReferenceMeta (named, l, r).endpoint == "Speakers (VB-Audio Virtual Cable)");

    // LEGACY sidecar (no endpoint line, the v0.4.0 shape): still fully valid, endpoint empty.
    const auto legacy = eb::serializeReferenceMeta (l, r);
    CHECK_FALSE (legacy.contains ("endpoint"));
    c = eb::checkReferenceMeta (legacy, l, r);
    CHECK (c.valid);
    CHECK (c.endpoint.isEmpty());

    // The endpoint NEVER gates: a mismatched hash still fails for the hash, not the endpoint...
    eb::ReferenceMetadata bad = l; bad.contentHash = "ffff";
    CHECK_FALSE (eb::checkReferenceMeta (withEp, bad, r).valid);
    // ...and the integrity gate passing is unaffected by any endpoint content.
    CHECK (eb::checkReferenceMeta (withEp, l, r).valid);
}
