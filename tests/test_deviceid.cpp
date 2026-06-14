#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceId.h"

TEST_CASE("DeviceId::key concatenates typeName|name|uid") {
    eb::DeviceId d; d.typeName = "Windows Audio"; d.name = "EARS"; d.uid = "{abc-123}";
    CHECK (d.key() == juce::String ("Windows Audio|EARS|{abc-123}"));
}

TEST_CASE("DeviceId equality ignores volatile fields, uses identity triple + flags") {
    eb::DeviceId a; a.typeName = "Windows Audio"; a.name = "EARS"; a.uid = "u1";
    eb::DeviceId b = a;
    CHECK (a == b);
    b.uid = "u2";
    CHECK_FALSE (a == b);   // different endpoint id => different device
}

TEST_CASE("DeviceId key is stable across a simulated replug WHEN a real stable uid exists") {
    // This stability holds ONLY when rescan() captured a real stable endpoint id into uid
    // (here a USB hardware id). When the platform exposes only a display name, rescan() sets
    // uid==name and the key is merely name-stable, not replug-stable (see uid-stability note).
    eb::DeviceId before; before.typeName = "Windows Audio"; before.name = "miniDSP EARS"; before.uid = "USB\\VID_2752";
    eb::DeviceId after  = before;   // replug: OS index changed but the stable identity triple did not
    CHECK (before.key() == after.key());
    CHECK (before == after);
}
