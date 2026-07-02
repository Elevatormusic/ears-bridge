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
