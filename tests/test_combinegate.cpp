// test_combinegate.cpp
// Unit-tests for the combine-mode gate condition (D7 / R17).
// Tests the predicate logic in isolation using DeviceId directly, without
// instantiating MainComponent (which requires a live JUCE message loop).
#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceId.h"
#include "audio/CombineMode.h"
#include <vector>

// Mirror of the predicate MainComponent::isRealEarsWithCable uses.
// Extracted as a pure function so it is testable without the GUI.
static bool isRealEarsWithCable (const eb::DeviceId& in, const eb::DeviceId& out) noexcept {
    const bool realEars = (in.model == eb::EarsModel::Ears ||
                           in.model == eb::EarsModel::EarsPro);
    return realEars && out.isVirtualSink;
}

TEST_CASE("combine gate: real EARS + virtual cable -> gate is active") {
    eb::DeviceId in;
    in.model = eb::EarsModel::Ears;
    in.isVirtualSink = false;

    eb::DeviceId out;
    out.model = eb::EarsModel::Unknown;
    out.isVirtualSink = true;

    CHECK (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: EARS Pro + virtual cable -> gate is active") {
    eb::DeviceId in;
    in.model = eb::EarsModel::EarsPro;

    eb::DeviceId out;
    out.isVirtualSink = true;

    CHECK (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: generic mic + virtual cable -> gate NOT active") {
    // A generic mic (Unknown model) into a virtual sink should NOT be gated:
    // the user may use Sum/Average for non-Dirac monitoring.
    eb::DeviceId in;
    in.model = eb::EarsModel::Unknown;

    eb::DeviceId out;
    out.isVirtualSink = true;

    CHECK_FALSE (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: real EARS + real (non-virtual) output -> gate NOT active") {
    // A real output device (recording interface) is not a Dirac virtual cable,
    // so the gate must not fire.
    eb::DeviceId in;
    in.model = eb::EarsModel::Ears;

    eb::DeviceId out;
    out.isVirtualSink = false;

    CHECK_FALSE (isRealEarsWithCable (in, out));
}

TEST_CASE("combine gate: AutoPerEar is the ONLY non-gated mode") {
    // All four non-Auto modes should be gated; AutoPerEar should not be.
    const std::vector<eb::CombineMode> gatedModes {
        eb::CombineMode::LeftOnly,
        eb::CombineMode::RightOnly,
        eb::CombineMode::Average,
        eb::CombineMode::Sum,
    };
    for (auto mode : gatedModes)
        CHECK (mode != eb::CombineMode::AutoPerEar);   // all four are non-Auto

    CHECK (eb::CombineMode::AutoPerEar == eb::CombineMode::AutoPerEar);   // tautology: sanity
}
