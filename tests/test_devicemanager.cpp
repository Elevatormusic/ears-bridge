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

TEST_CASE("classifyVirtualSink distinguishes the std cable, Hi-Fi cable, and other sinks") {
    using DM = eb::DeviceManager;
    using VK = eb::DeviceManager::VirtualSinkKind;
    // Hi-Fi must win over the generic "cable"/"vb-audio" std match (it also contains them).
    CHECK (DM::classifyVirtualSink ("Hi-Fi Cable Output (VB-Audio Hi-Fi Cable)") == VK::HiFiCable);
    // Standard VB-CABLE and a renamed VB variant both classify as the std cable (600007 path).
    CHECK (DM::classifyVirtualSink ("CABLE Output (VB-Audio Virtual Cable)")     == VK::StdVbCable);
    CHECK (DM::classifyVirtualSink ("CABLE-A Output (VB-Audio Cable A)")         == VK::StdVbCable);
    // Other virtual sinks get the soft hint, not silence.
    CHECK (DM::classifyVirtualSink ("VoiceMeeter Out (VB-Audio VoiceMeeter VAIO)") == VK::OtherVirtual);
    CHECK (DM::classifyVirtualSink ("BlackHole 2ch")                              == VK::OtherVirtual);
    CHECK (DM::classifyVirtualSink ("Speakers (Realtek High Definition Audio)")   == VK::NotVirtual);
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

TEST_CASE("DeviceManager::rawRailMatches is a pure no-SRC predicate") {
    using eb::DeviceManager;
    CHECK (DeviceManager::rawRailMatches (48000.0, 48000.0));   // requested == mix -> no SRC
    CHECK (DeviceManager::rawRailMatches (48000.0, 48000.4));   // within 0.5 Hz
    CHECK_FALSE (DeviceManager::rawRailMatches (96000.0, 48000.0)); // OS resamples 96k<->48k
    CHECK_FALSE (DeviceManager::rawRailMatches (48000.0, 0.0));     // mix rate unresolved -> not verified
    CHECK_FALSE (DeviceManager::rawRailMatches (0.0, 0.0));
}

TEST_CASE("DeviceManager raw-rail read-back is zeroed before open and after closeAll") {
    eb::DeviceManager dm;
    CHECK (dm.requestedInputSampleRate() == 0.0);
    CHECK (dm.endpointMixSampleRate()    == 0.0);
    CHECK_FALSE (dm.rawRailVerified());

    eb::DeviceId nope; nope.name = "no-such-input-device-xyz";
    auto err = dm.openInput (nope, 48000.0, 512);   // headless: device can't be created
    CHECK (err.isNotEmpty());
    CHECK (dm.requestedInputSampleRate() == 48000.0);   // request recorded even on failure
    CHECK (dm.endpointMixSampleRate()    == 0.0);        // nothing resolved
    CHECK_FALSE (dm.rawRailVerified());

    dm.closeAll();
    CHECK (dm.requestedInputSampleRate() == 0.0);
    CHECK (dm.endpointMixSampleRate()    == 0.0);
    CHECK_FALSE (dm.rawRailVerified());
}
