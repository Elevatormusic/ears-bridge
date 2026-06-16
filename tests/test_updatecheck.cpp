#include <catch2/catch_test_macros.hpp>
#include "net/UpdateChecker.h"

TEST_CASE ("UpdateCheck isNewer compares versions numerically", "[update]") {
    CHECK (eb::isNewer ("0.2.12", "v0.2.13") == true);
    CHECK (eb::isNewer ("0.2.12", "v0.2.12") == false);
    CHECK (eb::isNewer ("0.2.12", "v0.2.11") == false);
    CHECK (eb::isNewer ("0.2.9",  "v0.2.10") == true);   // numeric, not lexical
    CHECK (eb::isNewer ("0.2.12", "v0.3.0")  == true);
    CHECK (eb::isNewer ("0.2.12", "1.0.0")   == true);   // no 'v' prefix
    CHECK (eb::isNewer ("0.2.12", "v0.2.13-beta") == true);   // suffix ignored
    CHECK (eb::isNewer ("0.2.12", "garbage") == false);
    CHECK (eb::isNewer ("0.2.12", "")        == false);
}

TEST_CASE ("UpdateCheck parseRelease reads tag and url", "[update]") {
    const juce::String body =
        R"({"tag_name":"v0.2.13","html_url":"https://github.com/Elevatormusic/ears-bridge/releases/tag/v0.2.13"})";

    SECTION ("newer release") {
        auto info = eb::parseRelease (body, "0.2.12");
        CHECK (info.reachedServer);
        CHECK (info.updateAvailable);
        CHECK (info.latestVersion == "0.2.13");
        CHECK (info.releaseUrl == "https://github.com/Elevatormusic/ears-bridge/releases/tag/v0.2.13");
    }
    SECTION ("same version -> no update") {
        auto info = eb::parseRelease (body, "0.2.13");
        CHECK (info.reachedServer);
        CHECK_FALSE (info.updateAvailable);
    }
    SECTION ("rate-limit error JSON (no tag_name)") {
        auto info = eb::parseRelease (R"({"message":"API rate limit exceeded"})", "0.2.12");
        CHECK (info.reachedServer);
        CHECK_FALSE (info.updateAvailable);
    }
    SECTION ("garbage body") {
        auto info = eb::parseRelease ("not json", "0.2.12");
        CHECK (info.reachedServer);
        CHECK_FALSE (info.updateAvailable);
    }
}
