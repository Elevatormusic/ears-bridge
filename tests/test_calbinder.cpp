#include <catch2/catch_test_macros.hpp>
#include "audio/CalBinder.h"

static eb::DeviceId makeDev (juce::String name, juce::String uid, eb::EarsModel m) {
    eb::DeviceId d;
    d.typeName = "Windows Audio";
    d.name = name; d.uid = uid; d.model = m;
    return d;
}

TEST_CASE("CalBinder re-binds cal after replug when key + model match") {
    eb::CalBinder b;
    auto dev = makeDev ("EARS Pro", "usb-vid1234-pid5678", eb::EarsModel::EarsPro);
    b.bind (dev, "/cals/L_HPN.txt", "/cals/R_HPN.txt");

    // Re-enumeration yields an equal DeviceId (same name/uid -> same key()).
    auto again = makeDev ("EARS Pro", "usb-vid1234-pid5678", eb::EarsModel::EarsPro);
    REQUIRE(b.hasValidBinding (again));
    auto rb = b.rebind (again);
    REQUIRE(rb.has_value());
    CHECK(rb->leftCalPath  == juce::String ("/cals/L_HPN.txt"));
    CHECK(rb->rightCalPath == juce::String ("/cals/R_HPN.txt"));
    CHECK(rb->model == eb::EarsModel::EarsPro);
}

TEST_CASE("CalBinder refuses a stale binding when the model changed for the same key") {
    eb::CalBinder b;
    auto pro = makeDev ("EARS", "shared-uid", eb::EarsModel::EarsPro);
    b.bind (pro, "/cals/L.txt", "/cals/R.txt");
    // Same name+uid (same key) but now reports as the original EARS -> do not reuse.
    auto orig = makeDev ("EARS", "shared-uid", eb::EarsModel::Ears);
    CHECK_FALSE(b.hasValidBinding (orig));
    CHECK_FALSE(b.rebind (orig).has_value());
}

TEST_CASE("CalBinder returns nullopt for an unknown device") {
    eb::CalBinder b;
    auto dev = makeDev ("Some Mic", "uid-x", eb::EarsModel::Unknown);
    CHECK_FALSE(b.rebind (dev).has_value());
}

TEST_CASE("CalBinder bind overwrites the previous association for the same key") {
    eb::CalBinder b;
    auto dev = makeDev ("EARS", "uid-1", eb::EarsModel::Ears);
    b.bind (dev, "/old/L.txt", "/old/R.txt");
    b.bind (dev, "/new/L.txt", "/new/R.txt");
    CHECK(b.size() == 1);
    CHECK(b.rebind (dev)->leftCalPath == juce::String ("/new/L.txt"));
}

TEST_CASE("CalBinder forget removes a binding") {
    eb::CalBinder b;
    auto dev = makeDev ("EARS", "uid-1", eb::EarsModel::Ears);
    b.bind (dev, "/L.txt", "/R.txt");
    b.forget (dev);
    CHECK(b.size() == 0);
    CHECK_FALSE(b.rebind (dev).has_value());
}
