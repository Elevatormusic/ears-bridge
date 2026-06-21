#include "audio/ReferenceGradePoller.h"

#include <algorithm>   // std::min

namespace eb {

void ReferenceGradePoller::reset() {
    lastPollMatched_   = false;
    gradedThisSession_ = false;
}

GradePollResult ReferenceGradePoller::decide (const float* window, int winLen,
                                              const float* reference, int refLen) {
    GradePollResult r;

    // Guard: with no window or no reference there is nothing to grade against (mirrors the GUI's
    // loadedReference_.empty() / got<=0 early returns). The match-gate cannot run -> not matched, no grade.
    if (window == nullptr || reference == nullptr || winLen <= 0 || refLen <= 0)
        return r;

    // 1) The DETECTOR: cross-correlate the window against the learned reference. The coherence is published
    //    every poll (the GUI's engine.setLastMatchCoherence) so the diagnostic log + RefMon-change line see
    //    it — the match-gate ran here, FIRST, before any quality. There is NO absolute level gate.
    const int matchLen = std::min (refLen, winLen);
    const MatchVerdict m = referenceMatches (reference, window, matchLen);
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

GradePollResult ReferenceGradePoller::gradeWindow (const float* window, int winLen,
                                                   const float* reference, int refLen, double rate) {
    GradePollResult r;
    if (window == nullptr || reference == nullptr || winLen <= 0 || refLen <= 0)
        return r;

    // The pure grade: gradeMeasurementWindow locates the sweep inside the long window (align), re-runs the
    // match-gate FIRST on the aligned segment, then quality — so a non-sweep window returns ReferenceStale
    // (never a clean grade). Safe to call off-thread; touches no debounce state. This is the verdict the GUI
    // publishes via publishReferenceGrade.
    const MeasurementGrade g = gradeMeasurementWindow (reference, refLen, window, winLen, rate);
    r.didGrade   = true;
    r.state      = g.state;
    r.irSnrDb    = g.quality.irSnrDb;
    r.thdPercent = g.quality.thdPercent;
    r.mismatch   = (g.state == RefMonState::ReferenceStale);
    r.lowQuality = g.quality.lowQuality;
    return r;
}

GradePollResult ReferenceGradePoller::poll (const float* window, int winLen,
                                            const float* reference, int refLen, double rate) {
    // The full synchronous decision: match + debounce, then (only on the stable-match edge) the grade.
    GradePollResult r = decide (window, winLen, reference, refLen);
    if (! r.didGrade)
        return r;   // not the grading poll: coherence/matched are set, grade fields stay default

    // Carry the coherence/matched forward onto the graded result so a single poll() return is self-describing.
    GradePollResult g = gradeWindow (window, winLen, reference, refLen, rate);
    g.coherence = r.coherence;
    g.matched   = r.matched;
    return g;
}

} // namespace eb
