#pragma once
#include "SimSignals.h"      // same-directory includes: see VirtualDevices.h (clang vs MSVC quote-include resolution)
#include "VirtualDevices.h"
#include "audio/AudioEngine.h"
#include "audio/LoopbackReference.h"      // findActiveSpan / validateReferenceCapture (the learn pures)
#include "audio/SweepSchedule.h"          // extractSchedule
#include "audio/ReferenceGradePoller.h"   // the production grade-poll decision (per ear)
#include "audio/ResponseShape.h"          // the SP3 shape detectors (D1-D7) — mirror runShapeDetectors
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
    float renderedPeak = 0.0f;            // peak |sample| of the rendered cable feed (the render path ran)
    bool  renderedSweepCarried = false;   // mid-session render RMS >> lead RMS (the sweep actually crossed)

    // SP3 shape read-back: the engine's published shapeFlags(ear) + the two headline D1 scalars, after
    // the grade path ran the seven detectors (mirrors runShapeDetectors + publishShapeAnomalies). 0 when
    // that ear never graded (a non-graded outcome publishes NOTHING — the honesty gate).
    unsigned shapeFlagsL = 0u, shapeFlagsR = 0u;
    float    hfShelfL = 0.0f, hfShelfR = 0.0f, driftMaxL = 0.0f, driftMaxR = 0.0f;
    float    effHiL = 0.0f, effHiR = 0.0f, combDelayL = 0.0f, combDelayR = 0.0f;
    float    stepL = 0.0f, stepR = 0.0f;
    int      humBaseL = 0, humBaseR = 0;
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

// The SEVEN shape detectors, in the EXACT order + with the EXACT decisions of
// MainComponent::runShapeDetectors (MainComponent.cpp ~1948-2054). Replicated here (not called)
// because that fn is a MainComponent static coupled to ShapeResult + kShapePolaritySeg; the rig
// pins the parity contract by mirroring the pure eb:: sequence 1:1. PARITY CONTRACT: if the
// production runShapeDetectors order/decisions change, THIS must change in lockstep. Returns the
// per-ear flags/scalars + this ear's peak segment (for the message-thread cross-ear D4 pass).
struct RigShapeResult {
    unsigned flags = 0u;
    float driftMaxDb = 0.0f, hfShelfDb = 0.0f, combDelayMs = 0.0f, effHiHz = 0.0f, stepDb = 0.0f;
    int   humBaseHz = 0;
    std::vector<float> peakSeg;               // 2048-sample IR peak segment for D4 cross-ear
    eb::BandCurve newBaseline; bool setBaseline = false;
};

inline RigShapeResult runShapeDetectorsRig (bool gradedClean, const eb::BandCurve* baseline,
        const std::vector<float>& ir, double bandLoHz, double bandHiHz,
        const std::vector<float>& window, int windowStart, int gradedLen,
        const std::vector<float>& reference, double rate) {
    constexpr int kShapePolaritySeg = 2048;   // MainComponent.h kShapePolaritySeg
    RigShapeResult res;
    if (ir.empty() || rate <= 0.0) return res;
    const bool haveBand = bandLoHz > 0.0 && bandHiHz > bandLoHz;
    const double bLo = haveBand ? bandLoHz : 20.0;
    const double bHi = haveBand ? bandHiHz : 20000.0;
    const eb::WindowedSpectrum ws = eb::windowedBandSpectrum (ir.data(), (int) ir.size(), rate, bLo, bHi);
    if (ws.valid) {
        const eb::BandCurve cur = eb::computeBandCurve (ws);                              // D1
        if (cur.valid) {
            if (baseline != nullptr && baseline->valid) {
                const eb::DriftReport d = eb::compareCurves (*baseline, cur);
                if (d.valid) {
                    res.driftMaxDb = d.maxDeltaDb; res.hfShelfDb = d.hfShelfDb;
                    if (d.exceedsTolerance) res.flags |= eb::ShapeFlag::kDrift;
                }
            } else if (gradedClean) { res.newBaseline = cur; res.setBaseline = true; }
        }
        const eb::CombReport comb = eb::detectComb (ws, ir.data(), (int) ir.size());      // D2
        if (comb.found) { res.flags |= eb::ShapeFlag::kComb; res.combDelayMs = comb.delayMs; }
        if (haveBand) {                                                                    // D3
            const eb::TruncationReport tr = eb::detectTruncation (ws, bandLoHz, bandHiHz);
            if (tr.valid) {
                res.effHiHz = tr.effHiHz;
                if (tr.effLoHz == 0.0f && tr.effHiHz == 0.0f) res.flags |= eb::ShapeFlag::kNoBand;
                if (tr.truncatedHi) res.flags |= eb::ShapeFlag::kTruncHi;
                if (tr.truncatedLo) res.flags |= eb::ShapeFlag::kTruncLo;
            }
        }
        const eb::SpikeReport spk = eb::detectResonance (ws);                             // D5b
        if (spk.found) res.flags |= eb::ShapeFlag::kResonance;
    }
    const float lobe = eb::mainLobeWidthSamples (ir.data(), (int) ir.size());            // D6
    if (lobe > 0.0f && lobe > 10.0f) res.flags |= eb::ShapeFlag::kSkew;                  // kLobeWidthMax = 10
    if (windowStart >= 16384 && (int) window.size() >= windowStart) {                    // D5a
        const eb::HumReport hum = eb::detectMainsHum (window.data(), windowStart, rate);
        if (hum.found) { res.flags |= eb::ShapeFlag::kHum; res.humBaseHz = (int) std::lround (hum.baseHz); }
    }
    if (gradedLen > 0 && windowStart >= 0 && (int) window.size() >= windowStart + gradedLen
        && (int) reference.size() >= gradedLen) {                                        // D7
        const eb::StepReport st = eb::detectLevelStep (window.data() + windowStart, reference.data(),
                                                       gradedLen, rate);
        if (st.found) { res.flags |= eb::ShapeFlag::kStep; res.stepDb = st.stepDb; }
    }
    res.peakSeg.assign ((size_t) kShapePolaritySeg, 0.0f);                               // D4 segment
    eb::extractPeakSegment (ir.data(), (int) ir.size(), res.peakSeg.data(), kShapePolaritySeg);
    return res;
}

// Cross-ear D4 state carried across the two gradeEar calls (ear 0 stashes, ear 1 runs the pass) —
// mirrors MainComponent's shapePeakSegPerEar_ / shapePeakSegFresh_ (the message-thread cross-ear pass).
struct CrossEarShape { std::vector<float> seg[2]; bool fresh[2] = { false, false }; };

// The production per-ear grade poll (the pollReferenceGrade composition) over the engine's FINAL
// ring snapshot. Two polls through a fresh poller realise the two-consecutive-matched debounce.
// `xe` (optional) threads the cross-ear D4 polarity pass across the two ears (as production does on
// the message thread). SP3: on a MATCHED grade the shape detectors run on the SAME IR/band the grade
// keyed off (via GradeArtifacts), then the engine publishes them — INFO-ONLY, exactly like production.
inline void gradeEar (eb::AudioEngine& e, int ear, const std::vector<float>& reference,
                      double referenceRate, SessionOutcome& out, CrossEarShape* xe = nullptr) {
    // Verifier MINOR-2: production sizes the window by the GRANTED rate, not the reference rate
    // (identical at 48 k, but a future rate-mismatch scenario must mirror production's geometry).
    const int winLen = (int) std::llround (eb::AudioEngine::gradingWindowSeconds() * e.gradingResponseRate());
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

    // SP3: the grade byproducts (graded IR + reference band + aligned-segment offset/len) the shape
    // detectors need — production plumbs them through GradeArtifacts on the GRADING poll.
    eb::GradeArtifacts art;
    eb::ReferenceGradePoller poller;
    eb::GradePollResult g;
    for (int pass = 0; pass < 2 && ! g.didGrade; ++pass)      // the stable-match two-poll debounce
        g = poller.poll (window.data(), got, reference.data(), (int) reference.size(), referenceRate, &art);
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
    // Verifier MINOR-1: production publishes the raw sweep peak UNCONDITIONALLY after the SNR block
    // (gradeOneEar), so the engine's referenceSweepPeakDb(ear) store must be fed here too.
    e.publishCompletedSweepPeakDb (ear, g.sweepPeakDb);

    // ---- SP3 shape pass (INFO-ONLY) ---------------------------------------------------------------
    // Mirrors the gradeOneEar tail (MainComponent.cpp ~2149-2241): ONLY a MATCHED graded state runs the
    // detectors; a non-graded outcome publishes NOTHING here. The worker runs runShapeDetectors against a
    // COPY of THIS ear's baseline; the message thread sets the baseline on the first GradedClean, runs the
    // cross-ear D4 pass once BOTH ears carry a fresh segment, then publishes. Order + decisions are pinned
    // to production (see runShapeDetectorsRig's PARITY CONTRACT note).
    const auto rs = (eb::RefMonState) g.state;
    const bool graded = (rs == eb::RefMonState::GradedClean || rs == eb::RefMonState::GradedMarginal
                         || rs == eb::RefMonState::GradedSuspect);
    if (graded) {
        const eb::BandCurve* baseline = e.shapeBaseline (ear);   // message-thread single writer (COPY on the worker)
        eb::BandCurve baselineCopy; if (baseline) baselineCopy = *baseline;
        RigShapeResult shape = runShapeDetectorsRig (rs == eb::RefMonState::GradedClean,
                                                     baselineCopy.valid ? &baselineCopy : nullptr,
                                                     art.ir, art.bandLoHz, art.bandHiHz,
                                                     window, art.windowStart, art.gradedLen, reference, referenceRate);
        // The FIRST GradedClean per ear learns the D1 baseline (single writer).
        if (shape.setBaseline && shape.newBaseline.valid)
            e.setShapeBaseline (ear, shape.newBaseline);
        // D4 cross-ear polarity: stash THIS ear's fresh segment; run the L-vs-R pass once BOTH are fresh.
        if (xe != nullptr) {
            const int me = (ear == 1) ? 1 : 0, other = 1 - me;
            if ((int) shape.peakSeg.size() == 2048) { xe->seg[me] = shape.peakSeg; xe->fresh[me] = true; }
            if (xe->fresh[0] && xe->fresh[1]
                && (int) xe->seg[0].size() == 2048 && (int) xe->seg[1].size() == 2048) {
                const eb::PolarityReport pol = eb::crossEarPolarity (xe->seg[0].data(), 2048,
                                                                     xe->seg[1].data(), 2048);
                if (pol.valid && pol.inverted) {
                    shape.flags |= eb::ShapeFlag::kPolarity;
                    e.raiseShapeFlag (other, eb::ShapeFlag::kPolarity);        // the pair carries it on both ears
                }
            }
        }
        e.publishShapeAnomalies (ear, shape.flags, shape.driftMaxDb, shape.hfShelfDb,
                                 /*combDepthDb*/ 0.0f, shape.combDelayMs, /*effLoHz*/ 0.0f,
                                 shape.effHiHz, /*lobeWidth*/ 0.0f, shape.stepDb,
                                 shape.humBaseHz, /*resonanceHz*/ 0.0f);
        // Read the engine's PUBLISHED shape state back into the outcome (the acquire load + relaxed scalars,
        // exactly the GUI's read path — so the scenarios assert on what the app would show).
        (ear == 0 ? out.shapeFlagsL : out.shapeFlagsR) = e.shapeFlags (ear);
        (ear == 0 ? out.hfShelfL : out.hfShelfR)       = e.shapeHfShelfDb (ear);
        (ear == 0 ? out.driftMaxL : out.driftMaxR)     = e.shapeDriftMaxDb (ear);
        (ear == 0 ? out.effHiL : out.effHiR)           = e.shapeEffHiHz (ear);
        (ear == 0 ? out.combDelayL : out.combDelayR)   = e.shapeCombDelayMs (ear);
        (ear == 0 ? out.stepL : out.stepR)             = e.shapeStepDb (ear);
        (ear == 0 ? out.humBaseL : out.humBaseR)       = e.shapeHumBaseHz (ear);
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
    // LEARN always uses the UNDRIFTED session (the reference was learned earlier, before any content
    // skew) - contentPpm applies only to the MEASURE-phase mic timeline (S6: drift SINCE learn).
    const auto cleanSession = makeDiracSession (spec, truth, 0.0);
    const auto learn = detail::learnFromSession (wrongReference ? *wrongReference : cleanSession);
    out.learnOk = learn.ok; out.learnReason = learn.reason;
    if (! learn.ok) return out;

    // MIC timeline = impaired copy of the (possibly content-drifted) session.
    SessionTruth micTruth;
    StereoTimeline mic = (contentPpm != 0.0) ? makeDiracSession (spec, micTruth, contentPpm)
                                             : cleanSession;
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
    const auto rendered = streamSession (e, mic, feeder, [&] {
        if (e.gradingResponseReady (0)) (void) e.snapshotGradeRing (0, drainScratch.data(), winLen);
        if (e.gradingResponseReady (1)) (void) e.snapshotGradeRing (1, drainScratch.data(), winLen);
    });
    StereoTimeline tail; tail.fs = spec.fs;
    tail.L.assign ((size_t) std::llround (2.0 * spec.fs), 0.0f); tail.R = tail.L;
    if (impair) impair (tail);                                  // the room floor continues after the sweep
    (void) streamSession (e, tail, feeder);                     // NO drain: leaves a fresh snapshot ready

    // Rendered-output integrity (spec S1): the cable feed must have CARRIED the measurement - peak
    // plus a mid-vs-lead RMS ratio (the lead is the FIFO-primed silence + tone; the middle holds
    // sweeps). Pure accounting on the collected render; no extra streaming.
    {
        for (float v : rendered.mono) out.renderedPeak = std::max (out.renderedPeak, std::abs (v));
        const size_t n3 = rendered.mono.size() / 3;
        auto rms = [&] (size_t a, size_t b) {
            double acc = 0.0; for (size_t i = a; i < b && i < rendered.mono.size(); ++i)
                acc += (double) rendered.mono[i] * rendered.mono[i];
            return std::sqrt (acc / (double) std::max<size_t> (1, b - a));
        };
        const double lead = rms (0, (size_t) std::llround (0.3 * spec.fs));
        const double mid  = rms (n3, 2 * n3);
        out.renderedSweepCarried = mid > 10.0 * (lead + 1e-9);
    }

    // GRADE both ears via the production composition, then snapshot health.
    detail::CrossEarShape xe;
    detail::gradeEar (e, 0, learn.refL, spec.fs, out, &xe);
    detail::gradeEar (e, 1, learn.refR, spec.fs, out, &xe);
    // Re-read the FINAL published flags: the ear-1 cross-ear D4 pass may have OR'd kPolarity onto ear 0
    // AFTER gradeEar(0) read its flags back (production's message-thread single writer does the same).
    if (out.stateL != 0) out.shapeFlagsL = e.shapeFlags (0);
    if (out.stateR != 0) out.shapeFlagsR = e.shapeFlags (1);
    const auto h = e.health();
    out.flags = h.flags;
    out.cleanCapture = h.cleanCapture;
    return out;
}

// ==================================================================================================
// Two-pass session (SP3 Task 6): learn ONCE from the clean session, prepare the engine ONCE, then
// stream TWO measurement passes into the SAME engine (impair1 then impair2) with NO re-prepare — so
// pass 1's D1 baseline (set on its first GradedClean) SURVIVES into pass 2, where drift is measured
// against it. This is the drift-monitor's whole point: a mid-SESSION corruption is caught by comparing
// the SECOND measurement's recovered IR against the FIRST clean one. prepare() resets the shape state,
// so re-preparing between passes would wipe the baseline — hence the single-prepare invariant.
// ==================================================================================================
struct TwoPassOutcome {
    SessionOutcome first, second;
    unsigned shapeFlags1L = 0u, shapeFlags2L = 0u;   // engine shapeFlags(0) after each pass
    float    hfShelf2L = 0.0f, driftMax2L = 0.0f;    // pass-2 D1 drift scalars (the flip's evidence)
};

inline TwoPassOutcome runVirtualSessionTwoPass (const SessionSpec& spec, const MicTransform& impair1,
                                                const MicTransform& impair2, const FeederSpec& feeder) {
    TwoPassOutcome tp;
    SessionTruth truth;
    const auto cleanSession = makeDiracSession (spec, truth, 0.0);
    const auto learn = detail::learnFromSession (cleanSession);           // LEARN ONCE (clean)
    tp.first.learnOk = tp.second.learnOk = learn.ok;
    tp.first.learnReason = tp.second.learnReason = learn.reason;
    if (! learn.ok) return tp;

    // ENGINE setup — the #12 ordering, done ONCE. No re-prepare between passes (the baseline invariant).
    eb::AudioEngine e;
    juce::AudioBuffer<float> unity (1, 8); unity.clear(); unity.setSample (0, 0, 1.0f);
    e.setLeftCalFir (unity); e.setRightCalFir (unity);
    e.setCombineMode (eb::CombineMode::AutoPerEar);
    if (learn.schedule.valid) e.setSweepSchedule (learn.schedule);
    e.setReferenceLoaded (true);
    e.prepareCallbacksForTest (spec.fs, 480, 1 << 16);

    const int winLen = (int) std::llround (eb::AudioEngine::gradingWindowSeconds() * spec.fs);
    std::vector<float> drainScratch ((size_t) winLen, 0.0f);
    auto drain = [&] {
        if (e.gradingResponseReady (0)) (void) e.snapshotGradeRing (0, drainScratch.data(), winLen);
        if (e.gradingResponseReady (1)) (void) e.snapshotGradeRing (1, drainScratch.data(), winLen);
    };

    // One measurement pass: (optionally) FLUSH the rolling grade ring first, stream the impaired session
    // (draining at the GUI cadence), then ~2 s of trailing room silence WITHOUT draining (leaves ONE fresh
    // final snapshot per ear), then grade both ears into `out`. The engine PERSISTS across passes — same
    // test_refmon_live recipe as single-pass. The FLUSH is the two-pass invariant: the grade ring is a
    // 28 s ROLLING buffer (kGradingSeconds), longer than one ~14 s session, so back-to-back passes would
    // otherwise leave pass 1's sweeps inside pass 2's grade window and the match-gate would align to the
    // OLD (clean) sweep — grading pass 1 twice (measured: pass-2 drift read 0.00 exactly). Streaming a full
    // ring-length of silence before pass 2 rolls pass 1 completely out, so pass 2 grades ITS OWN sweep. This
    // MODELS production faithfully: a real second Dirac run happens long after the first; the ring has rolled
    // over by then. Pass 1 is NOT flushed (nothing precedes it). The engine/baseline are untouched by the flush.
    auto runPass = [&] (const MicTransform& impair, SessionOutcome& out, bool flushFirst) {
        if (flushFirst) {
            StereoTimeline flush; flush.fs = spec.fs;
            const int flushLen = (int) std::llround ((eb::AudioEngine::gradingWindowSeconds() + 1.0) * spec.fs);
            flush.L.assign ((size_t) flushLen, 0.0f); flush.R = flush.L;
            if (impair) impair (flush);                    // the room floor persists between runs
            (void) streamSession (e, flush, feeder, drain);
        }
        StereoTimeline mic = cleanSession;                 // fresh impaired copy of the clean session
        if (impair) impair (mic);
        (void) streamSession (e, mic, feeder, drain);
        StereoTimeline tail; tail.fs = spec.fs;
        tail.L.assign ((size_t) std::llround (2.0 * spec.fs), 0.0f); tail.R = tail.L;
        if (impair) impair (tail);
        (void) streamSession (e, tail, feeder);            // NO drain: fresh final snapshot ready
        detail::CrossEarShape xe;
        detail::gradeEar (e, 0, learn.refL, spec.fs, out, &xe);
        detail::gradeEar (e, 1, learn.refR, spec.fs, out, &xe);
        if (out.stateL != 0) out.shapeFlagsL = e.shapeFlags (0);
        if (out.stateR != 0) out.shapeFlagsR = e.shapeFlags (1);
        const auto h = e.health();
        out.flags = h.flags; out.cleanCapture = h.cleanCapture;
    };

    runPass (impair1, tp.first,  /*flushFirst*/ false);
    runPass (impair2, tp.second, /*flushFirst*/ true);

    tp.shapeFlags1L = tp.first.shapeFlagsL;
    tp.shapeFlags2L = tp.second.shapeFlagsL;
    tp.hfShelf2L    = tp.second.hfShelfL;
    tp.driftMax2L   = tp.second.driftMaxL;
    return tp;
}

} // namespace ebsim
