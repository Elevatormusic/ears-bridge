#include <catch2/catch_test_macros.hpp>
#include "sim/VirtualSession.h"

// ==================================================================================================
// The synthetic end-to-end scenarios (spec: 2026-07-02-synthetic-measurement-rig-design, S1-S9).
// SEPARATION-ONLY assertions: relationships against the in-run clean baseline, never synthetic
// absolutes (the on-device ratification campaign owns absolute thresholds).
// ==================================================================================================

static ebsim::SessionSpec spec() { return {}; }

// The standard clean impairment: coupler IR + a very quiet (~-94 dBFS) room floor. The floor
// USED to be forced this low because the flat-regularization THD defect (SIM F1, discovered by
// this rig) read tens-of-percent on any realistic floor; sub-project 2's banded regularization
// fixed the root cause (see SIM F1 RESOLVED below). The quiet floor is kept anyway: the clean
// baseline should isolate the pipeline, not test noise robustness (S2/F1 own that).
static ebsim::MicTransform cleanImpair() {
    return [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m);
        ebsim::addSeededNoise (m, 0.00002f, 42u);
    };
}

// S1 - the clean baseline. Everything downstream compares against this run.
TEST_CASE("SIM S1: a clean virtual session learns, streams, and grades BOTH ears clean") {
    auto out = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    INFO ("learn: " << out.learnReason << " | L state=" << out.stateL << " snr=" << out.sweepSnrL
          << " ir=" << out.irSnrL << " thd=" << out.thdL << " mainLobe=" << out.mainLobeL
          << " | R state=" << out.stateR << " snr=" << out.sweepSnrR << " mainLobe=" << out.mainLobeR);
    REQUIRE (out.learnOk);
    CHECK (out.stateL == (int) eb::RefMonState::GradedClean);
    CHECK (out.stateR == (int) eb::RefMonState::GradedClean);
    CHECK (out.cleanCapture);
    // The RV pin, calibrated to the rig: the research target band (0.2-0.4) is the ON-DEVICE
    // population, whose denominator includes a real room's diffuse correlation energy across a 28 s
    // window - a clean synthetic window cannot reach it with any PHYSICAL earcup IR (measured here:
    // the spread coupler IR reads ~0.71; forcing 0.2-0.4 would need >60% of the IR's energy past
    // 2.7 ms, which is a cave, not a headphone). The pin's SUBSTANCE is preserved: the synthetic
    // must sit clearly in the spread-IR region (far below a delta's ~0.9+, comfortably above the
    // 0.08 gate floor), never the unrealistic-delta corner the gate was re-tuned away from.
    CHECK (out.mainLobeL > 0.15f); CHECK (out.mainLobeL < 0.80f);
    CHECK (out.mainLobeR > 0.15f); CHECK (out.mainLobeR < 0.80f);
}

// ==================================================================================================
// SIM F1 - RESOLVED (sub-project 2, 2026-07-03). The rig's first discovery (2026-07-02): the
// v0.4.0 flat-regularized inverse boosted out-of-band content ~1/reg, so broadband room noise
// deconvolved ASYMMETRICALLY into the negative-time THD region (the reference's >f2 spectral
// content lives at the sweep END, so boosted noise maps to negative lags). Dose-response measured
// pre-fix: ~1.3% THD at a -94 dBFS floor, ~6.7% at -80, ~33% at -66, ~134% at -54 - real rooms
// are -60..-40 dBFS at the EARS, so on-device THD was meaningless. The reference-derived banded
// regularization (deriveBandedRegularization; spec 2026-07-02) ATTENUATES out-of-band bins
// instead, and this test is the REGRESSION PROOF: the pre-fix pin here was `noisy.thdL > 10.0f`
// (EXPECT_INFLATED). If THD noise-inflation ever returns, THIS is the test that must catch it.
// ==================================================================================================
TEST_CASE("SIM F1 RESOLVED: broadband noise no longer inflates the THD metric (regression proof)") {
    auto quiet = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    auto noisy = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m);
        ebsim::addSeededNoise (m, 0.0005f, 42u);           // ~-66 dBFS: QUIETER than most real rooms
    }, {});
    REQUIRE (quiet.learnOk); REQUIRE (noisy.learnOk);
    INFO ("quiet thd=" << quiet.thdL << "  noisy thd=" << noisy.thdL << "  noisy ir=" << noisy.irSnrL);
    CHECK (noisy.thdL < 5.0f);                             // read ~33% under the flat eps
    CHECK (noisy.irSnrL > 20.0f);                          // the IR stays healthy, as before
    CHECK (quiet.thdL < 5.0f);                             // and the quiet baseline stays sane
}

// ==================================================================================================
// Rig-integrity guards (Task 5).
// ==================================================================================================
TEST_CASE("SIM guard: the FFT working length cannot wrap a harmonic image onto the linear peak") {
    ebsim::SessionSpec sp; ebsim::SessionTruth truth;
    auto clean = ebsim::makeDiracSession (sp, truth);
    auto learn = ebsim::detail::learnFromSession (clean);
    REQUIRE (learn.ok);
    const int refLen = (int) learn.refL.size();
    int p2 = 1; while (p2 < refLen) p2 <<= 1;                   // the deconvolver's nextPow2 padding
    const int sweepLen = eb::measuredSweepLength (learn.refL.data(), refLen);
    const auto offs = eb::farinaHarmonicOffsets (sweepLen > 0 ? sweepLen : refLen, sp.fs);
    REQUIRE_FALSE (offs.empty());
    // The zero-pad guard band must exceed the farthest THD-region reach (1.2x the m=4 offset) plus
    // margin, so circular wraparound can never alias distortion-region content onto the linear IR.
    CHECK (p2 - refLen > (int) std::lround (1.2 * offs.back()) + 512);
}

TEST_CASE("SIM guard: the rendered cable feed carried the measurement (S1 addition)") {
    auto out = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    REQUIRE (out.learnOk);
    CHECK (out.renderedPeak > 0.15f);
    CHECK (out.renderedSweepCarried);
}

// ==================================================================================================
// The impairment scenarios (Tasks 6-7). SEPARATION-ONLY: every assertion is a relationship against
// the in-run clean baseline. The clean outcome is computed per scenario that needs it.
// ==================================================================================================
TEST_CASE("SIM S2: room noise degrades sweepSNR; heavy noise never reads GradedClean") {
    auto clean = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    auto noisy = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::addSeededNoise (m, 0.02f, 7u); }, {});      // ~-34 dBFS floor
    REQUIRE (clean.learnOk); REQUIRE (noisy.learnOk);
    INFO ("clean snr=" << clean.sweepSnrL << "  noisy snr=" << noisy.sweepSnrL
          << "  noisy state=" << noisy.stateL);
    CHECK (noisy.sweepSnrL < clean.sweepSnrL - 10.0f);
    auto heavy = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::addSeededNoise (m, 0.08f, 7u); }, {});      // ~-22 dBFS floor
    INFO ("heavy state=" << heavy.stateL << " snr=" << heavy.sweepSnrL);
    CHECK (heavy.stateL != (int) eb::RefMonState::GradedClean);                   // never falsely clean
}

TEST_CASE("SIM S3: open-back crosstalk does not collapse sweepSNR (pins 37 end-to-end); shaped agrees") {
    auto clean = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    REQUIRE (clean.learnOk);
    for (const bool shaped : { false, true }) {
        auto xt = ebsim::runVirtualSession (spec(), [shaped] (ebsim::StereoTimeline& m) {
            ebsim::convolveIr (m);
            if (shaped) ebsim::mixCrosstalkShaped (m, -15.0f); else ebsim::mixCrosstalk (m, -15.0f);
            ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
        REQUIRE (xt.learnOk);
        INFO ((shaped ? "shaped" : "flat") << " xt: stateR=" << xt.stateR
              << " snrR=" << xt.sweepSnrR << " (clean " << clean.sweepSnrR << ")"
              << " stateL=" << xt.stateL);
        CHECK (xt.stateL == (int) eb::RefMonState::GradedClean);
        CHECK (xt.stateR == (int) eb::RefMonState::GradedClean);
        // Verifier M1: the #37 regime is ear R - ITS leading noise region [0, align) contains the
        // FIRST (L) sweep's -15 dB leak; ear L's leading region holds only tone+silence, so
        // asserting on L was vacuous (bit-identical to clean). The measured truth: the robust
        // 25th-percentile floor lands on the LEVEL TONE (quiet blocks outnumber the leak's 61%
        // contamination), reading ~36 dB flat / ~57 dB shaped versus the clean ~87 - an honest drop
        // from the near-digital-silence baseline, NOT a collapse. The old full-RMS floor was
        // energy-dominated by the leak (~17 dB here, the pre-#37 false-noisy regime). The
        // load-bearing form of "#37 end-to-end": the crosstalk case still clears the PRODUCTION
        // green band, where the old floor put it deep in the red.
        CHECK (xt.sweepSnrR > eb::kSweepSnrGoodDb);
    }
}

TEST_CASE("SIM S4: harmonic distortion is detected and monotone; gross distortion escalates") {
    auto clean = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    // Sub-project 2 follow-through: with the banded regularization the negative-time region is no
    // longer dominated by reg-boosted junk, so the metric is finally MONOTONE in the distortion
    // coefficient (pre-fix it INVERTED at this drive: a2=0.10 read ~754%, a2=0.22 ~167%). The old
    // `gross > 10%` pin measured the DEFECT's garbage, not distortion. Measured post-fix: clean
    // 0%, a2=0.10 -> ~11%, a2=0.22 -> ~24% - a 2.20x reading for a 2.2x coefficient, i.e. LINEAR
    // in a2 (the region metric integrates the compressed H2 pulse's energy, so it runs above the
    // instantaneous H2/H1 = a2*A/2 ballpark; absolute calibration is the on-device campaign's).
    auto distort = [] (float a2) {
        return [a2] (ebsim::StereoTimeline& m) {
            ebsim::applyDistortion (m, a2, 0.0f);              // drive pinned by SessionSpec (0.5 peak)
            ebsim::convolveIr (m);
            ebsim::addSeededNoise (m, 0.00002f, 42u);
        };
    };
    auto five  = ebsim::runVirtualSession (spec(), distort (0.10f), {});
    auto gross = ebsim::runVirtualSession (spec(), distort (0.22f), {});
    REQUIRE (clean.learnOk); REQUIRE (five.learnOk); REQUIRE (gross.learnOk);
    INFO ("clean thd=" << clean.thdL << "  a2=0.10 thd=" << five.thdL << "  a2=0.22 thd=" << gross.thdL);
    CHECK (five.thdL  > 2.0f * (clean.thdL + 0.01f));          // detected well above the clean baseline
    CHECK (gross.thdL > 1.3f * five.thdL);                     // REAL coefficient monotonicity (the SP2 win)
    CHECK (gross.thdL > 1.0f);                                 // gross distortion clearly registers
}

TEST_CASE("SIM S5: bridge-clock drift - realistic ppm stays clean; stress degrades HONESTLY") {
    for (const double ppm : { 50.0, 150.0 }) {
        ebsim::FeederSpec f; f.bridgePpm = ppm;
        auto out = ebsim::runVirtualSession (spec(), cleanImpair(), f);
        REQUIRE (out.learnOk);
        INFO ("ppm=" << ppm << " state=" << out.stateL << " clean=" << out.cleanCapture);
        CHECK (out.stateL == (int) eb::RefMonState::GradedClean);
        CHECK (out.cleanCapture);                              // freeze holds; no drift flags
    }
    // Verifier m2 relabel: 400 ppm x ~12 s = ~230 samples of net skew - far inside the half-primed
    // 65536-frame FIFO, so even the beyond-spec rung is ABSORBED over a session this short (the
    // freeze emergency edge needs hours of accumulation or a tiny FIFO; not worth CI minutes). The
    // assertion is the honesty disjunction: absorbed-and-clean, or flagged-with-explanation.
    ebsim::FeederSpec f; f.bridgePpm = 400.0;
    auto stress = ebsim::runVirtualSession (spec(), cleanImpair(), f);
    REQUIRE (stress.learnOk);
    INFO ("400ppm: clean=" << stress.cleanCapture << " flags=" << (unsigned) stress.flags
          << " state=" << stress.stateL);
    if (! stress.cleanCapture)
        CHECK (eb::any (stress.flags & (eb::HealthFlag::ExcessDrift | eb::HealthFlag::SweepRetimed
                                        | eb::HealthFlag::Dropout | eb::HealthFlag::FifoStarved)));
    else
        CHECK (stress.stateL == (int) eb::RefMonState::GradedClean);   // absorbed => still grades clean
}

TEST_CASE("SIM S6: content-clock drift since learn - the verdict is honest and never noise-blamed") {
    auto clean = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    auto drift = ebsim::runVirtualSession (spec(), cleanImpair(), {}, nullptr, /*contentPpm*/ 150.0);
    REQUIRE (drift.learnOk);
    INFO ("content-drift 150ppm: state=" << drift.stateL << " snr=" << drift.sweepSnrL
          << " ir=" << drift.irSnrL << " coherence=" << drift.coherenceL);
    // CHARACTERIZATION (spec S6): 150 ppm over 3.2 s = ~23 samples of terminal skew - inside the
    // correlation lobe, so a grade is expected; the requirement is a grade OR no-grade (both honest),
    // and any grade must not blame noise for a pure timing skew.
    const bool graded  = drift.stateL == (int) eb::RefMonState::GradedClean
                      || drift.stateL == (int) eb::RefMonState::GradedMarginal
                      || drift.stateL == (int) eb::RefMonState::GradedSuspect;
    const bool ungraded = drift.stateL == 0;                   // no grade recorded (the outcome's raw
                                                                // default; == RefMonState::NotLearned's value)
    CHECK ((graded || ungraded));
    if (graded)
        CHECK (drift.sweepSnrL > clean.sweepSnrL - 6.0f);      // the skew is not mis-read as noise
}

TEST_CASE("SIM S7: level errors - low level reads low peaks; clipping invalidates with positive dBFS") {
    auto low = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::applyGainDb (m, -30.0f);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (low.learnOk);
    INFO ("low: state=" << low.stateL << " peak=" << low.peakDbL);
    // Verifier M2: the low drive grades Suspect (not Clean), so gating this on GradedClean left the
    // peak-truth check DEAD. Any recorded grade must publish the truthful low peak.
    REQUIRE (low.stateL != 0);                                 // a grade was recorded
    CHECK (low.peakDbL < -25.0f);                              // the published peak tells the truth

    auto clip = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::applyGainDb (m, +12.0f); ebsim::clipHard (m); }, {});
    REQUIRE (clip.learnOk);
    INFO ("clip: clean=" << clip.cleanCapture << " flags=" << (unsigned) clip.flags);
    CHECK_FALSE (clip.cleanCapture);                           // rail runs -> ClipConfirmed invalidates
    CHECK (eb::any (clip.flags & eb::HealthFlag::ClipConfirmed));
}

// ==================================================================================================
// SIM F2 - THE RIG'S SECOND DISCOVERY (2026-07-02, found when the planned "rig bites" scenario
// FAILED): an LTI spectral corruption (the historical tinny-EQ class - HF droop in the capture
// path) is INVISIBLE to the ENTIRE quality machinery. Measured: a brutal 6 kHz one-pole droop
// leaves coherence at 0.9912 vs clean 0.9920, mainLobe unchanged, IR-SNR even slightly BETTER,
// verdict GradedClean. This is structural, not a tuning issue: deconvolution ABSORBS any
// linear-time-invariant filtering into "the headphone IR" - a spectrally-wrong-but-clean
// measurement is mathematically indistinguishable from a different headphone. The pipeline grades
// signal QUALITY (match/noise/distortion), not spectral FIDELITY. Historically this class was
// caught by Dirac collapsing its detected range + human graph reading, never by the bridge.
// KNOWN GAP + candidate future work: a response-drift monitor (compare each measurement's recovered
// IR magnitude response against the session's first clean measurement; flag band-level drift).
// This test PINS the invisibility so the gap is documented and its closure will be measurable.
// ==================================================================================================
TEST_CASE("SIM F2: an LTI HF-droop corruption is INVISIBLE to the grade (KNOWN GAP, pinned)") {
    auto clean = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    auto droop = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::applyHfDroop (m, 6000.0f);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (clean.learnOk); REQUIRE (droop.learnOk);
    INFO ("droop: state=" << droop.stateL << " coherence=" << droop.coherenceL
          << " (clean " << clean.coherenceL << ")  lobe=" << droop.mainLobeL
          << " (clean " << clean.mainLobeL << ")  ir=" << droop.irSnrL << " (clean " << clean.irSnrL << ")");
    // The pinned GAP: every metric reads clean through a corruption that guts everything above 6 kHz.
    // If any of these start failing, the pipeline has GAINED spectral-fidelity detection - move these
    // expectations into a positive "the corruption is DETECTED" form and celebrate.
    CHECK (droop.stateL == (int) eb::RefMonState::GradedClean);
    CHECK (droop.coherenceL > clean.coherenceL - 0.01f);
    CHECK (droop.mainLobeL  > clean.mainLobeL * 0.8f);
    CHECK (droop.irSnrL     > clean.irSnrL - 6.0f);
}

TEST_CASE("SIM S9: a wrong reference never yields a grade or quality numbers (honesty end-to-end)") {
    // Learn from a DIFFERENT sweep (100 Hz - 8 kHz), then measure the NORMAL session. Production's
    // match gate never fires two consecutive matches -> gradeEar publishes NOTHING (the outcome's
    // state stays at its raw default 0; ReferenceStale is only reached when a grade RUNS and its
    // internal gate fails). The honesty contract: no verdict, no numbers, no false green.
    ebsim::SessionSpec wrongSpec = spec(); wrongSpec.f1 = 100.0; wrongSpec.f2 = 8000.0;
    ebsim::SessionTruth wt;
    const auto wrong = ebsim::makeDiracSession (wrongSpec, wt);
    auto out = ebsim::runVirtualSession (spec(), cleanImpair(), {}, &wrong);
    REQUIRE (out.learnOk);
    INFO ("wrong-ref: state=" << out.stateL << " coherence=" << out.coherenceL
          << " thd=" << out.thdL << " ir=" << out.irSnrL);
    CHECK (out.stateL != (int) eb::RefMonState::GradedClean);
    CHECK (out.stateL != (int) eb::RefMonState::GradedMarginal);
    CHECK (out.stateL != (int) eb::RefMonState::GradedSuspect);
    CHECK (out.thdL == 0.0f);
    CHECK (out.irSnrL == 0.0f);
}
