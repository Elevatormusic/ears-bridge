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
          << " shapeFlagsL=0x" << std::hex << out.shapeFlagsL << std::dec
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
// SIM F2 - THE RIG'S SECOND DISCOVERY (2026-07-02), now RESOLVED for the mid-SESSION case by the
// SP3 response-drift monitor. The original finding: an LTI spectral corruption (the historical
// tinny-EQ class - HF droop in the capture path) is INVISIBLE to the SINGLE-measurement quality
// machinery, because deconvolution ABSORBS any linear-time-invariant filtering into "the headphone
// IR" - a spectrally-wrong-but-clean measurement is mathematically indistinguishable from a
// different headphone. The pipeline grades signal QUALITY (match/noise/distortion), not spectral
// FIDELITY within one measurement.
//
// SP3's response-drift monitor (D1) closes this WHERE IT CAN be closed honestly: it learns a
// baseline IR-magnitude curve on the FIRST clean measurement, then flags BAND-LEVEL drift on later
// measurements in the SAME session. So a corruption that appears MID-SESSION (pass 1 clean, pass 2
// droop) is now VISIBLE - see "SIM F2 FLIP" below, the regression proof.
//
// The RESIDUAL that stays invisible - and MUST, honestly - is a droop present from the VERY FIRST
// measurement: there is no earlier clean baseline to compare against, so D1 has nothing to fire on
// (it LEARNS that first curve as the baseline). "SIM F2 NARROWED" pins that residual.
// ==================================================================================================
TEST_CASE("SIM F2 FLIP: mid-session LTI corruption is now VISIBLE (drift monitor)") {
    // THE SP3 regression proof. In single-pass F2 (below) an LTI HF droop is INVISIBLE. Here, with a
    // clean pass 1 that LEARNS the D1 baseline and a droop-corrupted pass 2 into the SAME engine (the
    // baseline SURVIVES - no re-prepare), the drift monitor makes the corruption VISIBLE.
    //
    // MEASURED (both passes GradedClean; the two-pass ring FLUSH lets pass 2 grade ITS OWN sweep):
    //   pass 1: kBaselineSet raised; the coupler IR's own per-measurement signatures also present
    //           (comb 0x2 from its early-reflection tap cluster @ ~0.46 ms, truncLo 0x8 from the LF band
    //           edge, step 0x100 from D7 on the tone->sweep envelope) - see the "coupler IR is not
    //           shape-neutral" note on the negatives test; those are IDENTICAL across both passes, so
    //           they are NOT drift and do not confound the D1 verdict.
    //   pass 2 (6 kHz droop): hfShelf2L = -4.44 dB (the tinny signature, clearly < -3), driftMax2L =
    //           7.86 dB (clearly > 3) - the corruption is now MEASURED, where single-pass F2 read it as
    //           a different-but-clean headphone. The D1 SCALARS are the load-bearing visibility proof.
    //
    // The kDrift BIT is dose-dependent on the provisional #54C tolerance envelope (<=4k 3 dB, <=8k 5 dB,
    // <=14k 8 dB; >14k report-only). A 6 kHz ONE-POLE droop is only 6 dB/oct, so its sub-14 kHz deltas
    // sit JUST inside the envelope and kDrift stays 0; a harsher droop trips it. Measured dose-response:
    //     fc=6 kHz: kDrift=0  hfShelf=-4.44  driftMax= 7.86
    //     fc=4 kHz: kDrift=1  hfShelf=-6.84  driftMax=11.08
    //     fc=3 kHz: kDrift=1  hfShelf=-8.68  driftMax=13.47
    //     fc=2 kHz: kDrift=1  hfShelf=-11.23 driftMax=16.91
    // So the monitor's boolean WORKS (it fires on a brutal corruption); the 6 kHz stimulus is simply at
    // the envelope edge. We pin BOTH: the 6 kHz visibility via the scalars (the plan's -3 / +3 lines) AND
    // that kDrift fires once the drift clears the envelope (fc=4 kHz) - the honest closure of the F2 gap.
    auto tp = ebsim::runVirtualSessionTwoPass (spec(),
        cleanImpair(),
        [] (ebsim::StereoTimeline& m) {
            ebsim::convolveIr (m); ebsim::applyHfDroop (m, 6000.0f);
            ebsim::addSeededNoise (m, 0.00002f, 42u);
        }, {});
    REQUIRE (tp.first.learnOk); REQUIRE (tp.second.learnOk);
    INFO ("pass1: state=" << tp.first.stateL << " flags1L=0x" << std::hex << tp.shapeFlags1L << std::dec
          << " | pass2: state=" << tp.second.stateL << " flags2L=0x" << std::hex << tp.shapeFlags2L << std::dec
          << " hfShelf2L=" << tp.hfShelf2L << " driftMax2L=" << tp.driftMax2L);
    // Pass 1 (clean) LEARNS the baseline.
    CHECK ((tp.shapeFlags1L & eb::ShapeFlag::kBaselineSet) != 0u);
    // Pass 2 (droop) DRIFTS against the pass-1 baseline: a NEGATIVE HF shelf (tinny) and a max band
    // delta well past a few dB. THE regression proof - the drift monitor now MEASURES the F2 corruption.
    CHECK (tp.hfShelf2L  < -3.0f);
    CHECK (tp.driftMax2L >  3.0f);
    // ...and a harsher droop (fc=4 kHz) clears the provisional envelope and raises the kDrift BIT.
    auto harsh = ebsim::runVirtualSessionTwoPass (spec(), cleanImpair(),
        [] (ebsim::StereoTimeline& m) { ebsim::convolveIr (m); ebsim::applyHfDroop (m, 4000.0f);
                                        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (harsh.second.learnOk);
    INFO ("harsh(4k): flags2L=0x" << std::hex << harsh.shapeFlags2L << std::dec
          << " hfShelf2L=" << harsh.hfShelf2L << " driftMax2L=" << harsh.driftMax2L);
    CHECK ((harsh.shapeFlags2L & eb::ShapeFlag::kDrift) != 0u);
    CHECK (harsh.hfShelf2L < -3.0f);
}

TEST_CASE("SIM shape negatives: two clean passes raise NO DRIFT (D1 monitor, CLAUDE.md rule 1)") {
    // CLAUDE.md rule 1 (test the negative): a genuinely clean session, measured TWICE, must NOT
    // manufacture a DRIFT. Pass 1 learns the baseline; pass 2 compares clean-vs-clean.
    //
    // SCOPE NOTE - the D1 drift monitor is what this negative targets, NOT every shape bit. The synthetic
    // COUPLER IR (convolveIr) is NOT shape-neutral: its early-reflection tap cluster reads as a mild comb
    // (kComb, delay ~0.46 ms), its LF band edge trips kTruncLo, and D7 flags a step on the tone->sweep
    // envelope - MEASURED clean flags = 0x30a (comb|truncLo|step|baselineSet). Those are PER-MEASUREMENT
    // signatures of the FIXED coupler IR, IDENTICAL in every pass, so they are not drift and are not what
    // the two-pass monitor watches. The load-bearing negative: clean-vs-clean raises NO kDrift and the D1
    // drift scalar stays ~0 (measured driftMax2L = 0.00), so the monitor never fabricates a corruption
    // where there is none.
    auto tp = ebsim::runVirtualSessionTwoPass (spec(), cleanImpair(), cleanImpair(), {});
    REQUIRE (tp.first.learnOk); REQUIRE (tp.second.learnOk);
    INFO ("neg pass1 flags=0x" << std::hex << tp.shapeFlags1L << " pass2 flags=0x" << tp.shapeFlags2L
          << std::dec << " driftMax2L=" << tp.driftMax2L << " hfShelf2L=" << tp.hfShelf2L);
    CHECK ((tp.shapeFlags1L & eb::ShapeFlag::kBaselineSet) != 0u);    // pass 1 learned the baseline
    CHECK ((tp.shapeFlags2L & eb::ShapeFlag::kDrift) == 0u);          // pass 2: clean-vs-clean, NO drift bit
    CHECK (std::abs (tp.driftMax2L) < 1.0f);                          // and the D1 scalar stays ~0
    // The per-measurement coupler-IR bits are STABLE across passes (not manufactured by the second run):
    CHECK ((tp.shapeFlags1L & ~eb::ShapeFlag::kBaselineSet) == (tp.shapeFlags2L & ~eb::ShapeFlag::kBaselineSet));
}

TEST_CASE("SIM shape comb: an echo raises kShapeComb on the first pass") {
    // A -10 dB echo at 5 ms -> comb spacing 200 Hz. D2's cepstrum + envelope arm should catch it on a
    // SINGLE pass (no baseline needed - comb is intrinsic to one IR).
    auto out = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::addEcho (m, 5.0, -10.0f);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (out.learnOk);
    INFO ("comb: state=" << out.stateL << " flagsL=0x" << std::hex << out.shapeFlagsL << std::dec
          << " combDelayMs=" << out.combDelayL);
    CHECK ((out.shapeFlagsL & eb::ShapeFlag::kComb) != 0u);
    CHECK (out.combDelayL > 4.0f); CHECK (out.combDelayL < 6.0f);    // recovered delay near 5 ms
}

// ==================================================================================================
// SIM F3 - THE RIG'S THIRD DISCOVERY (2026-07-03). The plan predicted a 12 kHz brickwall would raise
// kShapeTruncHi. It does NOT, end-to-end - and the reason is a genuine pipeline property the synthetic
// D3 unit tests could not surface. D3's HF arm requires BOTH a >= 30 dB drop within 1/3 oct AND a FLAT
// (<= 6 dB range) plateau above the edge (the plateau is what separates a digital cliff from a steep
// acoustic rolloff). Measured through the REAL deconvolver (drop / plateau-range, both channels, four
// cutoffs):
//     cut  8 kHz: effHi= 8004, drop=81.0 dB, plateauRange=26.8 dB
//     cut 10 kHz: effHi=10002, drop=89.3 dB, plateauRange=12.0 dB
//     cut 12 kHz: effHi=12041, drop=84.1 dB, plateauRange=11.1 dB
//     cut 14 kHz: effHi=14036, drop=81.3 dB, plateauRange= 8.7 dB
// The DROP is always huge (the cliff is unmistakable) and the effective HF edge is recovered to within
// ~0.4 % - so the truncation is QUANTITATIVELY DETECTED. But the plateau above the cliff is NOT flat:
// deconvolution's 1/|Ref|^2 boosts the sub-floor numerical residual UNEVENLY (the reference's own HF
// rolloff varies bin-to-bin), leaving 8.7-26.8 dB of ripple where a synthetic 1e-12 floor is flat. So
// the flatness gate - validated only against synthetic flat floors (test_responseshape) - never fires
// through the real chain. This is NOT a defect to tune around (lowering the flatness gate would let
// deconv ripple false-fire on clean measurements); it is a HONEST end-to-end limit of the plateau
// discriminator, pinned here so its eventual on-device closure (#54C) is measurable. The effHi edge IS
// the reliable truncation signal end-to-end; the boolean HF flag is a synthetic-only capability today.
// ==================================================================================================
TEST_CASE("SIM F3: a brickwall's HF edge is measured but kShapeTruncHi stays silent (deconv-ripple GAP, pinned)") {
    // The brickwall is applied LAST (after the room floor): a real SRC / Bluetooth codec truncates the
    // ENTIRE downstream chain, its own noise included - so nothing (signal or floor) survives the cliff.
    auto out = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::addSeededNoise (m, 0.00002f, 42u);
        ebsim::applyBrickwallLowpass (m, 12000.0); }, {});
    REQUIRE (out.learnOk);
    INFO ("trunc: state=" << out.stateL << " flagsL=0x" << std::hex << out.shapeFlagsL << std::dec
          << " effHiHz=" << out.effHiL);
    // The truncation IS measured: the recovered HF band edge lands within 1/3 oct of the 12 kHz cliff.
    CHECK (out.effHiL > 12000.0f / 1.26f); CHECK (out.effHiL < 12000.0f * 1.26f);   // 1/3 oct = x1.26
    // ...but the boolean flag stays silent through the real deconv chain (the pinned GAP). If this
    // starts FAILING, D3's plateau test has gained real-chain sensitivity - flip it to a positive
    // "kShapeTruncHi fires" pin and celebrate (mirrors the F1/F2 resolution pattern).
    CHECK ((out.shapeFlagsL & eb::ShapeFlag::kTruncHi) == 0u);
}

TEST_CASE("SIM shape polarity: an inverted earcup raises kShapePolarity after both ears grade") {
    // Invert ONE ear (R): D4's cross-ear IR cross-correlation flips sign -> kPolarity on BOTH ears once
    // both have a fresh matched segment this session.
    auto out = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::invertPolarity (m, 1);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (out.learnOk);
    INFO ("polarity: stateL=" << out.stateL << " stateR=" << out.stateR
          << " flagsL=0x" << std::hex << out.shapeFlagsL << " flagsR=0x" << out.shapeFlagsR << std::dec);
    CHECK ((out.shapeFlagsL & eb::ShapeFlag::kPolarity) != 0u);
    CHECK ((out.shapeFlagsR & eb::ShapeFlag::kPolarity) != 0u);
}

TEST_CASE("SIM shape hum: mains hum in the pre-sweep noise raises kShapeHum") {
    // 60 Hz hum + harmonics riding the leading noise region -> D5a's Welch PSD detects the line family.
    auto out = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::addMainsHum (m, 60.0, 0.003f);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (out.learnOk);
    INFO ("hum: state=" << out.stateL << " flagsL=0x" << std::hex << out.shapeFlagsL << std::dec
          << " humBaseHz=" << out.humBaseL);
    CHECK ((out.shapeFlagsL & eb::ShapeFlag::kHum) != 0u);
    CHECK (out.humBaseL == 60);
}

// ==================================================================================================
// SIM F4 - THE RIG'S FOURTH DISCOVERY (2026-07-03). The plan predicted a mid-sweep +3 dB gain step
// would raise kShapeStep as the D7 time-variance signature. Two measured facts contradict a naive
// injected-step demonstration through THIS pipeline:
//   (1) D7 ALREADY fires kStep on the clean COUPLER measurement: an ESS swept through the frequency-
//       shaping coupler IR has a per-block RMS envelope that differs from the flat reference, so the
//       capture/reference ratio ramps across the sweep. Measured clean stepDb = 11.4 dB (that is the
//       coupler envelope, not a level jump) - this is a real production D7 sensitivity the rig surfaces.
//   (2) applyGainStepAt is INVISIBLE to D7 through this pipeline. Measured, on a FLAT IR (coupler
//       removed, so the coupler ramp cannot mask it) the D7 stepDb is 0.0 dB for a +0, +3, +6 AND +12
//       dB injected step - the flag never even sets. On the coupler IR the reported stepDb is a flat
//       11.4 dB across +3/+6/+12 dB injected steps (identical) - the coupler ramp is the max |diff| D7
//       reports and the injected step never exceeds it.
// The injected step lands mid-L-sweep, inside the graded segment; why D7's median-split does not
// resolve it here (block-boundary geometry vs the FIFO-primed / match-aligned window, or the step's
// energy spreading under deconvolution alignment) is UNRESOLVED and needs a focused D7-through-pipeline
// investigation - it is NOT tuned around here. This test PINS the two measured facts so the gap is
// documented; the D7 unit test (test_responseshape) still proves the detector fires on a clean aligned
// pair, so this is a pipeline-integration gap, not a broken detector.
// ==================================================================================================
TEST_CASE("SIM F4: D7 step fires on the coupler envelope but does not resolve an injected level step (pinned)") {
    ebsim::SessionSpec sp = spec();
    const double stepAt = sp.toneSeconds + sp.gapSeconds + sp.sweepSeconds * 0.5;   // mid the FIRST (L) sweep
    // (1) The clean coupler measurement already raises kStep, from the sweep's spectral-envelope RMS ramp.
    auto clean = ebsim::runVirtualSession (sp, cleanImpair(), {});
    REQUIRE (clean.learnOk);
    INFO ("clean coupler: flagsL=0x" << std::hex << clean.shapeFlagsL << std::dec << " stepDb=" << clean.stepL);
    CHECK ((clean.shapeFlagsL & eb::ShapeFlag::kStep) != 0u);
    CHECK (std::abs (clean.stepL) > 2.0f);                            // the coupler envelope reads as a step
    // (2) An injected +3 dB step through the coupler does NOT push stepDb beyond that coupler baseline
    //     (the injected step is not separately resolved by D7 here - the documented gap).
    auto stepped = ebsim::runVirtualSession (sp, [stepAt] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::applyGainStepAt (m, stepAt, +3.0f);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (stepped.learnOk);
    INFO ("stepped(+3): flagsL=0x" << std::hex << stepped.shapeFlagsL << std::dec << " stepDb=" << stepped.stepL);
    CHECK (std::abs (stepped.stepL - clean.stepL) < 1.0f);           // the injected step barely moves D7's reading
    // (3) On a FLAT IR (coupler removed) the injected step is fully invisible: no kStep, stepDb == 0. If
    //     this starts FAILING, D7 has gained real injected-step sensitivity end-to-end - flip to a
    //     positive pin and celebrate (mirrors the F1/F2 resolution pattern).
    auto flat = ebsim::runVirtualSession (sp, [stepAt] (ebsim::StereoTimeline& m) {
        ebsim::applyGainStepAt (m, stepAt, +6.0f); ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (flat.learnOk);
    INFO ("flatIR(+6): flagsL=0x" << std::hex << flat.shapeFlagsL << std::dec << " stepDb=" << flat.stepL << " state=" << flat.stateL);
    CHECK ((flat.shapeFlagsL & eb::ShapeFlag::kStep) == 0u);
    CHECK (flat.stepL == 0.0f);
}

TEST_CASE("SIM F2 NARROWED (documented residual): a first-measurement droop stays invisible") {
    // The HONEST residual of the F2 gap: a droop present from the VERY FIRST measurement has NO earlier
    // clean baseline to drift against - D1 LEARNS that first curve as the baseline, so kDrift cannot
    // fire, and a 1-pole 6 kHz droop is a 6 dB/oct SLOPE (not a >=30 dB digital cliff with a plateau),
    // so D3's HF arm does not read it as truncation either. The measurement grades CLEAN (the ABSORBED
    // filtering is indistinguishable from a different headphone). This is not a defect: within ONE
    // measurement, spectral fidelity is unknowable. The mid-session case IS caught - see "SIM F2 FLIP".
    auto out = ebsim::runVirtualSession (spec(), [] (ebsim::StereoTimeline& m) {
        ebsim::convolveIr (m); ebsim::applyHfDroop (m, 6000.0f);
        ebsim::addSeededNoise (m, 0.00002f, 42u); }, {});
    REQUIRE (out.learnOk);
    INFO ("narrowed: state=" << out.stateL << " flagsL=0x" << std::hex << out.shapeFlagsL << std::dec
          << " driftMaxL=" << out.driftMaxL << " effHiL=" << out.effHiL);
    CHECK (out.stateL == (int) eb::RefMonState::GradedClean);         // still grades clean (absorbed)
    CHECK ((out.shapeFlagsL & eb::ShapeFlag::kDrift)    == 0u);       // no baseline delta on the first pass
    CHECK ((out.shapeFlagsL & eb::ShapeFlag::kTruncHi)  == 0u);       // a slope is not a cliff
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
