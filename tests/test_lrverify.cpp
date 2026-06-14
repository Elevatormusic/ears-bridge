#include <catch2/catch_test_macros.hpp>
#include "audio/LrVerify.h"

static void feed (eb::LrVerify& v, float l, float r, int n) {
    for (int i = 0; i < n; ++i) v.observe (l, r);
}

TEST_CASE("LrVerify: tone into LEFT, left mic responds -> Pass") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.30f, 0.005f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.isComplete());
    CHECK(v.result() == eb::LrResult::Pass);
}

TEST_CASE("LrVerify: tone into LEFT but RIGHT mic responds -> Swapped") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.005f, 0.30f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Swapped);
}

TEST_CASE("LrVerify: both channels loud -> Ambiguous") {
    eb::LrVerify v; v.begin (eb::Ear::Right);
    feed (v, 0.30f, 0.28f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Ambiguous);
}

TEST_CASE("LrVerify: neither channel responds -> Silent") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.001f, 0.001f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Silent);
}

TEST_CASE("LrVerify: not enough blocks -> Pending") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.30f, 0.005f, eb::LrVerify::kBlocksToConfirm - 1);
    CHECK(v.result() == eb::LrResult::Pending);
    CHECK_FALSE(v.isComplete());
}

TEST_CASE("LrVerify: tone into RIGHT, right mic responds -> Pass") {
    eb::LrVerify v; v.begin (eb::Ear::Right);
    feed (v, 0.004f, 0.40f, eb::LrVerify::kBlocksToConfirm);
    CHECK(v.result() == eb::LrResult::Pass);
}

TEST_CASE("LrVerify: reset clears prior evidence") {
    eb::LrVerify v; v.begin (eb::Ear::Left);
    feed (v, 0.30f, 0.005f, eb::LrVerify::kBlocksToConfirm);
    REQUIRE(v.result() == eb::LrResult::Pass);
    v.reset();
    CHECK(v.result() == eb::LrResult::Pending);
}
