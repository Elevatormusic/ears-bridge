#include "cal/CalibrationPairValidator.h"
#include <cmath>   // std::isfinite

namespace eb {
namespace {

constexpr int kMinPoints = 8;

// Rule 5 per-file integrity: every freq > 0 and STRICTLY increasing, every spl/phase finite.
// Returns true if the file is sound; on failure sets `reason` naming the file + "frequencies".
bool fileIntegrityOk (const CalFile& f, const char* sideLabel, juce::String& reason)
{
    double prev = -1.0;   // first freq must be > 0, so any non-positive first value fails too
    for (const auto& p : f.points)
    {
        const bool freqOk  = std::isfinite (p.freqHz) && p.freqHz > 0.0 && p.freqHz > prev;
        const bool finiteOk = std::isfinite (p.splDb) && std::isfinite (p.phaseDeg);
        if (! freqOk || ! finiteOk)
        {
            reason = juce::String (sideLabel)
                   + " calibration has invalid/zero/duplicate frequencies "
                     "(values must be finite, positive and strictly increasing)";
            return false;
        }
        prev = p.freqHz;
    }
    return true;
}

} // namespace

CalPairResult validateCalibrationPair (const CalFile& left,
                                       const CalFile& right,
                                       FirMode        /*mode*/,
                                       double         requiredLowHz,
                                       double         requiredHighHz,
                                       bool           allowUnknownType)
{
    // Rule 1: minimum data density.
    if ((int) left.points.size() < kMinPoints)
        return { false, "Left calibration has too few points" };
    if ((int) right.points.size() < kMinPoints)
        return { false, "Right calibration has too few points" };

    // Rule 2: side not wrong (Unknown is allowed; only a declared opposite side fails).
    if (left.side == CalSide::Right)
        return { false, "Left calibration slot holds a file declaring the RIGHT side" };
    if (right.side == CalSide::Left)
        return { false, "Right calibration slot holds a file declaring the LEFT side" };

    // Rule 3: serial match if both are non-empty.
    if (left.serial.isNotEmpty() && right.serial.isNotEmpty()
        && left.serial != right.serial)
        return { false, "Left/Right serial mismatch (" + left.serial + " vs " + right.serial + ")" };

    // Rule 4: type policy. HEQ is miniDSP's RECOMMENDED Dirac headphone cal ("We suggest using the HEQ
    // calibration file" — miniDSP's Dirac-Live headphone note), so it is NOT blocked: a type-name veto of a
    // known-good type was wrong. HPN/RAW/HEQ all pass; only an UNIDENTIFIABLE type is gated (behind
    // allowUnknownType). The real reject basis is the STRUCTURAL integrity below (Rules 5-6).
    if (! allowUnknownType)
    {
        for (const auto* f : { &left, &right })
            if (f->type == CalType::Unknown)
                return { false, "Unknown calibration type" };
    }

    // Rule 5: per-file integrity — MUST run before Rule 6 so minFreqHz()/maxFreqHz()
    // (front()/back()) are trustworthy.
    {
        juce::String reason;
        if (! fileIntegrityOk (left,  "Left",  reason))  return { false, reason };
        if (! fileIntegrityOk (right, "Right", reason))  return { false, reason };
    }

    // Rule 6: range coverage. Both files are strictly increasing here, so front()/back()
    // give the true min/max. The combined pair must span the correction band.
    const double minFreq = juce::jmin (left.minFreqHz(),  right.minFreqHz());
    const double maxFreq = juce::jmax (left.maxFreqHz(),  right.maxFreqHz());
    if (minFreq > requiredLowHz || maxFreq < requiredHighHz)
        return { false, "Calibration does not cover the 20 Hz–20 kHz correction band" };

    return { true, {} };
}

} // namespace eb
