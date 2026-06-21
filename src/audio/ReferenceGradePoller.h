#pragma once
#include <vector>

#include "audio/Deconvolver.h"   // eb::referenceMatches / MatchVerdict (the detector)
#include "audio/RefMonitor.h"    // eb::gradeMeasurementWindow / RefMonState / MeasurementGrade

// Reference-Based Measurement Monitor — the grade-poll DECISION, extracted from the GUI so it can be
// exercised HEADLESSLY (no JUCE GUI, no audio thread). It is the logic that used to live inline inside
// MainComponent::pollReferenceGrade(): run the match-gate FIRST, apply the two-consecutive-matched-polls
// STABLE debounce, and run gradeMeasurementWindow() exactly ONCE per match-session.
//
// What stays in the GUI (NOT here): the 30 Hz throttle (kGradePollTicks), the off-thread firPool dispatch,
// the engine snapshot copy / setLastMatchCoherence / publishReferenceGrade calls, and the cosmetic
// gradeSignalPresent() status. This unit is the pure DECISION only: given a response window and the
// reference, decide whether THIS poll grades, and (if so) what the grade is.
//
// Dependencies are juce_core-free at the interface (raw pointers + POD result); the .cpp pulls in only the
// existing pure DSP headers. The same regression the GUI poll suffered ("grade never fired because it
// waited for silence") is now testable without hardware — see tests/test_referencegradepoller.cpp.
namespace eb {

// The outcome of ONE poll. `coherence`/`matched` are always valid (the match-gate runs every poll). The
// grade fields (state/irSnrDb/thdPercent/mismatch/lowQuality) are meaningful ONLY when didGrade==true;
// they hold the gradeMeasurementWindow() verdict the GUI would publish via publishReferenceGrade().
struct GradePollResult {
    float coherence = 0.0f;                        // cross-correlation peak prominence published this poll
    bool  matched   = false;                       // the match-gate verdict for THIS poll's window
    bool  didGrade  = false;                        // a grade was produced THIS poll (the stable-match edge)
    int   alignOffset = 0;                          // where decide() located the sweep inside the window (>=0,
                                                    // clamped to keep a reference-length segment in range); the
                                                    // SAME offset the grade must use so the gate and grader agree

    RefMonState state = RefMonState::NotLearned;    // the published state — valid when didGrade
    float irSnrDb     = 0.0f;
    float thdPercent  = 0.0f;
    bool  mismatch    = false;                      // state == ReferenceStale (raise the RefMismatch flag)
    bool  lowQuality  = false;                      // the IR-quality guidance flag (info-only)
};

// The stateful grade-poll decision. Holds the two-poll debounce state; otherwise pure logic. NOT
// thread-safe by itself (the GUI owns it on the message thread); a headless caller drives it single-threaded.
class ReferenceGradePoller {
public:
    // One poll over a response `window` (the rolling-ring snapshot: leading silence + the sweep inside it)
    // against the learned `reference`. Mirrors MainComponent::pollReferenceGrade() EXACTLY:
    //   1. MATCH-GATE FIRST — eb::referenceMatches over min(refLen,winLen) samples (publishes coherence/matched).
    //   2. STABLE-MATCH debounce — a dropped match (!matched) re-arms the session; a grade fires once when the
    //      match holds across TWO consecutive polls (matched THIS poll AND the previous poll) and that match
    //      session has not already graded.
    //   3. gradeMeasurementWindow() — run ONCE per match-session (it re-runs the match-gate on the aligned
    //      segment, so a non-sweep window returns ReferenceStale, never a clean grade).
    // Returns the per-poll result; didGrade is true only on the grading poll. This runs the grade SYNCHRONOUSLY
    // (the headless harness + tests use it). A GUI that must grade off-thread can use decide() + gradeWindow().
    GradePollResult poll (const float* window, int winLen,
                          const float* reference, int refLen, double rate);

    // The match + debounce DECISION ONLY, without running the (heavy) grade. Steps 1 and 2 of poll(): the
    // match-gate publishes coherence/matched; didGrade tells the caller a grade SHOULD run now (the stable-match
    // edge). The grade fields are left default — call gradeWindow() to fill them, optionally off-thread. The GUI
    // uses this so the cross-correlation match runs on the message thread but the deconvolve+grade runs on a worker.
    //
    // ALIGNMENT (the honesty fix): the match-gate is NOT run over the window prefix. The ring snapshot is
    // oldest->newest, so a just-finished sweep sits at the END of the long window, OUTSIDE the reference-length
    // prefix. decide() FIRST cross-correlates the reference against the whole window to LOCATE the sweep
    // (crossCorrelateAlign), then runs referenceMatches on the ALIGNED reference-length segment — exactly where
    // gradeMeasurementWindow grades. The located offset is published in result.alignOffset so the subsequent
    // grade reuses it (no second cross-correlation, and the gate + grader agree on where the sweep is).
    GradePollResult decide (const float* window, int winLen,
                            const float* reference, int refLen);

    // The pure grade for a window decide() said to grade (no debounce state touched). Safe to call off-thread.
    // Returns the same state/quality fields poll() would have produced. Static: it is just gradeMeasurementWindow.
    // The two-arg `alignOffset` overload reuses the offset decide() already located (so the sweep is graded at
    // the SAME place the gate matched it, and the expensive cross-correlation runs once, not twice); the other
    // overload locates the sweep itself (for callers that did not run decide(), e.g. the synchronous poll()).
    static GradePollResult gradeWindow (const float* window, int winLen,
                                        const float* reference, int refLen, double rate);
    static GradePollResult gradeWindow (const float* window, int winLen,
                                        const float* reference, int refLen, double rate, int alignOffset);

    // Clear the debounce state (call on Start/Stop — a fresh run starts un-matched + un-graded, so two
    // consecutive matched polls are again required before the first grade).
    void reset();

private:
    bool lastPollMatched_   = false;   // previous poll's match result (the stable-match edge)
    bool gradedThisSession_ = false;   // one-grade-per-match-session latch (cleared when the match drops)
};

} // namespace eb
