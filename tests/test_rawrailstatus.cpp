#include <catch2/catch_test_macros.hpp>
#include "gui/RawRailStatus.h"

TEST_CASE("rawRailNote is silent when verified, warns only when resampled/unverifiable") {
    using eb::RawRailState; using eb::rawRailNote;
    SECTION ("no run yet => empty") { CHECK (rawRailNote (RawRailState{}).isEmpty()); }
    SECTION ("verified => empty (no news is good news)") {
        RawRailState rr; rr.verified = true; rr.requestedRate = 48000.0; rr.mixRate = 48000.0;
        CHECK (rawRailNote (rr).isEmpty());
    }
    SECTION ("OS-resampled => approximate caveat naming both rates") {
        RawRailState rr; rr.verified = false; rr.requestedRate = 96000.0; rr.mixRate = 48000.0;
        CHECK (rawRailNote (rr)
               == juce::String ("OS-resampled to 48.0 kHz (endpoint mix rate) vs the requested 96.0 kHz "
                                "- clip detection approximate."));
    }
    SECTION ("unverifiable (mix rate unknown) => honest caveat") {
        RawRailState rr; rr.verified = false; rr.requestedRate = 48000.0; rr.mixRate = 0.0;
        CHECK (rawRailNote (rr)
               == juce::String ("Could not confirm the EARS endpoint rate - clip detection approximate."));
    }
}
