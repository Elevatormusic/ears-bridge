#pragma once
#include "sim/SimSignals.h"
#include "sim/VirtualDevices.h"
#include "audio/AudioEngine.h"
#include "audio/LoopbackReference.h"      // findActiveSpan / validateReferenceCapture (the learn pures)
#include "audio/SweepSchedule.h"          // extractSchedule
#include "audio/ReferenceGradePoller.h"   // the production grade-poll decision (per ear)
#include "gui/GradeGuards.h"              // rateAllowsGrade / wouldRaiseLowSnr (the production predicates)
#include <functional>

// ==================================================================================================
// The rig ORCHESTRATOR (spec unit 3): one virtual measurement session end-to-end through the REAL
// pipeline. Nothing here re-models pipeline behaviour — every decision is the production function:
//
//   LEARN  = the exact MainComponent learn order (worker lambda ~MainComponent.cpp:1712-1755):
//            findActiveSpan per channel -> disjoint-spans + span<0.8*capture sanity -> trim ->
//            validateReferenceCapture(trim, rate, minSeconds=3.0) -> extractSchedule from the FULL
//            untrimmed stereo. The clean digital session IS what the loopback learn captures.
//   STREAM = the real capture/render callbacks via the two-clock feeder (VirtualDevices.h) with the
//            impaired mic timeline: ClockBridge + FreezeGate + AutoPerEar all run for real.
//   GRADE  = the exact MainComponent::pollReferenceGrade composition per ear: snapshotGradeRing ->
//            rateAllowsGrade -> ReferenceGradePoller (two-consecutive-matched-poll debounce) ->
//            publishReferenceGrade / setLastMatchCoherence / publishCompletedSweepSnrDb /
//            wouldRaiseLowSnr -> raiseLowSnr — the same calls, same order, same predicates.
// ==================================================================================================
namespace ebsim {

struct SessionOutcome {
    bool         learnOk = false;
    juce::String learnReason;             // names the failing learn rule (mirrors production wording)
    int   stateL = 0, stateR = 0;         // (int) eb::RefMonState per ear
    float sweepSnrL = 0, sweepSnrR = 0, irSnrL = 0, irSnrR = 0, thdL = 0, thdR = 0;
    float peakDbL = -120.0f, peakDbR = -120.0f;
    float coherenceL = 0, coherenceR = 0, mainLobeL = 0, mainLobeR = 0;
    eb::HealthFlag flags {};
    bool  cleanCapture = true;
};

using MicTransform = std::function<void (StereoTimeline&)>;

namespace detail {

// The production learn, verbatim order, on a stereo capture. Returns false + reason on rejection.
struct LearnResult {
    bool ok = false; juce::String reason;
    std::vector<float> refL, refR;        // the TRIMMED per-ear references (what production stores)
    eb::SweepSchedule  schedule;          // learned from the FULL untrimmed stereo
};

inline LearnResult learnFromSession (const StereoTimeline& clean) {
    LearnResult r;
    const int n = (int) clean.L.size();
    const auto spanL = eb::findActiveSpan (clean.L.data(), n, clean.fs);
    const auto spanR = eb::findActiveSpan (clean.R.data(), n, clean.fs);
    if (! spanL.valid)      { r.reason = "Rejected (Left ear): no sweep found";  return r; }
    if (! spanR.valid)      { r.reason = "Rejected (Right ear): no sweep found"; return r; }
    if (spanL.first < spanR.last && spanR.first < spanL.last) {
        r.reason = "Rejected: the two ears' sweeps overlap in time"; return r;
    }
    if ((spanL.last - spanL.first) > (int) (0.8 * (double) n)
     || (spanR.last - spanR.first) > (int) (0.8 * (double) n)) {
        r.reason = "Rejected: a sweep span covers most of the capture"; return r;
    }
    r.refL.assign (clean.L.begin() + spanL.first, clean.L.begin() + spanL.last);
    r.refR.assign (clean.R.begin() + spanR.first, clean.R.begin() + spanR.last);
    const auto vL = eb::validateReferenceCapture (r.refL.data(), (int) r.refL.size(), clean.fs, 3.0);
    const auto vR = eb::validateReferenceCapture (r.refR.data(), (int) r.refR.size(), clean.fs, 3.0);
    if (! vL.ok) { r.reason = "Rejected (Left ear): "  + vL.reason; return r; }
    if (! vR.ok) { r.reason = "Rejected (Right ear): " + vR.reason; return r; }
    r.schedule = eb::extractSchedule (clean.L.data(), clean.R.data(), n, clean.fs);
    r.ok = true;
    return r;
}

// The production per-ear grade poll (the pollReferenceGrade composition) over the engine's FINAL
// ring snapshot. Two polls through a fresh poller realise the two-consecutive-matched debounce.
inline void gradeEar (eb::AudioEngine& e, int ear, const std::vector<float>& reference,
                      double referenceRate, SessionOutcome& out) {
    const int winLen = (int) std::llround (eb::AudioEngine::gradingWindowSeconds() * referenceRate);
    std::vector<float> window ((size_t) winLen, 0.0f);
    const int got = e.snapshotGradeRing (ear, window.data(), winLen);
    if (got <= 0) return;                                     // nothing buffered for this ear

    // #32/#36: the PRODUCTION rate guard, from the same predicate header the GUI uses.
    if (! eb::rateAllowsGrade (referenceRate, e.gradingResponseRate())) {
        e.setLastMatchCoherence (ear, 0.0f);
        e.publishReferenceGrade (ear, (int) eb::RefMonState::ReferenceStale, 0.0f, 0.0f, true, false);
        (ear == 0 ? out.stateL : out.stateR) = (int) eb::RefMonState::ReferenceStale;
        return;
    }

    // Diagnostics FIRST: the debounce path's cheap decide() publishes coherence but not mainLobe, so
    // the outcome's match metrics come from one explicit full match probe (offline; cost is fine).
    const auto probe = eb::ReferenceGradePoller::matchAlign (window.data(), got,
                                                             reference.data(), (int) reference.size());
    auto& coher = (ear == 0 ? out.coherenceL : out.coherenceR);
    auto& lobe  = (ear == 0 ? out.mainLobeL  : out.mainLobeR);
    coher = probe.coherence; lobe = probe.mainLobe;

    eb::ReferenceGradePoller poller;
    eb::GradePollResult g;
    for (int pass = 0; pass < 2 && ! g.didGrade; ++pass)      // the stable-match two-poll debounce
        g = poller.poll (window.data(), got, reference.data(), (int) reference.size(), referenceRate);
    e.setLastMatchCoherence (ear, g.coherence);

    if (! g.didGrade) return;                                 // no stable match: nothing published

    e.publishReferenceGrade (ear, (int) g.state, g.irSnrDb, g.thdPercent, g.mismatch, g.lowQuality);
    (ear == 0 ? out.stateL : out.stateR)   = (int) g.state;
    (ear == 0 ? out.irSnrL : out.irSnrR)   = g.irSnrDb;
    (ear == 0 ? out.thdL   : out.thdR)     = g.thdPercent;
    (ear == 0 ? out.peakDbL : out.peakDbR) = g.sweepPeakDb;
    if (g.sweepSnrValid) {
        (ear == 0 ? out.sweepSnrL : out.sweepSnrR) = g.sweepSnrDb;
        e.publishCompletedSweepSnrDb (ear, g.sweepSnrDb);
        if (eb::wouldRaiseLowSnr (g.sweepSnrValid, g.sweepSnrDb))
            e.raiseLowSnr();
    }
}

} // namespace detail

// Run one full virtual session. `wrongReference` (optional) substitutes the LEARN source (S9: learn
// from a different sweep, measure the normal session) — the measure phase always streams the
// impaired copy of the NORMAL session.
inline SessionOutcome runVirtualSession (const SessionSpec& spec, const MicTransform& impair,
                                         const FeederSpec& feeder,
                                         const StereoTimeline* wrongReference = nullptr,
                                         double contentPpm = 0.0) {
    SessionOutcome out;
    SessionTruth truth;
    const auto cleanSession = makeDiracSession (spec, truth, contentPpm);

    // LEARN (from the clean digital session, or the wrong-reference source for S9).
    const auto learn = detail::learnFromSession (wrongReference ? *wrongReference : cleanSession);
    out.learnOk = learn.ok; out.learnReason = learn.reason;
    if (! learn.ok) return out;

    // MIC timeline = impaired copy of the clean session.
    StereoTimeline mic = cleanSession;
    if (impair) impair (mic);

    // ENGINE setup — the #12 ordering: every STOPPED-only install BEFORE prepareCallbacksForTest.
    eb::AudioEngine e;
    juce::AudioBuffer<float> unity (1, 8); unity.clear(); unity.setSample (0, 0, 1.0f);
    e.setLeftCalFir (unity); e.setRightCalFir (unity);
    e.setCombineMode (eb::CombineMode::AutoPerEar);
    if (learn.schedule.valid) e.setSweepSchedule (learn.schedule);
    e.setReferenceLoaded (true);
    e.prepareCallbacksForTest (spec.fs, 480, 1 << 16);

    // STREAM through the real callbacks (ClockBridge/FreezeGate/AutoPerEar all live). The grade-ring
    // snapshot publish is SINGLE-BUFFERED (it refreshes only once consumed), so mirror the GUI's
    // 30 Hz consumption: DRAIN every ready snapshot while streaming, then stream ~2 s of trailing
    // room silence WITHOUT draining so ONE fresh final snapshot (holding the whole session) is left
    // ready for the grade poll - the exact test_refmon_live recipe, which mirrors production timing.
    const int winLen = (int) std::llround (eb::AudioEngine::gradingWindowSeconds() * spec.fs);
    std::vector<float> drainScratch ((size_t) winLen, 0.0f);
    (void) streamSession (e, mic, feeder, [&] {
        if (e.gradingResponseReady (0)) (void) e.snapshotGradeRing (0, drainScratch.data(), winLen);
        if (e.gradingResponseReady (1)) (void) e.snapshotGradeRing (1, drainScratch.data(), winLen);
    });
    StereoTimeline tail; tail.fs = spec.fs;
    tail.L.assign ((size_t) std::llround (2.0 * spec.fs), 0.0f); tail.R = tail.L;
    if (impair) impair (tail);                                  // the room floor continues after the sweep
    (void) streamSession (e, tail, feeder);                     // NO drain: leaves a fresh snapshot ready

    // GRADE both ears via the production composition, then snapshot health.
    detail::gradeEar (e, 0, learn.refL, spec.fs, out);
    detail::gradeEar (e, 1, learn.refR, spec.fs, out);
    const auto h = e.health();
    out.flags = h.flags;
    out.cleanCapture = h.cleanCapture;
    return out;
}

} // namespace ebsim
