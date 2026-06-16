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
