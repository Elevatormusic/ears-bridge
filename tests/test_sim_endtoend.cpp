#include <catch2/catch_test_macros.hpp>
#include "sim/VirtualSession.h"

// ==================================================================================================
// The synthetic end-to-end scenarios (spec: 2026-07-02-synthetic-measurement-rig-design, S1-S9).
// SEPARATION-ONLY assertions: relationships against the in-run clean baseline, never synthetic
// absolutes (the on-device ratification campaign owns absolute thresholds).
// ==================================================================================================

static ebsim::SessionSpec spec() { return {}; }

// The standard clean impairment: coupler IR + a very quiet (~-94 dBFS) room floor. The floor is
// deliberately BELOW realistic rooms (-60..-40 dBFS at the EARS): the rig's FIRST discovery (F1
// below) is that the v0.4.0 THD metric reads tens-of-percent on ANY realistic floor - reg-boosted
// out-of-band noise deconvolves asymmetrically into the negative-time THD region (the same #18
// physics, injected by noise instead of distortion). Until sub-project 2 (frequency-dependent
// regularization) fixes the root cause, the clean baseline must sit below that sensitivity.
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
// SIM F1 - THE RIG'S FIRST DISCOVERY (2026-07-02, found on the rig's first clean-session run):
// the v0.4.0 THD metric (mirror-compensated negative-time region energy, audit #18's fix) is NOT
// noise-robust. Broadband room noise deconvolves through the flat-regularized inverse filter with
// its out-of-band content boosted ~1/reg and lands ASYMMETRICALLY at negative time (the reference's
// >f2 spectral content lives at the sweep END, so boosted noise maps to negative lags) - directly
// inside the THD region, overwhelming the mirror compensation. Dose-response measured here:
// ~1.3% THD at a -94 dBFS floor, ~6.7% at -80, ~33% at -66, ~134% at -54. Real rooms are
// -60..-40 dBFS at the EARS, so ON-DEVICE THD READINGS WILL BE MEANINGLESS until fixed.
// THE ROOT FIX IS SUB-PROJECT 2 (frequency-dependent deconvolution regularization). When it lands,
// the EXPECT_INFLATED branch below must flip: a -66 dBFS floor should then read low single-digit
// THD, and this test becomes the regression proof that the regularization killed the sensitivity.
// ==================================================================================================
TEST_CASE("SIM F1: broadband noise inflates the THD metric (KNOWN DEFECT - motivates sub-project 2)") {
    auto quiet = ebsim::runVirtualSession (spec(), cleanImpair(), {});
    auto noisy = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m);
        ebsim::addSeededNoise (m, 0.0005f, 42u);           // ~-66 dBFS: QUIETER than most real rooms
    }, {});
    REQUIRE (quiet.learnOk); REQUIRE (noisy.learnOk);
    INFO ("quiet thd=" << quiet.thdL << "  noisy thd=" << noisy.thdL << "  noisy ir=" << noisy.irSnrL);
    // The characterized defect: a merely-noisy (NOT distorted) capture reads >10% THD while the IR
    // itself stays healthy. Flip these expectations when frequency-dependent regularization lands.
    CHECK (noisy.thdL > 10.0f);                            // EXPECT_INFLATED (defect pinned)
    CHECK (noisy.irSnrL > 20.0f);                          // the IR is fine - THD alone is the liar
    CHECK (quiet.thdL < 5.0f);                             // the metric behaves once noise vanishes
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
    // Verifier m1 follow-through, with an honest downgrade: monotonicity is NOT assertable while
    // the metric carries the F1 defect. Measured at full-scale drive both coefficients SATURATE
    // (~167%); at the native 0.5 drive the response INVERTS (a2=0.10 reads ~754%, a2=0.22 ~167%) -
    // the reg-boosted junk that dominates the region is not a monotone function of the distortion
    // coefficient. What IS assertable today: distortion is DETECTED (far above the clean baseline)
    // and gross distortion reads grossly. Monotonicity joins F1's flip-list for sub-project 2 -
    // once the frequency-dependent regularization lands, tighten this scenario to a real
    // coefficient-monotone escalation on the unsaturated slope.
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
    CHECK (gross.thdL > 10.0f);                                // gross distortion reads grossly
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
