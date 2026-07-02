#include <catch2/catch_test_macros.hpp>
#include "gui/StatusLadder.h"

// Headless tests for eb::runningStatus / earStatusLine / capturedHeadline — the PURE Running status
// ladder (#50). These pin the audit's honesty cluster:
//   #1  the captured headline is green ONLY when every graded ear is GradedClean (and none clipped);
//   #4  the captured branch surfaces BOTH ears (compact second line + full tooltips);
//   #21 the silent-input warning beats the captured/activity branches that used to shadow it;
//   #44 greenCaptureViolation() (the refMonBlocksGreen contract) holds across the whole state space.

using eb::RefMonState;
using eb::StatusTone;

static eb::EarGradeSnapshot ear (RefMonState s, float irSnr = 50.0f, float thd = 0.2f,
                                 float sweepSnr = 30.0f, float peakDb = -6.0f) {
    eb::EarGradeSnapshot e;
    e.state = (int) s; e.irSnrDb = irSnr; e.thdPercent = thd; e.sweepSnrDb = sweepSnr; e.peakDb = peakDb;
    return e;
}

static eb::RunningSnapshot capturedSnapshot (RefMonState l, RefMonState r) {
    eb::RunningSnapshot s;
    s.referenceLoaded = true;
    s.earL = ear (l);
    s.earR = ear (r);
    return s;
}

// ---- #1: the captured headline consults the worst graded state -----------------------------------

TEST_CASE("StatusLadder #1: both ears GradedClean -> the green safe-to-run headline") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedClean));
    CHECK (out.line1.tone == StatusTone::Ok);
    CHECK (out.line1.text.contains ("safe to run the next sweep"));
}

TEST_CASE("StatusLadder #1: one Clean + one ungraded ear stays green (the go-ahead for sweep two)") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::GradedClean, RefMonState::NotGraded));
    CHECK (out.line1.tone == StatusTone::Ok);
}

TEST_CASE("StatusLadder #1: a GradedSuspect ear makes the headline DANGER, never green") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedSuspect));
    CHECK (out.line1.tone == StatusTone::Danger);
    CHECK (out.line1.text.contains ("flagged"));
    CHECK_FALSE (out.line1.text.contains ("safe to run"));
}

TEST_CASE("StatusLadder #1: a GradedMarginal ear makes the headline WARN") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::GradedMarginal, RefMonState::GradedClean));
    CHECK (out.line1.tone == StatusTone::Warn);
    CHECK (out.line1.text.contains ("marginal"));
}

TEST_CASE("StatusLadder #1: a ReferenceStale ear makes the headline WARN (re-learn), never green") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::ReferenceStale, RefMonState::GradedClean));
    CHECK (out.line1.tone == StatusTone::Warn);
    CHECK (out.line1.text.contains ("re-learn"));
}

TEST_CASE("StatusLadder #1: Suspect outranks Stale outranks Marginal in the combined verdict") {
    CHECK (eb::runningStatus (capturedSnapshot (RefMonState::ReferenceStale, RefMonState::GradedSuspect))
               .line1.tone == StatusTone::Danger);
    CHECK (eb::runningStatus (capturedSnapshot (RefMonState::GradedMarginal, RefMonState::ReferenceStale))
               .line1.text.contains ("didn't match the reference"));
}

TEST_CASE("StatusLadder #1: a clipped (>= 0 dBFS) clean grade cannot read green") {
    auto s = capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedClean);
    s.earR.peakDb = 1.6f;
    const auto out = eb::runningStatus (s);
    CHECK (out.line1.tone == StatusTone::Warn);
    CHECK (out.line1.text.contains ("clipped"));
}

// ---- #4: both ears rendered in the captured branch ------------------------------------------------

TEST_CASE("StatusLadder #4: the captured second line carries BOTH ears") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::GradedClean, RefMonState::ReferenceStale));
    CHECK (out.line2.text.contains ("L:"));
    CHECK (out.line2.text.contains ("R:"));
    CHECK (out.line2.text.contains ("verified"));
    CHECK (out.line2.text.contains ("re-learn"));
    // The full per-ear detail rides in the tooltip (numbers + guidance for both ears).
    CHECK (out.line2.tip.contains ("IR-SNR"));
    CHECK (out.line2.tip.contains ("didn't match the learned reference"));
}

TEST_CASE("StatusLadder #4: the L ear's quantified clip cut is reachable while R holds a grade") {
    // The audit's exact complaint: once ANY grade existed, the old branch rendered only ear R —
    // the L ear's actionable clip cut was computed nowhere.
    auto s = capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedClean);
    s.earL.peakDb = 1.6f;   // ceil(1.6)+3 = 5 dB cut
    const auto out = eb::runningStatus (s);
    CHECK (out.line2.text.contains ("L: clipped +1.6"));
    CHECK (out.line2.text.contains ("lower ~5 dB"));
}

TEST_CASE("StatusLadder #4: the compact line carries the WORST ear's tone") {
    const auto out = eb::runningStatus (capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedSuspect));
    CHECK (out.line2.tone == StatusTone::Danger);
}

// ---- #21: silent input is no longer shadowed ------------------------------------------------------

TEST_CASE("StatusLadder #21: silent input beats the pre-sweep activity branch that shadowed it") {
    eb::RunningSnapshot s;
    s.referenceLoaded = true;
    s.phaseIdleOrPreflight = true;   // the branch that used to win ("Listening...")
    s.silentHold = true;
    const auto out = eb::runningStatus (s);
    CHECK (out.line1.text.contains ("no input signal"));
    CHECK (out.line1.tone == StatusTone::Warn);
}

TEST_CASE("StatusLadder #21: silent input beats a stale captured confirmation") {
    auto s = capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedClean);
    s.silentHold = true;
    CHECK (eb::runningStatus (s).line1.text.contains ("no input signal"));
}

TEST_CASE("StatusLadder #21: the hard-error cluster still beats silence") {
    eb::RunningSnapshot s;
    s.silentHold = true;
    s.cleanCapture = false; s.invalidMessage = "Dropouts detected - measurement invalid";
    CHECK (eb::runningStatus (s).line1.tone == StatusTone::Danger);
    s.cleanCapture = true;  s.outputClip = true;
    CHECK (eb::runningStatus (s).line1.text.contains ("Output clipping"));
}

// ---- #44: the refMonBlocksGreen contract, exhaustively --------------------------------------------

TEST_CASE("StatusLadder #44: no state combination produces a green headline over a blocking grade") {
    const RefMonState all[] = { RefMonState::NotLearned, RefMonState::Learned, RefMonState::NotGraded,
                                RefMonState::ReferenceStale, RefMonState::GradedClean,
                                RefMonState::GradedMarginal, RefMonState::GradedSuspect,
                                RefMonState::GradingOffHardware };
    for (auto l : all) for (auto r : all) {
        for (float peakL : { -6.0f, 1.6f }) for (float peakR : { -6.0f, 1.6f }) {
            auto s = capturedSnapshot (l, r);
            s.earL.peakDb = peakL; s.earR.peakDb = peakR;
            const auto out = eb::runningStatus (s);
            INFO ("L=" << (int) l << " R=" << (int) r << " peakL=" << peakL << " peakR=" << peakR);
            CHECK_FALSE (eb::greenCaptureViolation (out.line1.tone, s.earL, s.earR));
        }
    }
}

TEST_CASE("StatusLadder #44: a green PER-EAR line is only ever the GradedClean verdict") {
    const RefMonState all[] = { RefMonState::NotLearned, RefMonState::Learned, RefMonState::NotGraded,
                                RefMonState::ReferenceStale, RefMonState::GradedClean,
                                RefMonState::GradedMarginal, RefMonState::GradedSuspect,
                                RefMonState::GradingOffHardware };
    for (auto st : all) for (float peak : { -120.0f, -20.0f, -6.0f, -0.5f, 1.6f }) {
        const auto line = eb::earStatusLine ("L", ear (st, 50.0f, 0.2f, 30.0f, peak));
        INFO ("state=" << (int) st << " peak=" << peak);
        if (line.tone == StatusTone::Ok)
            CHECK (st == RefMonState::GradedClean);
    }
}

// ---- precedence pins (the full chain order) --------------------------------------------------------

TEST_CASE("StatusLadder precedence: invalid > live > output-clip > silent > low-SNR > rate veto > captured") {
    auto s = capturedSnapshot (RefMonState::GradedClean, RefMonState::GradedClean);
    s.invalidMessage = "invalid"; s.liveText = "live -12 dBFS";
    s.rateSummary = "Output is 44.1k - set everything to 48k";
    s.cleanCapture = false; s.liveOwns = true; s.outputClip = true; s.silentHold = true;
    s.lowSnrHold = true; s.snrL = 12.0f; s.rateVeto = true;

    CHECK (eb::runningStatus (s).line1.text == "invalid");
    s.cleanCapture = true;
    // rateVeto suppresses the live line (a level shown over a mis-rated chain can't be trusted).
    CHECK (eb::runningStatus (s).line1.text.contains ("Output clipping"));
    s.rateVeto = false;
    CHECK (eb::runningStatus (s).line1.tone == StatusTone::Live);
    s.liveOwns = false;
    CHECK (eb::runningStatus (s).line1.text.contains ("Output clipping"));
    s.outputClip = false;
    CHECK (eb::runningStatus (s).line1.text.contains ("no input signal"));
    s.silentHold = false;
    CHECK (eb::runningStatus (s).line1.text.contains ("Low SNR"));
    s.lowSnrHold = false; s.rateVeto = true;
    CHECK (eb::runningStatus (s).line1.text.contains ("44.1k"));
    s.rateVeto = false;
    CHECK (eb::runningStatus (s).line1.text.contains ("safe to run the next sweep"));
}

TEST_CASE("StatusLadder #15: the low-SNR warning names the offending ear from the per-ear stores") {
    eb::RunningSnapshot s;
    s.lowSnrHold = true; s.snrL = 12.0f; s.snrR = 31.0f; s.snrCombined = 31.0f;
    const auto out = eb::runningStatus (s);
    CHECK (out.line1.text.contains ("(L)"));
    CHECK (out.line1.text.contains ("12 dB"));
}

// ---- the non-captured branches --------------------------------------------------------------------

TEST_CASE("StatusLadder: pre-sweep activity wording (reference vs non-adopter)") {
    eb::RunningSnapshot s;
    s.phaseIdleOrPreflight = true;
    CHECK (eb::runningStatus (s).line1.text.contains ("waiting for the Dirac sweep"));
    s.referenceLoaded = true;
    CHECK (eb::runningStatus (s).line1.text.contains ("Listening for the Dirac sweep"));
    s.gradeSignalPresent = true;
    CHECK (eb::runningStatus (s).line1.text.contains ("Sweep in progress"));
}

TEST_CASE("StatusLadder: reference loaded + nothing graded -> full per-ear waiting lines") {
    eb::RunningSnapshot s;
    s.referenceLoaded = true;
    s.earL = ear (RefMonState::Learned, 0, 0, 0, -120.0f);
    s.earR = ear (RefMonState::Learned, 0, 0, 0, -120.0f);
    const auto out = eb::runningStatus (s);
    CHECK (out.line1.text == "L: waiting for the sweep");
    CHECK (out.line2.text == "R: waiting for the sweep");
}

TEST_CASE("StatusLadder: the Complete branch keeps its honest verdict mapping + OS-resample caveat") {
    eb::RunningSnapshot s;
    s.phaseComplete = true;
    s.earL = ear (RefMonState::GradedSuspect);
    CHECK (eb::runningStatus (s).line1.tone == StatusTone::Danger);
    s.earL = ear (RefMonState::NotLearned, 0, 0, 0, -120.0f);
    s.osResampled = true;
    const auto out = eb::runningStatus (s);
    CHECK (out.line1.text.contains ("no clipping or dropouts"));
    CHECK (out.line1.text.contains ("OS-resampled"));
    CHECK (out.line1.tone == StatusTone::Ok);
}

TEST_CASE("StatusLadder: the chain advisory decorates calm lines only") {
    eb::RunningSnapshot s;
    s.advisoryTail = " - input is 16-bit";
    auto out = eb::runningStatus (s);                    // "clean so far" (Ok) -> decorated
    CHECK (out.line1.text.endsWith (" - input is 16-bit"));
    s.outputClip = true;                                 // warn -> NOT decorated
    out = eb::runningStatus (s);
    CHECK_FALSE (out.line1.text.contains ("16-bit"));
}

TEST_CASE("StatusLadder: earStatusLine wording parity for the graded states") {
    const auto clean = eb::earStatusLine ("L", ear (RefMonState::GradedClean, 54.4f, 0.3f, 31.0f, -6.2f));
    // NOTE: juce::String(float, 0) = shortest representation (not zero decimals) — matches the old GUI code.
    CHECK (clean.text == "L: verified - IR-SNR 54 dB, THD 0% (calibration pending) (peak -6.2 dBFS)");
    CHECK (clean.tone == StatusTone::Ok);

    const auto stale = eb::earStatusLine ("R", ear (RefMonState::ReferenceStale, 0, 0, 0, -120.0f));
    CHECK (stale.text == "R: re-learn the reference");
    CHECK (stale.tone == StatusTone::Warn);

    const auto clipped = eb::earStatusLine ("L", ear (RefMonState::GradedClean, 54.0f, 0.3f, 31.0f, 1.6f));
    CHECK (clipped.text.contains ("clipped +1.6 dBFS - lower the output ~5 dB"));
    CHECK (clipped.tone == StatusTone::Warn);

    const auto lowSnr = eb::earStatusLine ("L", ear (RefMonState::GradedClean, 54.0f, 0.3f, 12.0f, -6.0f));
    CHECK (lowSnr.text.contains ("(low SNR)"));
}
