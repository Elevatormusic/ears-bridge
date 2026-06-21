#include <catch2/catch_test_macros.hpp>
#include "gui/StartNotes.h"

// Task 4 (#8): the bit-depth note must read as NORMAL info (not a yellow warning) and FIT one rail
// line. buildStartNotes splits the post-start notes into `warnings` (genuinely cautionary: a real
// resample, an unverifiable raw rail) and a single short `info` line (the 32-bit-float fact).

using eb::buildStartNotes;
using eb::StartNotes;
using eb::RawRailState;

namespace {
// A raw rail that is verified at the granted rate so rawRailNote stays silent unless a test wants it.
RawRailState verifiedRail (double rate) {
    RawRailState rr; rr.verified = true; rr.requestedRate = rate; rr.mixRate = rate; return rr;
}
}

TEST_CASE("buildStartNotes: a bit-depth difference becomes a SHORT normal info line, not a warning") {
    // granted 32-bit float vs a selected 24-bit preference -> the fact lives in info, fits one line.
    const auto n = buildStartNotes (48000.0, 48000.0, 32, 24, verifiedRail (48000.0));

    CHECK (n.info.isNotEmpty());
    // The info phrase must be COMPLETE (no trailing clause that would clip on a 14px rail line).
    CHECK_FALSE (n.info.containsIgnoreCase ("preference only"));
    CHECK_FALSE (n.info.containsIgnoreCase ("setting is a preference"));
    // Single-line budget for a ~230px rail at 12px.
    CHECK (n.info.length() <= 48);
    // It must read as normal, not a problem.
    CHECK (n.info.containsIgnoreCase ("normal"));
    CHECK (n.info.contains ("32-bit float"));

    // And it must NOT leak into the warnings (which stay yellow).
    for (const auto& w : n.warnings)
        CHECK_FALSE (w.containsIgnoreCase ("bit"));
    CHECK (n.warnings.isEmpty());
}

TEST_CASE("buildStartNotes: granted depth == selected depth -> no bit-depth note at all") {
    const auto n = buildStartNotes (48000.0, 48000.0, 24, 24, verifiedRail (48000.0));
    CHECK (n.info.isEmpty());            // nothing to say about depth
    CHECK (n.warnings.isEmpty());
}

TEST_CASE("buildStartNotes: an unknown granted depth (0) says nothing") {
    const auto n = buildStartNotes (48000.0, 48000.0, 0, 24, verifiedRail (48000.0));
    CHECK (n.info.isEmpty());
    CHECK (n.warnings.isEmpty());
}

TEST_CASE("buildStartNotes: a real resample is a WARNING, not info") {
    // granted rate differs from the selected rate -> the OS is resampling: genuinely cautionary.
    const auto n = buildStartNotes (44100.0, 48000.0, 24, 24, verifiedRail (44100.0));
    REQUIRE (n.warnings.size() >= 1);
    bool found = false;
    for (const auto& w : n.warnings)
        if (w.containsIgnoreCase ("resampled")) found = true;
    CHECK (found);
    // a resample is never demoted to the calm info line
    CHECK_FALSE (n.info.containsIgnoreCase ("resampled"));
}

TEST_CASE("buildStartNotes: an unverified raw rail stays in warnings (cautionary), not info") {
    RawRailState rr; rr.verified = false; rr.requestedRate = 48000.0; rr.mixRate = 0.0;
    const auto n = buildStartNotes (48000.0, 48000.0, 24, 24, rr);
    bool found = false;
    for (const auto& w : n.warnings)
        if (w.containsIgnoreCase ("Could not confirm the EARS endpoint rate")) found = true;
    CHECK (found);
    CHECK (n.info.isEmpty());           // raw-rail caveat is never demoted to calm info
}

TEST_CASE("buildStartNotes: bit-depth info and a resample warning coexist independently") {
    const auto n = buildStartNotes (44100.0, 48000.0, 32, 24, verifiedRail (44100.0));
    // resample warning present...
    bool resample = false;
    for (const auto& w : n.warnings)
        if (w.containsIgnoreCase ("resampled")) resample = true;
    CHECK (resample);
    // ...and the calm bit-depth info present + short
    CHECK (n.info.contains ("32-bit float"));
    CHECK (n.info.length() <= 48);
    // bit-depth never appears as a warning
    for (const auto& w : n.warnings)
        CHECK_FALSE (w.containsIgnoreCase ("bit"));
}
