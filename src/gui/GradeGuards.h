#pragma once
#include "gui/SnrStatus.h"   // kMinSweepSnrDb (the provisional sweep-SNR floor)
#include <cmath>

namespace eb {

// #32: the two grade-path predicates MainComponent applies around the reference-grade poll, extracted
// PURE so the suite exercises the PRODUCTION logic. The tests previously asserted hand-copied mirror
// lambdas ("mirrors the GUI poll EXACTLY" by convention only) — a drift in the real GUI condition would
// never fail a test. Now MainComponent and the tests call the SAME functions.

// Rate guard (pollReferenceGrade): grade only when both rates are known and agree within 1 Hz. A 44.1k
// response matched against a 48k reference is the SAME chirp stretched ~8.8% — it can still clear the
// provisional match cutoffs and read green "verified", which is a lie. Unknown (<= 0) rates pass: the
// guard can only act when both sides are actually known.
[[nodiscard]] inline bool rateAllowsGrade (double referenceRate, double responseRate) noexcept {
    return ! (referenceRate > 0.0 && responseRate > 0.0
              && std::abs (responseRate - referenceRate) > 1.0);
}

// LowSnr guidance (the completed-grade publish): flag iff the sweep-SNR is VALID and below the
// provisional floor. A silent / non-sweep ear never produces a valid SNR, so it can never raise LowSnr
// — GUIDANCE only, it does not invalidate the capture.
[[nodiscard]] inline bool wouldRaiseLowSnr (bool snrValid, float sweepSnrDb,
                                            float minDb = kMinSweepSnrDb) noexcept {
    return snrValid && sweepSnrDb < minDb;
}

} // namespace eb
