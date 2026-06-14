#include "audio/LrVerify.h"

namespace eb {

void LrVerify::begin (Ear earUnderTest) noexcept {
    tested = earUnderTest;
    reset();
    tested = earUnderTest;   // reset() leaves tested at Left; restore the requested ear
}

void LrVerify::reset() noexcept {
    tested = Ear::Left;
    blocks = 0;
    sumLeft = sumRight = 0.0;
    activeBlocksLeft = activeBlocksRight = 0;
}

void LrVerify::observe (float peakLeft, float peakRight) noexcept {
    ++blocks;
    sumLeft  += static_cast<double> (peakLeft);
    sumRight += static_cast<double> (peakRight);
    if (peakLeft  >= kActiveLinear) ++activeBlocksLeft;
    if (peakRight >= kActiveLinear) ++activeBlocksRight;
}

LrResult LrVerify::result() const noexcept {
    if (blocks < kBlocksToConfirm) return LrResult::Pending;

    const bool leftActive  = activeBlocksLeft  >= kBlocksToConfirm / 2;
    const bool rightActive = activeBlocksRight >= kBlocksToConfirm / 2;

    if (! leftActive && ! rightActive) return LrResult::Silent;

    // Both clearly active without a dominant channel -> ambiguous (crosstalk / both driven).
    const double eps = 1.0e-9;
    const double lr  = sumLeft  / (sumRight + eps);
    const double rl  = sumRight / (sumLeft  + eps);

    if (leftActive && rightActive && lr < kRatioToConfirm && rl < kRatioToConfirm)
        return LrResult::Ambiguous;

    // Determine which channel dominates.
    const Ear dominant = (sumLeft >= sumRight) ? Ear::Left : Ear::Right;
    const double dominance = (dominant == Ear::Left) ? lr : rl;
    if (dominance < kRatioToConfirm) return LrResult::Ambiguous;

    return (dominant == tested) ? LrResult::Pass : LrResult::Swapped;
}

} // namespace eb
