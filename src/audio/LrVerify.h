#pragma once
#include <juce_core/juce_core.h>

namespace eb {

enum class Ear { Left = 0, Right = 1 };

enum class LrResult {
    Pending,        // not enough evidence yet
    Pass,           // the channel under test is the one that responded
    Swapped,        // the OTHER channel responded -> channels are physically swapped
    Ambiguous,      // both channels responded (crosstalk/leak) or neither did
    Silent          // neither channel responded above the floor
};

// Pure state machine. Caller feeds per-block peak levels for both mic channels while a tone is
// driven into ONE earcup (the "ear under test"); the machine accumulates evidence and decides.
class LrVerify {
public:
    static constexpr float  kActiveLinear   = 0.02f;  // ~ -34 dBFS: a channel is "responding"
    static constexpr double kRatioToConfirm  = 4.0;    // live ear must be >= 4x the other to be unambiguous
    static constexpr int    kBlocksToConfirm = 16;     // sustained blocks before deciding

    void begin (Ear earUnderTest) noexcept;            // resets accumulators
    void observe (float peakLeft, float peakRight) noexcept;
    LrResult result() const noexcept;
    Ear      earUnderTest() const noexcept { return tested; }
    bool     isComplete() const noexcept { return result() != LrResult::Pending; }
    void     reset() noexcept;

private:
    Ear    tested = Ear::Left;
    int    blocks = 0;
    double sumLeft = 0.0, sumRight = 0.0;   // accumulated peak energy proxy
    int    activeBlocksLeft = 0, activeBlocksRight = 0;
};

} // namespace eb
