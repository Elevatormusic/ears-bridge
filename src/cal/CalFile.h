#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <stdexcept>
namespace eb {
enum class CalType { Unknown, Raw, Hpn, Heq, Idf };
struct CalPoint { double freqHz; double splDb; double phaseDeg; };
struct CalParseError : std::runtime_error { using std::runtime_error::runtime_error; };
struct CalFile {
    juce::String serial;
    CalType type = CalType::Unknown;
    std::vector<CalPoint> points;
    static CalFile parse (const juce::String& text);   // throws CalParseError
};
}
