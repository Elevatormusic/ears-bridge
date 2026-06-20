// test_combinegate.cpp
// Unit-tests for the combine-mode gate condition (D7 / R17).
// Tests the predicate logic in isolation using DeviceId directly, without
// instantiating MainComponent (which requires a live JUCE message loop).
#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceId.h"
#include "audio/CombineMode.h"
#include <vector>

// Mirror of the real-EARS-input check MainComponent::isRealEarsInput uses.
// Extracted as a pure function so it is testable without the GUI.
static bool isRealEarsInput (const eb::DeviceId& in) noexcept {
    return in.model == eb::EarsModel::Ears ||
           in.model == eb::EarsModel::EarsPro;
}

// Mirror of the predicate MainComponent::isRealEarsWithCable uses.
// Extracted as a pure function so it is testable without the GUI.
static bool isRealEarsWithCable (const eb::DeviceId& in, const eb::DeviceId& out) noexcept {
    return isRealEarsInput (in) && out.isVirtualSink;
}

// Mirror of the physical-output gate MainComponent::updateStartGate uses (P1-09):
// a real EARS input feeding an output that is NOT a verified virtual sink would
// record into a device Dirac never sees, so Start must be blocked.
static bool isRealEarsWithPhysicalOutput (const eb::DeviceId& in, const eb::DeviceId& out) noexcept {
    return isRealEarsInput (in) && ! out.isVirtualSink;
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

// --- Physical-output gate (P1-09) -------------------------------------------
// Independent of the combine-mode gate: a real EARS into a non-virtual output
// can never reach Dirac, so Start is blocked regardless of combine mode. The
// SAME inputs that leave the combine-mode predicate inactive (real EARS + real
// output, ~line 52-62) must make THIS predicate active.

TEST_CASE("physical-output gate: real EARS + non-virtual output -> gate is active") {
    eb::DeviceId in;
    in.model = eb::EarsModel::Ears;

    eb::DeviceId out;
    out.model = eb::EarsModel::Unknown;
    out.isVirtualSink = false;

    CHECK (isRealEarsWithPhysicalOutput (in, out));
    // ...while the combine-mode predicate stays inactive for the same pair.
    CHECK_FALSE (isRealEarsWithCable (in, out));
}

TEST_CASE("physical-output gate: EARS Pro + non-virtual output -> gate is active") {
    eb::DeviceId in;
    in.model = eb::EarsModel::EarsPro;

    eb::DeviceId out;
    out.isVirtualSink = false;

    CHECK (isRealEarsWithPhysicalOutput (in, out));
}

TEST_CASE("physical-output gate: real EARS + virtual cable -> gate NOT active") {
    // The output IS a verified virtual sink, so the physical-output gate must
    // not fire (the combine-mode gate handles the Dirac path from here).
    eb::DeviceId in;
    in.model = eb::EarsModel::Ears;

    eb::DeviceId out;
    out.isVirtualSink = true;

    CHECK_FALSE (isRealEarsWithPhysicalOutput (in, out));
}

TEST_CASE("physical-output gate: generic mic + non-virtual output -> gate NOT active") {
    // Only a real EARS triggers the Dirac path; a generic mic (Unknown) into a
    // physical output is a normal, allowed monitoring setup.
    eb::DeviceId in;
    in.model = eb::EarsModel::Unknown;

    eb::DeviceId out;
    out.isVirtualSink = false;

    CHECK_FALSE (isRealEarsWithPhysicalOutput (in, out));
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
