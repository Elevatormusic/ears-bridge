#include "audio/ReferenceGradePoller.h"

#include <algorithm>   // std::min
#include <cmath>       // std::isfinite

namespace eb {

void ReferenceGradePoller::reset() {
    lastPollMatched_   = false;
    gradedThisSession_ = false;
}

namespace {
// A window that carries any non-finite (NaN/Inf) sample must NOT match: those propagate into the FFT and the
// match-gate returns coherence=1.0 when the off-lobe RMS collapses to zero (Deconvolver.cpp restRms==0 branch),
// so a degenerate window could FALSE-match. The grade ring is fed the RAW active-ear capture (pre-sanitization),
// so this is the poller's own guard. Cheap linear scan, off the audio thread.
bool windowIsFinite (const float* window, int n) noexcept {
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (window[i])) return false;
    return true;
}
} // namespace

GradePollResult ReferenceGradePoller::decide (const float* window, int winLen,
                                              const float* reference, int refLen) {
    GradePollResult r;

    // Guard: with no window or no reference there is nothing to grade against (mirrors the GUI's
    // loadedReference_.empty() / got<=0 early returns). The match-gate cannot run -> not matched, no grade.
    if (window == nullptr || reference == nullptr || winLen <= 0 || refLen <= 0)
        return r;

    // Fix 3 (safety): a NaN/Inf-bearing window must NOT match. referenceMatches can read coherence=1.0 from a
    // degenerate window (restRms==0), so reject non-finite content up front -> not matched, no grade, no crash.
    if (! windowIsFinite (window, winLen)) {
        r.matched = false;
        r.coherence = 0.0f;
        gradedThisSession_ = false;   // a non-match re-arms the session (same as a dropped match below)
        lastPollMatched_   = false;
        return r;
    }

    // 1) The DETECTOR. Fix 1 (align): the ring snapshot is OLDEST->NEWEST, so a just-finished sweep sits at the
    //    END of the long window — OUTSIDE the reference-length PREFIX the old gate read (it matched window[0..],
    //    coherence ~= 0 for a late sweep -> never graded, intermittently). Instead, FIRST cross-correlate the
    //    reference against the WHOLE window to LOCATE the sweep (exactly as gradeMeasurementWindow does), then run
    //    referenceMatches over the ALIGNED reference-length segment. The gate and the grader now agree on where
    //    the sweep is. The located offset is carried in r.alignOffset so the grade reuses it (no second xcorr).
    //    The coherence is published every poll (engine.setLastMatchCoherence) so the diagnostic log sees it.
    //    There is NO absolute level gate.
    int matchStart = 0, matchLen = std::min (refLen, winLen);
    if (winLen >= refLen) {
        const AlignResult a = crossCorrelateAlign (reference, refLen, window, winLen);
        matchStart = clampSweepStart (a.delaySamples, refLen, winLen);
        matchLen   = std::min (refLen, winLen - matchStart);
    }
    r.alignOffset = matchStart;
    const MatchVerdict m = referenceMatches (reference, window + matchStart, matchLen);
    r.coherence = m.coherence;
    r.matched   = m.matched;

    // 2) STABLE-MATCH debounce (exactly MainComponent::pollReferenceGrade): the match-gate runs FIRST. A
    //    non-match re-arms the session so the next sweep can grade; a grade fires once when the match holds
    //    across TWO consecutive throttled polls (so the sweep is fully captured, not still rising into the ring).
    if (! m.matched) gradedThisSession_ = false;             // match dropped -> a fresh sweep may grade
    const bool stableMatch = m.matched && lastPollMatched_;   // two matched polls in a row
    lastPollMatched_ = m.matched;
    if (! stableMatch || gradedThisSession_)                 // need a stable, not-yet-graded match
        return r;
    gradedThisSession_ = true;                               // latch: don't re-grade this match session

    r.didGrade = true;   // the stable-match edge: a grade SHOULD run for this window now
    return r;
}

namespace {
// Pack a MeasurementGrade into the poll-result grade fields (shared by both gradeWindow overloads).
GradePollResult packGrade (const MeasurementGrade& g) {
    GradePollResult r;
    r.didGrade   = true;
    r.state      = g.state;
    r.irSnrDb    = g.quality.irSnrDb;
    r.thdPercent = g.quality.thdPercent;
    r.mismatch   = (g.state == RefMonState::ReferenceStale);
    r.lowQuality = g.quality.lowQuality;
    return r;
}
} // namespace

GradePollResult ReferenceGradePoller::gradeWindow (const float* window, int winLen,
                                                   const float* reference, int refLen, double rate) {
    GradePollResult r;
    if (window == nullptr || reference == nullptr || winLen <= 0 || refLen <= 0)
        return r;

    // The pure grade: gradeMeasurementWindow locates the sweep inside the long window (align), re-runs the
    // match-gate FIRST on the aligned segment, then quality — so a non-sweep window returns ReferenceStale
    // (never a clean grade). Safe to call off-thread; touches no debounce state. This is the verdict the GUI
    // publishes via publishReferenceGrade. (Used by the synchronous poll() and callers that did not run decide().)
    return packGrade (gradeMeasurementWindow (reference, refLen, window, winLen, rate));
}

GradePollResult ReferenceGradePoller::gradeWindow (const float* window, int winLen,
                                                   const float* reference, int refLen, double rate,
                                                   int alignOffset) {
    GradePollResult r;
    if (window == nullptr || reference == nullptr || winLen <= 0 || refLen <= 0)
        return r;
    // Grade at the offset decide() already located via cross-correlation: the gate matched the sweep THERE, so
    // the grade must quality-check the SAME segment. gradeMeasurementWindowAt re-runs the match-gate FIRST on
    // that segment (a non-sweep -> ReferenceStale), so this is just as honest as the self-aligning overload but
    // avoids a second (expensive) cross-correlation. The GUI worker path uses this.
    return packGrade (gradeMeasurementWindowAt (reference, refLen, window, winLen, alignOffset, rate));
}

GradePollResult ReferenceGradePoller::poll (const float* window, int winLen,
                                            const float* reference, int refLen, double rate) {
    // The full synchronous decision: match + debounce, then (only on the stable-match edge) the grade.
    GradePollResult r = decide (window, winLen, reference, refLen);
    if (! r.didGrade)
        return r;   // not the grading poll: coherence/matched are set, grade fields stay default

    // Grade at the SAME offset decide() located (Fix 1): reuse r.alignOffset so the gate and the grade agree on
    // where the sweep is, and the expensive cross-correlation runs once, not twice. Carry coherence/matched
    // forward onto the graded result so a single poll() return is self-describing.
    GradePollResult g = gradeWindow (window, winLen, reference, refLen, rate, r.alignOffset);
    g.coherence   = r.coherence;
    g.matched     = r.matched;
    g.alignOffset = r.alignOffset;
    return g;
}

} // namespace eb
