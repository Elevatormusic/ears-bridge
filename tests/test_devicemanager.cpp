#include <catch2/catch_test_macros.hpp>
#include "audio/DeviceManager.h"

TEST_CASE("preferredTypeName is WASAPI on Windows / CoreAudio on macOS") {
    auto t = eb::DeviceManager::preferredTypeName();
   #if JUCE_WINDOWS
    CHECK (t == juce::String ("Windows Audio"));
   #elif JUCE_MAC
    CHECK (t == juce::String ("CoreAudio"));
   #else
    CHECK (t.isNotEmpty());
   #endif
}

TEST_CASE("looksLikeVirtualSink tags known virtual cables, not real hardware") {
    using DM = eb::DeviceManager;
    CHECK (DM::looksLikeVirtualSink ("CABLE Input (VB-Audio Virtual Cable)"));
    CHECK (DM::looksLikeVirtualSink ("VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"));
    CHECK (DM::looksLikeVirtualSink ("BlackHole 2ch"));
    CHECK (DM::looksLikeVirtualSink ("Loopback Audio"));
    CHECK_FALSE (DM::looksLikeVirtualSink ("Speakers (Realtek High Definition Audio)"));
    CHECK_FALSE (DM::looksLikeVirtualSink ("miniDSP EARS"));
}

TEST_CASE("nativeRatesFor falls back to the model whitelist for an unopened device") {
    eb::DeviceManager dm;
    eb::DeviceId pro; pro.name = "miniDSP EARS Pro"; pro.model = eb::EarsModel::EarsPro;
    auto rates = dm.nativeRatesFor (pro);
    CHECK (rates == std::vector<double>{44100,48000,88200,96000,176400,192000});
    auto bits = dm.nativeBitDepthsFor (pro);
    CHECK (bits == std::vector<int>{16,24,32});

    eb::DeviceId ears; ears.name = "miniDSP EARS"; ears.model = eb::EarsModel::Ears;
    CHECK (dm.nativeRatesFor (ears) == std::vector<double>{48000});
    CHECK (dm.nativeBitDepthsFor (ears) == std::vector<int>{24});
}
