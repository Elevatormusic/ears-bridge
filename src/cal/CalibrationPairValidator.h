#pragma once
#include "cal/CalFile.h"
#include "cal/FirDesigner.h"   // eb::FirMode

namespace eb {

// Result of validating a left/right calibration pair for use as a Dirac correction.
// reason is empty iff valid.
struct CalPairResult {
    bool         valid = false;
    juce::String reason;
};

// Pure, GUI-free validator for a left/right calibration pair. Enforces (first failure
// wins; reason names the failing rule):
//   1. minimum data density   — each file >= 8 points
//   2. side not wrong         — left.side != Right, right.side != Left (Unknown allowed)
//   3. serial match           — if both serials non-empty they must be equal
//   4. type policy            — Heq blocked; Unknown blocked unless allowUnknownType
//   5. per-file integrity      — all freqs > 0 and STRICTLY increasing; all spl/phase finite
//   6. range coverage         — minFreqHz <= requiredLowHz and maxFreqHz >= requiredHighHz
//
// Rule 5 runs BEFORE Rule 6 on purpose: CalFile::minFreqHz()/maxFreqHz() read front()/back()
// and are only trustworthy on a strictly-increasing file (the Task-1 parser is permissive and
// loads a non-monotonic file with a warning), so the range check must not run until strict
// increase has passed.
CalPairResult validateCalibrationPair (const CalFile& left,
                                       const CalFile& right,
                                       FirMode        mode,
                                       double         requiredLowHz     = 20.0,
                                       double         requiredHighHz    = 20000.0,
                                       bool           allowUnknownType  = false);

} // namespace eb
