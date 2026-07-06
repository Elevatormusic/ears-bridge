#include <catch2/catch_test_macros.hpp>
#include "gui/DeviceNameDistill.h"

TEST_CASE("P2.9 distill: known device families collapse to W2-style short names") {
    CHECK (eb::distillDeviceName ("Microphone (E.A.R.S. Gain: 18dB)")            == "EARS");
    CHECK (eb::distillDeviceName ("miniDSP E.A.R.S. Gain: 18dB")                 == "EARS");
    CHECK (eb::distillDeviceName ("Hi-Fi Cable Output (VB-Audio Hi-Fi Cable)")   == "Hi-Fi Cable");
    CHECK (eb::distillDeviceName ("Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)")    == "Hi-Fi Cable");
    CHECK (eb::distillDeviceName ("CABLE Input (VB-Audio Virtual Cable)")        == "VB-Cable");
    CHECK (eb::distillDeviceName ("Voicemeeter Input (VB-Audio Voicemeeter VAIO)") == "Voicemeeter");
}

TEST_CASE("P2.9 distill: fallback takes the parenthetical, caps at 24 chars, never returns empty") {
    CHECK (eb::distillDeviceName ("Speakers (Realtek(R) Audio)")   == "Realtek(R) Audio");
    CHECK (eb::distillDeviceName ("Realtek Digital Output")        == "Realtek Digital Output");
    CHECK (eb::distillDeviceName ("Headphones (SteelSeries Arctis Nova Pro Wireless Gaming DAC)")
           == "SteelSeries Arctis Nov..");
    CHECK (eb::distillDeviceName ("")                              == juce::String());
    CHECK (eb::distillDeviceName ("   ")                           == juce::String());
}
