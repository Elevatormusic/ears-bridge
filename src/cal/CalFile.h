#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <stdexcept>
namespace eb {
enum class CalType { Unknown, Raw, Hpn, Heq, Idf };
enum class CalSide { Unknown, Left, Right };
struct CalPoint { double freqHz; double splDb; double phaseDeg; };
struct CalParseError : std::runtime_error { using std::runtime_error::runtime_error; };
struct CalFile {
    juce::String serial;
    CalType type = CalType::Unknown;
    CalSide side = CalSide::Unknown;
    juce::String contentHash;
    juce::StringArray parseWarnings;
    std::vector<CalPoint> points;

    // points are stored in ascending freq order (file order); the parser flags
    // any out-of-order row in parseWarnings, so front()/back() give the range.
    double minFreqHz() const { return points.empty() ? 0.0 : points.front().freqHz; }
    double maxFreqHz() const { return points.empty() ? 0.0 : points.back().freqHz; }

    static CalFile parse (const juce::String& text);   // throws CalParseError on no data rows
};

// Second ear-side signal (the file CONTENT is the first, read by parse()): derive Left/Right from
// the file NAME. Matches the words "LEFT"/"RIGHT" first, else a DELIMITED single-letter L/R token
// (leading "L"/"R" before _ - . or space; trailing; or "_L_"-style). Ambiguous (both sides, or a
// bare letter buried in a word) or no marker -> Unknown. Pure + dependency-free. Used by the cal
// slot to fill an Unknown content side and to cross-check a content-marked file (content wins).
CalSide sideFromFilename (const juce::String& fileName);
}
