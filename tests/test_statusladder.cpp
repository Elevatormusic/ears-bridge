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

TEST_CASE("StatusLadder #68: an over-budget advisory tail rides in the tooltip, never clips the line") {
    eb::RunningSnapshot s;
    s.advisoryTail = " - input, cable 16-bit - 24-bit+ recommended for the cleanest measurement";  // 74 chars
    const auto out = eb::runningStatus (s);                       // "clean so far" (Ok) headline
    CHECK_FALSE (out.line1.text.contains ("16-bit"));             // NOT appended (would overflow at min window)
    CHECK (out.line1.tip.contains ("16-bit"));                    // ...but fully present in the tooltip
    CHECK_FALSE (out.line1.tip.startsWith (" - "));               // tooltip form drops the tail's separator

    // A SHORT tail still decorates the line inline (the #49 behaviour, unchanged).
    s.advisoryTail = " - input is 16-bit";
    const auto short1 = eb::runningStatus (s);
    CHECK (short1.line1.text.endsWith (" - input is 16-bit"));
}

// ==================================================================================================
// SP3 — shapeInfoNote: the worst-offender INFO line for a per-ear shape anomaly (Task 5)
// ==================================================================================================
namespace ShapeFlag = eb::ShapeFlag;

TEST_CASE("shapeInfoNote: empty when no anomaly (flags 0 or only kBaselineSet)") {
    CHECK (eb::shapeInfoNote (0u, 0, 0, 0, 0, 0).isEmpty());
    // The baseline being learned is NOT a finding -> still empty.
    CHECK (eb::shapeInfoNote (ShapeFlag::kBaselineSet, 0, 0, 0, 0, 0).isEmpty());
}

TEST_CASE("shapeInfoNote: worst-offender precedence truncation>comb>polarity>drift>hum>resonance>skew>step") {
    const float dHz = 12000.0f, lHz = 200.0f;
    // Set EVERY anomaly bit at once; each removal should reveal the next in the precedence chain.
    unsigned all = ShapeFlag::kTruncHi | ShapeFlag::kTruncLo | ShapeFlag::kComb | ShapeFlag::kPolarity
                 | ShapeFlag::kDrift | ShapeFlag::kHum | ShapeFlag::kResonance | ShapeFlag::kSkew | ShapeFlag::kStep;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("content ends near"));        // TruncHi wins
    all &= ~ShapeFlag::kTruncHi;
    // SP3 final-verifier suppression (kShapeCopyRatified == false): kTruncLo's copy is GATED (it fired on
    // the rig's clean run at the provisional LF pivot), so precedence now SKIPS PAST it to Comb even
    // though the kTruncLo flag is set. Old pin (pre-suppression, restore when kShapeCopyRatified flips):
    //   CHECK (...contains ("no low-frequency content"));  // TruncLo
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("duplicate path"));           // TruncLo gated -> Comb
    all &= ~ShapeFlag::kTruncLo;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("duplicate path"));           // Comb
    all &= ~ShapeFlag::kComb;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("opposite polarity"));        // Polarity
    all &= ~ShapeFlag::kPolarity;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("response drifted"));         // Drift
    all &= ~ShapeFlag::kDrift;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("mains hum"));                // Hum
    all &= ~ShapeFlag::kHum;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("resonance"));                // Resonance
    all &= ~ShapeFlag::kResonance;
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).contains ("clock skew"));               // Skew
    all &= ~ShapeFlag::kSkew;
    // kStep's copy is GATED too (D7's ratio is confounded by the headphone envelope), so kStep-alone now
    // returns EMPTY. Old pin (restore when kShapeCopyRatified flips):
    //   CHECK (...contains ("level changed mid-sweep"));  // Step (last)
    CHECK (eb::shapeInfoNote (all, 5.0f, 5.0f, dHz, lHz, 60).isEmpty());                             // Step gated -> no note
}

TEST_CASE("shapeInfoNote: each line quantifies its finding and stays <= ~70 chars") {
    // The #8 lesson: no INFO line may overflow the header. Assert every single-flag note is short.
    // SP3 final-verifier suppression: kTruncLo and kStep copy is GATED (kShapeCopyRatified == false), so
    // their single-flag notes are EMPTY (numbers still ride shapeInfoTip — asserted separately below).
    // Old expectation (restore when kShapeCopyRatified flips): both were listed here and asserted non-empty.
    struct Case { unsigned flag; float drift; float delay; float hi; float lo; int hum; };
    const Case cases[] = {
        { ShapeFlag::kTruncHi, 0, 0, 12000.0f, 0, 0 },
        { ShapeFlag::kComb, 0, 5.3f, 0, 0, 0 },
        { ShapeFlag::kPolarity, 0, 0, 0, 0, 0 },
        { ShapeFlag::kDrift, 6.4f, 0, 0, 0, 0 },
        { ShapeFlag::kHum, 0, 0, 0, 0, 60 },
        { ShapeFlag::kResonance, 0, 0, 0, 0, 0 },
        { ShapeFlag::kSkew, 0, 0, 0, 0, 0 },
    };
    for (const auto& c : cases) {
        const auto note = eb::shapeInfoNote (c.flag, c.drift, c.delay, c.hi, c.lo, c.hum);
        INFO ("note = " << note.toStdString());
        CHECK (note.isNotEmpty());
        CHECK (note.length() <= 70);
    }
    // The two GATED findings produce NO note while unratified (the flags/numbers still publish; only the
    // accusatory one-liner is withheld pending #54C).
    CHECK (eb::shapeInfoNote (ShapeFlag::kTruncLo, 0, 0, 0, 180.0f, 0).isEmpty());
    CHECK (eb::shapeInfoNote (ShapeFlag::kStep,    0, 0, 0, 0, 0).isEmpty());
    // The quantified notes actually carry their number.
    CHECK (eb::shapeInfoNote (ShapeFlag::kTruncHi, 0, 0, 12000.0f, 0, 0).contains ("12.0 kHz"));
    CHECK (eb::shapeInfoNote (ShapeFlag::kComb, 0, 5.3f, 0, 0, 0).contains ("5.3 ms"));
    CHECK (eb::shapeInfoNote (ShapeFlag::kHum, 0, 0, 0, 0, 50).contains ("50 Hz"));
    // The gated numbers still ride the tooltip (kShapeCopyRatified withholds only the note, not the data).
    CHECK (eb::shapeInfoTip (ShapeFlag::kTruncLo, 0, 0, 0, 0, 180.0f, 0, 0, 0, 0, 0).contains ("180"));
    CHECK (eb::shapeInfoTip (ShapeFlag::kStep, 0, 0, 0, 0, 0, 0, 0, 2.5f, 0, 0).contains ("2.5"));
}

// MAJOR-1 (verifier gate): the spec §6 "no measurable band" finding is the loudest note.
TEST_CASE("shapeInfoNote: kNoBand copy + precedence over every other finding") {
    // The exact spec copy, and it stays within the line budget.
    const auto note = eb::shapeInfoNote (ShapeFlag::kNoBand, 0, 0, 0, 0, 0);
    CHECK (note == "no measurable response band - check the chain");
    CHECK (note.length() <= 70);
    // No-band OUTRANKS even truncation/comb (a bare truncation edge would understate a fully-empty band).
    unsigned withComb = ShapeFlag::kNoBand | ShapeFlag::kComb | ShapeFlag::kTruncHi;
    CHECK (eb::shapeInfoNote (withComb, 0, 5.3f, 12000.0f, 0, 0).contains ("no measurable response band"));
}

// MAJOR-2 (verifier gate): the numbers-bearing multi-line tooltip.
TEST_CASE("shapeInfoTip: empty on flags==0/baseline; carries the full numbers otherwise") {
    // Empty when there is no finding (bare baseline is not one).
    CHECK (eb::shapeInfoTip (0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0).isEmpty());
    CHECK (eb::shapeInfoTip (ShapeFlag::kBaselineSet, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0).isEmpty());

    // Drift: max delta + HF shelf, both quantified on their own line.
    const auto drift = eb::shapeInfoTip (ShapeFlag::kDrift, /*driftMax*/ 6.4f, /*hfShelf*/ -3.2f,
                                         0, 0, 0, 0, 0, 0, 0, 0);
    CHECK (drift.contains ("6.4"));
    CHECK (drift.contains ("-3.2"));

    // Comb: depth + delay. Every active flag contributes a line, so a multi-finding tip is multi-line.
    const auto multi = eb::shapeInfoTip (ShapeFlag::kComb | ShapeFlag::kHum,
                                         0, 0, /*combDepth*/ 8.0f, /*combDelay*/ 5.3f,
                                         0, 0, 0, 0, /*humBase*/ 50, 0);
    CHECK (multi.contains ("8.0"));
    CHECK (multi.contains ("5.3"));
    CHECK (multi.contains ("50"));
    CHECK (multi.contains ("\n"));   // one line per finding

    // Truncation edges + step + resonance + skew carry their measured numbers.
    CHECK (eb::shapeInfoTip (ShapeFlag::kTruncHi, 0, 0, 0, 0, 0, 12000.0f, 0, 0, 0, 0).contains ("12.00 kHz"));
    CHECK (eb::shapeInfoTip (ShapeFlag::kStep, 0, 0, 0, 0, 0, 0, 0, 2.5f, 0, 0).contains ("2.5"));
    CHECK (eb::shapeInfoTip (ShapeFlag::kResonance, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4200.0f).contains ("4200"));
    CHECK (eb::shapeInfoTip (ShapeFlag::kSkew, 0, 0, 0, 0, 0, 0, /*lobe*/ 14.0f, 0, 0, 0).contains ("14"));
}

// MAJOR-3 (verifier gate): a long verified line + tails + the LONGEST note stays under the line budget
// (the note elides with ".."); the full note/numbers move to the tooltip.
TEST_CASE("earStatusLine: a long-tails + drift-note composition stays within the length budget") {
    static constexpr int kBudget = 78;   // earStatusLine's #68 per-ear title-bar budget
    auto e = ear (RefMonState::GradedClean, /*ir*/ 56.0f, /*thd*/ 0.0f, /*sweepSnr*/ 30.0f, /*peak*/ -8.0f);
    e.shapeNote = "response drifted since sweep 1 (6.4 dB) - re-seat or check the chain";   // the longest copy
    e.shapeTip  = "Drift: max delta 6.4 dB vs sweep 1, HF shelf -3.2 dB";
    const auto out = eb::earStatusLine ("L", e);
    INFO ("line = " << out.text.toStdString() << " (len " << out.text.length() << ")");
    CHECK (out.text.length() <= kBudget);                 // never overflows the title-bar line
    CHECK (out.text.contains (".."));                     // elided, since the full note wouldn't fit
    CHECK (out.tip.contains ("HF shelf"));                // the full numbers rode into the tooltip
    // A note that DOES fit within the budget is appended WHOLE (no elision) and still carries the tip.
    // Use a peak below the -119 readout gate so the base carries no peak tail, then a short note that fits.
    auto e2 = ear (RefMonState::GradedClean, 56.0f, 0.0f, 30.0f, -120.0f);
    e2.shapeNote = "short note";
    e2.shapeTip  = "Full: short-note detail";
    const auto out2 = eb::earStatusLine ("L", e2);
    CHECK (out2.text.length() <= kBudget);
    CHECK (out2.text.contains ("short note"));
    CHECK_FALSE (out2.text.endsWith (".."));              // it fit, so no elision
    CHECK (out2.tip.contains ("short-note detail"));      // the tip still carries the full numbers
}
