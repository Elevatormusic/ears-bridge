#include <catch2/catch_test_macros.hpp>
#include "audio/AsioFallback.h"

static eb::DeviceTypeCaps caps (juce::String name, bool sep) { return { name, sep }; }

TEST_CASE("AsioFallback: ASIO (no separate I/O) forces a WASAPI fallback") {
    juce::Array<eb::DeviceTypeCaps> avail {
        caps ("ASIO", false),
        caps ("Windows Audio", true),            // WASAPI
        caps ("DirectSound", true)
    };
    auto d = eb::AsioFallback::decide (caps ("ASIO", false), avail);
    CHECK(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("Windows Audio"));
    CHECK(d.message.isNotEmpty());
    CHECK(d.message.containsIgnoreCase ("ASIO"));
}

TEST_CASE("AsioFallback: WASAPI is kept as-is (no fallback)") {
    juce::Array<eb::DeviceTypeCaps> avail {
        caps ("Windows Audio", true), caps ("ASIO", false)
    };
    auto d = eb::AsioFallback::decide (caps ("Windows Audio", true), avail);
    CHECK_FALSE(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("Windows Audio"));
    CHECK(d.message.isEmpty());
}

TEST_CASE("AsioFallback: CoreAudio is kept as-is on macOS") {
    juce::Array<eb::DeviceTypeCaps> avail { caps ("CoreAudio", true) };
    auto d = eb::AsioFallback::decide (caps ("CoreAudio", true), avail);
    CHECK_FALSE(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("CoreAudio"));
}

TEST_CASE("AsioFallback: any type lacking separate I/O triggers a fallback even if not named ASIO") {
    juce::Array<eb::DeviceTypeCaps> avail {
        caps ("WeirdDriver", false), caps ("Windows Audio", true)
    };
    auto d = eb::AsioFallback::decide (caps ("WeirdDriver", false), avail);
    CHECK(d.mustFallback);
    CHECK(d.chosenTypeName == juce::String ("Windows Audio"));
}

TEST_CASE("AsioFallback: fallback with no bridge-capable type available keeps the choice but warns") {
    juce::Array<eb::DeviceTypeCaps> avail { caps ("ASIO", false) };
    auto d = eb::AsioFallback::decide (caps ("ASIO", false), avail);
    CHECK(d.mustFallback);
    // No WASAPI/CoreAudio present: chosenTypeName stays empty, message explains the problem.
    CHECK(d.chosenTypeName.isEmpty());
    CHECK(d.message.isNotEmpty());
}
