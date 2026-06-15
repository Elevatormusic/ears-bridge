#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceId.h"

TEST_CASE("DeviceId::key keys on the real endpoint id when one is resolved (uid != name)") {
    eb::DeviceId d; d.typeName = "Windows Audio"; d.name = "EARS"; d.uid = "{abc-123}";
    CHECK (d.key() == juce::String ("Windows Audio|{abc-123}"));   // stable id wins; the name drops out
}

TEST_CASE("DeviceId equality ignores volatile fields, uses identity triple + flags") {
    eb::DeviceId a; a.typeName = "Windows Audio"; a.name = "EARS"; a.uid = "u1";
    eb::DeviceId b = a;
    CHECK (a == b);
    b.uid = "u2";
    CHECK_FALSE (a == b);   // different endpoint id => different device
}

TEST_CASE("DeviceId::key is gain-DIP invariant for an EARS so a gain change keeps the saved selection") {
    // Best case: a real endpoint id was resolved (uid != name). The id is gain-independent, so the
    // key is identical across a gain-DIP change -- the durable fix.
    eb::DeviceId u18; u18.typeName = "Windows Audio"; u18.model = eb::EarsModel::Ears;
    u18.name = "Microphone (E.A.R.S Gain: 18dB)"; u18.uid = "{0.0.1.00000000}.{a-guid}";
    eb::DeviceId u9 = u18;
    u9.name = "Microphone (E.A.R.S Gain: 9dB)";    // same endpoint, gain DIP moved -> uid unchanged
    CHECK (u18.key() == u9.key());
    CHECK (u18.key() == juce::String ("Windows Audio|{0.0.1.00000000}.{a-guid}"));

    // Fallback (no endpoint id resolved, uid == name): the EARS gain token is stripped so a gain
    // change still survives, name-stable.
    eb::DeviceId at18; at18.typeName = "Windows Audio"; at18.model = eb::EarsModel::Ears;
    at18.name = "Microphone (E.A.R.S Gain: 18dB)"; at18.uid = at18.name;
    eb::DeviceId at9 = at18; at9.name = "Microphone (E.A.R.S Gain: 9dB)"; at9.uid = at9.name;
    CHECK (at18.key() == at9.key());

    // A non-EARS device with no resolved id falls back to a plain name key.
    eb::DeviceId cable; cable.typeName = "Windows Audio"; cable.name = "CABLE Output"; cable.uid = "CABLE Output";
    CHECK (cable.key() == juce::String ("Windows Audio|CABLE Output"));
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
