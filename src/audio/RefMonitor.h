#pragma once
#include <cmath>
#include <vector>

#include "audio/Deconvolver.h"        // deconvolve / referenceMatches (Task 1)
#include "gui/IrQualityStatus.h"      // evaluateIr / IrQuality (Task 2)

// Reference-Based Measurement Monitor — Task 4: the pure glue that turns the
// Task 1 (match-gate) + Task 2 (IR quality) pieces into a per-measurement verdict
// and a workflow state machine.
//
// Everything here is PURE (no JUCE GUI, no platform, no audio-thread state) so the
// transitions and the grading can be exhaustively unit-tested with synthetic ESS +
// IR data. The engine calls gradeMeasurement() OFF the audio thread (on a worker /
// at the completed-sweep edge) and publishes the verdict via atomics; the GUI maps
// the resulting RefMonState onto the status-line ladder.
namespace eb {

// ---- Farina harmonic offsets (pure) --------------------------------------
// In an exponential-sine-sweep (ESS) deconvolution the k-th harmonic impulse lands
// at a known offset BEFORE the linear (main) impulse:
//     dt_k = T * ln(k) / ln(w2 / w1)   seconds   (Farina 2000)
// with T = sweepLengthSamples / sampleRate the sweep duration, w1 = 2*pi*f1, w2 =
// 2*pi*f2 the start/stop angular frequencies. Converted to SAMPLES it is
//     off_k = round( sampleRate * T * ln(k) / ln(w2/w1) )
//           = round( sweepLengthSamples * ln(k) / ln(f2/f1) )         (sampleRate cancels)
// which is what evaluateIr() (Task 2) wants as harmonicOffsetsSamples (offsets
// before the peak). Returns the offsets for k = 2..maxHarmonic (default 2,3,4),
// dropping any that are non-positive or >= sweepLengthSamples (out of range).
// Pure: a few std::log + round, no allocation beyond the small result vector.
[[nodiscard]] inline std::vector<int> farinaHarmonicOffsets (int sweepLengthSamples,
                                                             double sampleRate,
                                                             double f1 = 20.0,
                                                             double f2 = 20000.0,
                                                             int maxHarmonic = 4) {
    std::vector<int> offsets;
    if (sweepLengthSamples <= 0 || sampleRate <= 0.0 || f1 <= 0.0 || f2 <= f1 || maxHarmonic < 2)
        return offsets;
    const double lnR = std::log (f2 / f1);                         // ln(w2/w1) == ln(f2/f1)
    if (lnR <= 0.0) return offsets;
    // off_k = round( fs * T * ln(k)/lnR ) with T = N/fs -> the rate cancels exactly:
    //         off_k = round( N * ln(k) / lnR ). Compute the cancelled form so the result is
    // sample-rate-independent to the last ULP (a two-step fs*T*…*fs round-trips inexactly).
    for (int k = 2; k <= maxHarmonic; ++k) {
        const double offD   = (double) sweepLengthSamples * std::log ((double) k) / lnR;
        const int    offSamp = (int) std::lround (offD);           // samples before the main impulse
        if (offSamp > 0 && offSamp < sweepLengthSamples)
            offsets.push_back (offSamp);
    }
    return offsets;
}

// ---- 3-color measurement-quality bands (pure; PROVISIONAL thresholds) -----
// sweepSNR is the TRUST GATE (Dirac "20 dB good" + REW; on-device clean 22-36 / noisy 5-18). IR-SNR +
// THD are ADVISORY (see aggregateVerdict). Thresholds are on-device-informed, not literature: Müller &
// Massarani show ESS deconvolution is noise-IMMUNE, so IR-SNR can't sense capture noise -> sweepSNR gates.
enum class QualityBand { Unknown, Green, Orange, Red };

static constexpr float kSweepSnrGoodDb = 25.0f;   // >= -> green
static constexpr float kSweepSnrBadDb  = 18.0f;   // <  -> red ; [18,25) -> orange
static constexpr float kIrSnrGoodDb    = 50.0f;   // anchored to the 54-66 dB clean band (mean-power-gate INR)
static constexpr float kIrSnrBadDb     = 35.0f;
static constexpr float kThdGoodPct     = 3.0f;    // relaxed from 1% (LF driver THD is legitimately a few %)
static constexpr float kThdBadPct      = 10.0f;   // > -> clipping territory (REW)

// Ascending edge arrays + hysteresis margins for the GUI's BandHysteresis (audio/Hysteresis.h) smoothing.
// The classifiers below are the canonical hard boundary; the smoothed GUI band agrees except at the exact
// edge value, which a measured float never lands on.
static constexpr float kSweepSnrEdges[] = { kSweepSnrBadDb, kSweepSnrGoodDb };   // {18,25}: 0=Red 1=Orange 2=Green
static constexpr float kIrSnrEdges[]    = { kIrSnrBadDb, kIrSnrGoodDb };         // {35,50}: 0=Red 1=Orange 2=Green
static constexpr float kThdEdges[]      = { kThdGoodPct, kThdBadPct };           // {3,10}:  0=Green 1=Orange 2=Red
static constexpr float kSnrHystDb       = 1.0f;
static constexpr float kThdHystPct      = 0.5f;

[[nodiscard]] inline QualityBand classifySweepSnr (float db, bool valid) noexcept {
    if (! valid)               return QualityBand::Unknown;   // unmeasurable -> never penalize
    if (db >= kSweepSnrGoodDb) return QualityBand::Green;
    if (db <  kSweepSnrBadDb)  return QualityBand::Red;
    return QualityBand::Orange;
}
[[nodiscard]] inline QualityBand classifyIrSnr (float db) noexcept {
    if (db >= kIrSnrGoodDb) return QualityBand::Green;
    if (db <  kIrSnrBadDb)  return QualityBand::Red;
    return QualityBand::Orange;
}
[[nodiscard]] inline QualityBand classifyThd (float pct) noexcept {
    if (pct <= kThdGoodPct) return QualityBand::Green;
    if (pct >  kThdBadPct)  return QualityBand::Red;
    return QualityBand::Orange;
}

// Map a BandHysteresis index (0..2) to a QualityBand. SNR metrics: higher index = better; THD: reversed.
[[nodiscard]] inline QualityBand snrBandFromIndex (int i) noexcept {
    return i <= 0 ? QualityBand::Red : (i == 1 ? QualityBand::Orange : QualityBand::Green);
}
[[nodiscard]] inline QualityBand thdBandFromIndex (int i) noexcept {
    return i <= 0 ? QualityBand::Green : (i == 1 ? QualityBand::Orange : QualityBand::Red);
}

// ---- The workflow state machine (pure) -----------------------------------
// The honest precedence is: NO reference -> never green; reference present but the
// match-gate FAILED -> "re-learn", never a quality number; matched + low quality ->
// suspect; matched + clean -> verified; reference present but NOT yet measured ->
// "not graded". The "still in Windows Audio during a learn" case is surfaced by the
// GUI read-only (it never changes the grading verdict), so it is NOT a state here.
enum class RefMonState {
    NotLearned,      // no reference has been learned yet -> "not graded - learn a reference first"
    Learned,         // a reference is loaded, but no measurement has been graded against it yet
    ReferenceStale,  // a measurement ran but the match-gate FAILED -> "doesn't match - re-learn"
    GradedClean,     // matched AND quality OK -> let the green "captured" branch show (verified)
    GradedMarginal,  // matched AND sweepSNR marginal (orange) -> usable, cautioned; blocks green
    GradedSuspect,   // matched AND low quality -> show irQualityNote (guidance warn)
    NotGraded,       // a reference is loaded but this run produced no gradeable measurement yet
    GradingOffHardware  // Dirac on a hardware box: no loopback reference -> grading off (calm/neutral, NOT a warn)
};

// Inputs to the transition: the bare booleans the engine/GUI can supply lock-free.
struct RefMonInputs {
    bool referenceLoaded = false;   // a validated reference has been learned/stored
    bool measured        = false;   // a measurement (a completed sweep) has been graded this run
    bool matched         = false;   // the match-gate verdict for that measurement
    bool lowQuality      = false;   // the IR-quality verdict for that measurement (only if matched)
};

// Pure transition. Truth table (the order is the honesty contract):
//   !referenceLoaded                      -> NotLearned   (NEVER green; learn first)
//   referenceLoaded && !measured          -> Learned      (reference present, nothing measured yet)
//   measured && !matched                  -> ReferenceStale (gate failed -> re-learn; NOT a grade)
//   measured && matched && lowQuality      -> GradedSuspect  (guidance warn with the numbers)
//   measured && matched && !lowQuality     -> GradedClean    (verified; green allowed)
// NotGraded is reserved for "reference loaded, run in progress, no completed sweep yet" — the GUI
// uses it interchangeably with Learned for the dim "not graded" copy; nextState never invents it
// (Learned covers the loaded-but-unmeasured case), but it exists so the engine can publish a
// distinct "reference present, this run not graded" snapshot if it wants. Honest by construction:
// it can NEVER return GradedClean unless matched==true AND lowQuality==false.
[[nodiscard]] inline RefMonState nextRefMonState (const RefMonInputs& in) noexcept {
    if (! in.referenceLoaded) return RefMonState::NotLearned;
    if (! in.measured)        return RefMonState::Learned;
    if (! in.matched)         return RefMonState::ReferenceStale;   // gate FIRST, before any quality
    return in.lowQuality ? RefMonState::GradedSuspect : RefMonState::GradedClean;
}

// True iff the state is one a user must NOT read as a clean/green capture. The GUI ladder asserts on
// this so a refactor can never let "captured" show over a not-graded / mismatched / suspect state.
[[nodiscard]] inline bool refMonBlocksGreen (RefMonState s) noexcept {
    return s == RefMonState::NotLearned    || s == RefMonState::Learned
        || s == RefMonState::NotGraded     || s == RefMonState::ReferenceStale
        || s == RefMonState::GradedMarginal || s == RefMonState::GradedSuspect
        || s == RefMonState::GradingOffHardware;
}

// ---- 3-color verdict (pure): sweepSNR GATES; THD escalates only at RED; IR-SNR ADVISORY ----------
// The traffic light. GradedClean=green, GradedMarginal=orange, GradedSuspect=red; everything else
// (not-learned / loaded / not-graded / re-learn) is Neutral with its own copy.
enum class RefMonColour { Neutral, Green, Orange, Red };
[[nodiscard]] inline RefMonColour refMonColour (RefMonState s) noexcept {
    switch (s) {
        case RefMonState::GradedClean:    return RefMonColour::Green;
        case RefMonState::GradedMarginal: return RefMonColour::Orange;
        case RefMonState::GradedSuspect:  return RefMonColour::Red;
        default:                          return RefMonColour::Neutral;
    }
}

struct QualityVerdict {
    RefMonState state        = RefMonState::NotGraded;
    QualityBand sweepSnrBand = QualityBand::Unknown;
    QualityBand irSnrBand    = QualityBand::Unknown;   // display only — never gates the headline
    QualityBand thdBand      = QualityBand::Unknown;
};

// Combine the three metrics into the headline state. Match-gate FIRST (a non-match is "re-learn", never a
// quality number). Then sweepSNR drives the headline; THD escalates ONLY at red (Farina makes THD
// non-corrupting, so mild THD must not veto); IR-SNR is advisory (its band is reported, never gates).
// An UNVERIFIABLE sweepSNR (no usable noise region) cannot confirm clean -> Marginal, never Clean (integrity).
[[nodiscard]] inline QualityVerdict aggregateVerdict (bool matched, float sweepSnrDb, bool sweepSnrValid,
                                                      float irSnrDb, float thdPct) noexcept {
    QualityVerdict v;
    if (! matched) { v.state = RefMonState::ReferenceStale; return v; }   // gate first; bands stay Unknown

    v.sweepSnrBand = classifySweepSnr (sweepSnrDb, sweepSnrValid);
    v.irSnrBand    = classifyIrSnr (irSnrDb);     // advisory readout only
    v.thdBand      = classifyThd (thdPct);

    RefMonState s = RefMonState::GradedClean;
    // sweepSNR is the ONLY trust gate (IR-SNR is advisory, THD ~0 for non-harmonic noise). If it could NOT be
    // measured (sweepSnrValid==false / Unknown band: no usable noise region in the window), we cannot CONFIRM the
    // capture is clean -> fall to Marginal, never Clean. (The prior "Unknown stays Clean" was a false-green path:
    // a colored/tonal noisy capture with no separable noise region read green. Review 2026-06-23.)
    if (! sweepSnrValid || v.sweepSnrBand == QualityBand::Orange) s = RefMonState::GradedMarginal;
    if (v.sweepSnrBand == QualityBand::Red)                       s = RefMonState::GradedSuspect;
    if (v.thdBand      == QualityBand::Red)                       s = RefMonState::GradedSuspect;   // worst-wins escalate
    v.state = s;
    return v;
}

// GUI status string for the verdict (builds a juce::String — message-thread only; mirrors irQualityNote).
// Precedence = worst gating offender first; the advisory IR-SNR note shows only when nothing else fired.
[[nodiscard]] inline juce::String qualityNote (const QualityVerdict& v) {
    if (v.state == RefMonState::ReferenceStale)
        return "Reference doesn't match your sweep - re-learn it.";
    // sweepSNR predicts Dirac's "imprecise" verdict: low capture SNR corrupts the excess-phase estimate, so
    // Dirac drops the position from phase correction (research-confirmed; the fix is more SNR, not re-measuring
    // blindly). Name the verdict so the grade is actionable.
    if (v.sweepSnrBand == QualityBand::Red)     return "Noisy capture - Dirac will likely mark it imprecise; raise level or lower noise.";
    if (v.thdBand      == QualityBand::Red)     return "High distortion - check level/seal.";
    if (v.sweepSnrBand == QualityBand::Orange)  return "Marginal SNR - Dirac may mark it imprecise; raise level or lower noise.";
    if (v.sweepSnrBand == QualityBand::Unknown) return "Couldn't verify SNR (no quiet region in the capture) - re-measure.";
    if (v.irSnrBand    == QualityBand::Red)     return "Weak impulse response - possible time-variance or reference issue.";
    return {};   // all clear
}

// ---- Per-measurement grading (pure; the engine's worker calls this) -------
struct MeasurementGrade {
    RefMonState state   = RefMonState::NotGraded;
    MatchVerdict match;    // the Task 1 gate verdict (coherence / main-lobe)
    IrQuality    quality;  // the Task 2 verdict — VALID ONLY when match.matched (else all-zero)
};

// Grade ONE completed measurement against the learned reference. The order is the
// honesty contract from the spec (§5.2): the MATCH-GATE runs FIRST; only if it
// passes do we deconvolve+grade quality. On a non-match we return ReferenceStale
// with NO quality number (quality stays default/zero, matched==false) — a wrong
// reference is "re-learn", never "bad measurement".
//
//   reference / response : the learned ESS reference and this measurement's mic
//                          response, same length n (per-segment, already aligned —
//                          see Deconvolver's per-segment note).
//   sampleRate / f1 / f2 : the reference sweep params, used for the Farina offsets.
//   minIrSnrDb / maxThdPct : the quality cutoffs (default the PROVISIONAL Task-2
//                          constants). Parameterised for the SAME reason evaluateIr
//                          is: the production thresholds are on-device-ratification
//                          items (a real deconvolved ESS does not clear the synthetic
//                          20 dB cutoff even when clean — see the report), so the
//                          ratification campaign can drive tuned values without code
//                          changes, and the unit tests can pin the clean/suspect
//                          boundary deterministically.
//
// PURE + OFFLINE: two FFT passes inside (referenceMatches, then deconvolve+evaluateIr
// on a match). Heavy enough that the engine must run it OFF the audio thread.
[[nodiscard]] inline MeasurementGrade gradeMeasurement (const float* reference,
                                                        const float* response,
                                                        int n, double sampleRate,
                                                        double f1 = 20.0, double f2 = 20000.0,
                                                        float minIrSnrDb = kMinIrSnrDb,
                                                        float maxThdPct  = kMaxThdPct) {
    MeasurementGrade g;
    if (reference == nullptr || response == nullptr || n <= 0) {
        g.state = RefMonState::ReferenceStale;   // no data to trust -> treat as a mismatch, never green
        return g;
    }

    // 1) MATCH-GATE FIRST. A wrong/stale reference fails here and we stop — no quality verdict.
    g.match = referenceMatches (reference, response, n);
    if (! g.match.matched) {
        g.state = RefMonState::ReferenceStale;   // "doesn't match - re-learn"; quality stays zero
        return g;
    }

    // 2) MATCHED -> recover the IR and grade its quality (guidance only).
    const std::vector<float> ir = deconvolve (reference, response, n);
    const std::vector<int>   harm = farinaHarmonicOffsets (n, sampleRate, f1, f2);
    g.quality = evaluateIr (ir.data(), (int) ir.size(), /*matched*/ true,
                            minIrSnrDb, maxThdPct, harm, sampleRate);
    g.state = g.quality.lowQuality ? RefMonState::GradedSuspect : RefMonState::GradedClean;
    return g;
}

// ---- Window grading (pure; the engine's worker calls this) ---------------------------------------
// The #7 deterministic-detection counterpart to gradeMeasurement(). The engine now buffers the mic
// response CONTINUOUSLY into a rolling ring (NOT gated on the level arm), so the snapshot the worker
// receives is a LONG window (~kGradingSeconds) whose FRONT is leading silence/room noise and whose
// sweep sits somewhere INSIDE it. This helper locates the sweep inside that window via the matched-
// filter cross-correlation (crossCorrelateAlign), extracts the aligned reference-length segment, and
// grades it with gradeMeasurement (which re-runs the match-gate FIRST, so a non-sweep window — a
// finger snap, music, the inter-sweep gap — fails the gate and returns ReferenceStale, NOT a grade).
//
//   reference / refLen     : the learned ESS reference.
//   window / windowLen      : the rolling-ring snapshot (>= refLen; leading silence + the sweep inside).
// On windowLen < refLen we fall back to grading the window as-is (short capture); otherwise we align.
// PURE + OFFLINE (a cross-correlation FFT pass + gradeMeasurement's two passes). Run OFF the audio thread.
// Clamp a raw cross-correlation lag to the start index of the reference-length segment inside the window.
// A POSITIVE lag means the sweep starts `lag` samples into the window (the leading silence); negatives and
// over-runs are clamped so the extracted segment stays in range. SHARED by gradeMeasurementWindow and the
// poller's decide() so the match-gate and the grade key off the SAME offset (the honesty contract).
[[nodiscard]] inline int clampSweepStart (int rawLag, int refLen, int windowLen) noexcept {
    int start = rawLag;
    if (start < 0)                  start = 0;
    if (start > windowLen - refLen) start = windowLen - refLen;   // keep refLen samples in range
    if (start < 0)                  start = 0;
    return start;
}

// Grade a window at a PRECOMPUTED sweep offset (the start returned by clampSweepStart). Lets a caller that
// already located the sweep (the poller's decide()) reuse that offset instead of cross-correlating a second
// time — so the gate and the grade agree on where the sweep is. `start` must already be clamped in range.
[[nodiscard]] inline MeasurementGrade gradeMeasurementWindowAt (const float* reference, int refLen,
                                                               const float* window, int windowLen, int start,
                                                               double sampleRate,
                                                               double f1 = 20.0, double f2 = 20000.0,
                                                               float minIrSnrDb = kMinIrSnrDb,
                                                               float maxThdPct  = kMaxThdPct) {
    MeasurementGrade g;
    if (reference == nullptr || window == nullptr || refLen <= 0 || windowLen <= 0) {
        g.state = RefMonState::ReferenceStale;   // no data to trust -> never green
        return g;
    }
    if (windowLen < refLen)
        return gradeMeasurement (reference, window, windowLen, sampleRate, f1, f2, minIrSnrDb, maxThdPct);
    if (start < 0)                  start = 0;
    if (start > windowLen - refLen) start = windowLen - refLen;
    if (start < 0)                  start = 0;
    const int n = (windowLen - start < refLen) ? (windowLen - start) : refLen;
    return gradeMeasurement (reference, window + start, n, sampleRate, f1, f2, minIrSnrDb, maxThdPct);
}

[[nodiscard]] inline MeasurementGrade gradeMeasurementWindow (const float* reference, int refLen,
                                                             const float* window, int windowLen,
                                                             double sampleRate,
                                                             double f1 = 20.0, double f2 = 20000.0,
                                                             float minIrSnrDb = kMinIrSnrDb,
                                                             float maxThdPct  = kMaxThdPct) {
    MeasurementGrade g;
    if (reference == nullptr || window == nullptr || refLen <= 0 || windowLen <= 0) {
        g.state = RefMonState::ReferenceStale;   // no data to trust -> never green
        return g;
    }
    // Short window: nothing to search through -> grade what we have (equal-length segment).
    if (windowLen < refLen)
        return gradeMeasurement (reference, window, windowLen, sampleRate, f1, f2, minIrSnrDb, maxThdPct);

    // Locate the sweep inside the long window: crossCorrelateAlign(ref, window) returns the lag of the
    // window relative to the reference, which clampSweepStart turns into the in-range segment start.
    const AlignResult a = crossCorrelateAlign (reference, refLen, window, windowLen);
    const int start = clampSweepStart (a.delaySamples, refLen, windowLen);
    return gradeMeasurementWindowAt (reference, refLen, window, windowLen, start,
                                     sampleRate, f1, f2, minIrSnrDb, maxThdPct);
}

} // namespace eb
