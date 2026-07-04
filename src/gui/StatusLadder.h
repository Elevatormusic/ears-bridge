#pragma once
#include "audio/RefMonitor.h"   // RefMonState / refMonBlocksGreen / QualityVerdict / classify* / qualityNote
#include "audio/ResponseShape.h"   // SP3: eb::ShapeFlag (the shape-anomaly bitmask the INFO note reads)
#include "gui/SnrStatus.h"      // kMinSweepSnrDb (the per-ear "(low SNR)" tag threshold)
#include <juce_core/juce_core.h>

namespace eb {

// ==================================================================================================
// #50: the RUNNING status ladder extracted PURE (snapshot in -> text/tone/tooltip out), per the repo's
// StartGate/SnrStatus pattern. This is the presentation half of the measurement-honesty contract — the
// audit found nearly every contract violation lived in updateStatusLine's precedence (#1 #4 #21 #44
// #15 #49), and none of it was headlessly testable while it was welded to juce::Label commits. The GUI
// (MainComponent::updateStatusLine) now only BUILDS the snapshot from engine getters, calls
// runningStatus(), maps StatusTone -> Theme colours, and commits set-if-changed.
//
// Honesty fixes implemented HERE (each pinned by tests/test_statusladder.cpp):
//   #1  the captured headline consults the WORST graded state — green ONLY when every graded ear is
//       GradedClean and none clipped; Marginal/Stale -> warn, Suspect -> danger. (The old branch
//       committed ok-green "safe to run the next sweep" for ALL of them.)
//   #4  the captured branch renders BOTH ears (compact "L: ... / R: ..." second line + full detail in
//       the tooltip) — the L ear's actionable verdict (re-learn / flagged / quantified clip cut) was
//       previously computed nowhere once any grade existed.
//   #21 the silent-input warning sits ABOVE the activity/captured branches — it was dead code below
//       them, shadowed in exactly the connected-but-silent state it was built for.
//   #44 refMonBlocksGreen() is finally enforced: greenCaptureViolation() is asserted in-ladder and
//       exhaustively tested, so a refactor can never reintroduce a green line over a blocking state.
// ==================================================================================================

// Colour ROLE, mapped to Theme by the GUI (keeps this header pure/headless).
//   Live = "use the live-readout's own colour verbatim" (the meter logic computed it).
enum class StatusTone { Ok, Dim, Warn, Danger, Live };

// One ear's published grade snapshot (engine getters, ear 0 = L / 1 = R).
struct EarGradeSnapshot {
    int   state      = 0;        // (int) RefMonState
    float irSnrDb    = 0.0f;
    float thdPercent = 0.0f;
    float sweepSnrDb = 0.0f;     // > 0 only when this ear graded with a VALID sweep SNR
    float peakDb     = -120.0f;  // RAW input sweep peak (dBFS; positive = overshot full scale)
    // SP3 INFO note (worst-offender shape anomaly for this ear, from shapeInfoNote()). Empty when there
    // is no anomaly. INFO-ONLY: it decorates the line as a neutral clause + rides the tooltip; it NEVER
    // changes the line's tone (never Warn/Danger) or the headline. Precomputed GUI-side from the engine.
    juce::String shapeNote;
    // SP3 INFO tooltip (the FULL numbers behind the note, one line per active finding, from shapeInfoTip()).
    // Empty when there is no anomaly. Rides the line's tooltip so the elided one-liner stays discoverable.
    juce::String shapeTip;
};

struct StatusLineOut {
    juce::String text, tip;
    StatusTone   tone = StatusTone::Dim;
};

struct StatusOut { StatusLineOut line1, line2; };

// Everything the Running ladder decides from. The GUI precomputes the stateful/debounced inputs
// (tick counters, live readout, chain verdict) — the ladder itself is a pure precedence function.
struct RunningSnapshot {
    bool         cleanCapture = true;
    juce::String invalidMessage;           // eb::invalidMeasurementMessage(flags), used when !cleanCapture
    bool         liveOwns = false;         // sweep-active live readout takes the line
    juce::String liveText;
    bool         outputClip   = false;     // HealthFlag::ClipOutput latched
    bool         silentHold   = false;     // debounced: input below the silence floor ~2 s (#21)
    bool         lowSnrHold   = false;     // debounced LowSnr guidance
    float        snrL = 0.0f, snrR = 0.0f, snrCombined = 0.0f;   // per-ear stores + combined fallback (#15)
    bool         rateVeto = false;         // chain checked and NOT all-48k
    juce::String rateSummary;
    bool         referenceLoaded = false;
    EarGradeSnapshot earL, earR;
    bool         phaseIdleOrPreflight = false;
    bool         gradeSignalPresent   = false;   // cosmetic Listening <-> Sweep-in-progress
    bool         phaseComplete        = false;
    bool         osResampled          = false;
    bool         autoDetectedHardwareDirac = false;
    bool         hardwareDiracSetting     = false;
    bool         lowLevelHold = false;     // debounced never-reached-healthy-level guidance
    juce::String advisoryTail;             // chainAdvisoryTail(): " - <advisory>" or empty
};

// The graded-set test the captured branch keys on (mirrors the GUI's isGraded: any quality verdict OR
// the failed match-gate; NOT the waiting states).
[[nodiscard]] inline bool earIsGraded (int state) noexcept {
    const auto s = (RefMonState) state;
    return s == RefMonState::GradedClean    || s == RefMonState::GradedMarginal
        || s == RefMonState::GradedSuspect  || s == RefMonState::ReferenceStale;
}

// ---- per-ear FULL line (the pure form of the old renderEarStatusLine; display only) --------------
[[nodiscard]] inline StatusLineOut earStatusLine (const char* prefix, const EarGradeSnapshot& e) {
    const auto state     = (RefMonState) e.state;
    const juce::String p = juce::String (prefix) + ": ";
    // This ear's sweep-to-room-noise SNR is published (>0) only when it graded with a valid SNR; flag
    // low only then — a 0 (not graded / SNR not assessable) must NOT read as "low SNR".
    const bool lowSnr = e.sweepSnrDb > 0.0f && e.sweepSnrDb < kMinSweepSnrDb;
    const juce::String snrTail = lowSnr ? juce::String (" (low SNR)") : juce::String();

    StatusLineOut out;
    if (state == RefMonState::GradingOffHardware) {
        // Hardware Dirac: no loopback reference to grade against. CALM/neutral - NOT a warning, NOT green.
        out.text = p + "grading off - hardware Dirac; per-ear calibration still active";
        out.tone = StatusTone::Dim;
    } else if (state == RefMonState::GradedClean) {
        out.text = p + "verified - IR-SNR " + juce::String (juce::roundToInt (e.irSnrDb)) + " dB, THD "
                 + juce::String (juce::roundToInt (e.thdPercent)) + "% (calibration pending)";
        out.tone = StatusTone::Ok;
    } else if (state == RefMonState::GradedMarginal || state == RefMonState::GradedSuspect) {
        // The sweepSNR gate (ACTIVE): Marginal = orange (usable), Suspect = red (noisy/distorted).
        // Reconstruct THIS ear's bands from its published metrics so the note matches the verdict.
        QualityVerdict v;
        v.state        = state;
        v.sweepSnrBand = classifySweepSnr (e.sweepSnrDb, e.sweepSnrDb != 0.0f);
        v.irSnrBand    = classifyIrSnr (e.irSnrDb);
        v.thdBand      = classifyThd (e.thdPercent);
        const juce::String note = qualityNote (v);
        out.text = p + (note.isNotEmpty() ? note : juce::String ("re-measure"));
        out.tone = (state == RefMonState::GradedMarginal) ? StatusTone::Warn : StatusTone::Danger;
        out.tip  = "Graded against the reference: sweep-SNR "
                 + (e.sweepSnrDb != 0.0f ? juce::String (e.sweepSnrDb, 0) + " dB" : juce::String ("n/a"))
                 + ", IR-SNR " + juce::String (juce::roundToInt (e.irSnrDb))
                 + " dB, distortion " + juce::String (juce::roundToInt (e.thdPercent)) + "%.";
    } else if (state == RefMonState::ReferenceStale) {
        out.text = p + "re-learn the reference";
        out.tone = StatusTone::Warn;
        out.tip  = "This earcup's sweep didn't match the learned reference channel (the gate failed). Re-learn "
                   "the reference in Dirac's Windows Audio mode (Advanced -> Learn reference), then measure again.";
    } else {
        // Learned / NotGraded / NotLearned: this ear hasn't produced a gradeable sweep yet this run.
        out.text = p + "waiting for the sweep";
        out.tone = StatusTone::Dim;
    }

    // GAIN-STAGING READOUT (this ear's RAW input sweep peak in dBFS; positive = overshoot). Only
    // meaningful once this ear graded (the engine resets it to the -120 floor otherwise).
    const bool graded = state == RefMonState::GradedClean || state == RefMonState::GradedMarginal
                     || state == RefMonState::GradedSuspect;
    juce::String peakTail;
    if (graded && e.peakDb > -119.0f) {
        if (e.peakDb >= 0.0f) {
            // Clipped: quantify the overshoot; cut = ceil(peak) + 3 dB margin. This WARN OVERRIDES the
            // IR-SNR/THD INFO on this ear's line — the clip is the actionable thing.
            const int cutDb = juce::roundToInt (std::ceil (e.peakDb)) + 3;
            out.text = p + "clipped +" + juce::String (e.peakDb, 1) + " dBFS - lower the output ~"
                     + juce::String (cutDb) + " dB";
            out.tone = StatusTone::Warn;
            out.tip  = "This earcup's sweep peaked at +" + juce::String (e.peakDb, 1) + " dBFS - it overshot "
                       "full scale and clipped. Lower Dirac's output (or the system level) by about "
                     + juce::String (cutDb) + " dB, then re-measure. The correction is one shared output "
                       "level; the hotter earcup sets the cut.";
        } else if (e.peakDb >= -1.0f) {
            out.text = p + "peaked " + juce::String (e.peakDb, 1)
                     + " dBFS - right at clipping, ease the output down";
            out.tone = StatusTone::Warn;
            out.tip  = "This earcup's sweep peaked at " + juce::String (e.peakDb, 1) + " dBFS - right at full "
                       "scale with no headroom. Ease the output down a few dB so the next sweep can't clip.";
        } else if (e.peakDb >= -12.0f) {
            peakTail = " (peak " + juce::String (e.peakDb, 0) + " dBFS)";          // healthy: neutral note
        } else if (e.peakDb < -18.0f) {
            peakTail = " (peak " + juce::String (e.peakDb, 0) + " dBFS - low)";    // low: the level-low owns the warn
        }
    }
    out.text += snrTail + peakTail;
    // SP3 INFO note: append the worst-offender shape anomaly as a neutral clause (never changes the tone —
    // it is INFO-only). Only for graded ears (the note is meaningless on a waiting/stale line, and the
    // engine clears it otherwise). The FULL numbers (shapeTip) always ride the tooltip.
    //
    // #68 LENGTH BUDGET: the per-ear status line shares the title bar and can already carry "verified …"
    // + SNR/peak tails; a 68-char drift note can push the composed line past what fits at the app's
    // minimum width and get clipped by the label (the #8 cut-off lesson). If the note would overflow, ELIDE
    // it with ".." so the line stays within budget — the tooltip carries the full note + numbers, so nothing
    // is lost, only relocated (mirrors the running line's advisory-tail budget).
    if (graded && e.shapeNote.isNotEmpty()) {
        static constexpr int kEarLineCharBudget = 78;   // same title-bar budget the #68 render gate ratifies
        const juce::String sep = "  -  ";
        if (out.text.length() + sep.length() + e.shapeNote.length() <= kEarLineCharBudget) {
            out.text += sep + e.shapeNote;
        } else {
            const int room = kEarLineCharBudget - out.text.length() - sep.length() - 2;   // -2 for the ".."
            const juce::String shown = room > 0 ? e.shapeNote.substring (0, room).trimEnd() + ".."
                                                : juce::String ("..");
            out.text += sep + shown;
        }
        // The tooltip carries the full numbers (shapeTip); fall back to the note itself if none were composed.
        const juce::String full = e.shapeTip.isNotEmpty() ? e.shapeTip : e.shapeNote;
        out.tip   = out.tip.isNotEmpty() ? out.tip + "\n" + full : full;
    }
    // #44: a green per-ear line is only ever the GradedClean verdict.
    jassert (out.tone != StatusTone::Ok || state == RefMonState::GradedClean);
    return out;
}

// PROVISIONAL — on-device ratification gate for the SHAPE-note copy (mirrors IrQualityStatus.h's
// kIrThresholdsRatified block). FALSE until the #54C threshold campaign ratifies the shape cutoffs on
// real hardware. Two of the shape findings fire STRUCTURALLY on healthy headphone hardware at the
// provisional thresholds, so their accusatory one-liners would mislead a good-hardware user:
//   - kStep: D7's ratio is the headphone's spectral envelope vs the flat reference; EVERY real headphone
//     exceeds the 2 dB step tolerance (the synthetic coupler measures ~11.4 dB — see SIM F4). "level
//     changed mid-sweep - disable enhancements / AGC" is wrong for that user.
//   - kTruncLo: fired on the rig's CLEAN run at the provisional LF pivot ("no low-frequency content -
//     check the seal…" would accuse a healthy seal).
// The synthetic end-to-end rig's CLEAN run published flags 0x30a (kStep|kComb|kTruncLo|kBaselineSet)
// with NO impairment — that is the ground truth this gate responds to. While FALSE, shapeInfoNote SKIPS
// the kStep and kTruncLo branches: the flags STILL publish and the measured numbers STILL ride the
// tooltip (shapeInfoTip), so nothing is hidden — only the accusatory worst-offender one-liner waits for
// ratified thresholds. Precedence is otherwise unchanged. Flip to true once #54C ratifies the cutoffs.
static constexpr bool kShapeCopyRatified = false;

// ---- SP3 shape-anomaly INFO note (Task 5) --------------------------------------------------------
// The single worst-offender INFO line for one ear's published shape anomalies. INFO-ONLY — this NEVER
// changes a grade, a tone, or cleanCapture; the GUI renders it in the neutral info style (never Warn/
// Danger). Worst-offender precedence (spec §5/§6): no-band > truncation > comb > polarity > drift > hum
// > resonance > skew > step. Copy strings follow the spec §4 intent (drift line shortened to fit the
// length rule). Returns "" when there is no anomaly (flags == 0 or only kBaselineSet — the baseline being
// learned is not a finding). Each line stays <= ~70 chars (the #8 title-bar cut-off lesson); the caller
// carries the FULL numbers in the shapeInfoTip tooltip.
[[nodiscard]] inline juce::String shapeInfoNote (unsigned flags, float driftMaxDb, float combDelayMs,
                                                float effHiHz, float effLoHz, int humBaseHz) {
    using namespace eb;
    juce::ignoreUnused (effLoHz);   // the LF-truncation copy names no frequency (spec §4.D3: position-only)
    // No measurable band FIRST (the loudest finding: the reference had a band but the measurement has none,
    // so the chain is dropping ALL content — a bare truncation edge would understate it). spec §6.
    if (flags & ShapeFlag::kNoBand)
        return "no measurable response band - check the chain";
    // Truncation NEXT (a truncated band is the most consequential — the chain is dropping content).
    if (flags & ShapeFlag::kTruncHi)
        return "content ends near " + juce::String (effHiHz / 1000.0f, 1)
             + " kHz - the chain may be resampling";
    // kTruncLo copy gated (see kShapeCopyRatified): fires clean at the provisional LF pivot. Flag+number
    // stay live; the accusatory "check the seal" one-liner waits for #54C.
    if (kShapeCopyRatified && (flags & ShapeFlag::kTruncLo))
        return "no low-frequency content - check the seal or the chain";
    if (flags & ShapeFlag::kComb)
        return "possible duplicate path - echo ~" + juce::String (combDelayMs, 1) + " ms";
    if (flags & ShapeFlag::kPolarity)
        return "the two ears measure with opposite polarity - check wiring";
    if (flags & ShapeFlag::kDrift)
        return "response drifted since sweep 1 (" + juce::String (driftMaxDb, 1)
             + " dB) - re-seat or check the chain";
    if (flags & ShapeFlag::kHum)
        return "mains hum (" + juce::String (humBaseHz) + " Hz) in the noise floor - check ground/amp loop";
    if (flags & ShapeFlag::kResonance)
        return "possible narrow resonance in the response";
    if (flags & ShapeFlag::kSkew)
        return "clock skew suspected - check the device sample rates";
    // kStep copy gated (see kShapeCopyRatified): D7's ratio is confounded by the headphone envelope, so
    // every real headphone trips it. Flag+number stay live; the one-liner waits for #54C.
    if (kShapeCopyRatified && (flags & ShapeFlag::kStep))
        return "level changed mid-sweep - disable enhancements / AGC";
    return {};   // no anomaly (or only the baseline-set bit): no note
}

// ---- SP3 shape-anomaly numbers tooltip (Task 5) --------------------------------------------------
// The FULL numbers behind the one-line note, one "label: value" line per active finding (NOT the single
// worst-offender — the tooltip carries EVERYTHING the line elides). INFO-ONLY, same as shapeInfoNote.
// Takes exactly the scalars the engine publishes (publishShapeAnomalies + shapeFlags); each present flag
// contributes its measured number(s). Empty when there is no finding (flags == 0 or only kBaselineSet),
// so the caller can skip it. Drift also reports hfShelf (the tinny-EQ shelf signature) when non-zero.
[[nodiscard]] inline juce::String shapeInfoTip (unsigned flags, float driftMaxDb, float hfShelfDb,
                                                float combDepthDb, float combDelayMs, float effLoHz,
                                                float effHiHz, float lobeWidth, float stepDb,
                                                int humBaseHz, float resonanceHz) {
    using namespace eb;
    if ((flags & ~ShapeFlag::kBaselineSet) == 0u) return {};   // no finding (bare baseline is not one)

    juce::StringArray lines;
    if (flags & ShapeFlag::kNoBand)
        lines.add ("No measurable band: the reference had a band; the measurement has none");
    if (flags & ShapeFlag::kTruncHi)
        lines.add ("HF truncation: content ends near " + juce::String (effHiHz / 1000.0f, 2) + " kHz");
    if (flags & ShapeFlag::kTruncLo)
        lines.add ("LF truncation: content starts near " + juce::String (effLoHz, 0) + " Hz");
    if (flags & ShapeFlag::kComb) {
        juce::String l = "Comb / echo: depth " + juce::String (combDepthDb, 1) + " dB";
        if (combDelayMs != 0.0f) l += ", delay " + juce::String (combDelayMs, 2) + " ms";
        lines.add (l);
    }
    if (flags & ShapeFlag::kPolarity)
        lines.add ("Cross-ear polarity: the two ears measure inverted relative to each other");
    if ((flags & ShapeFlag::kDrift) || ((flags & ShapeFlag::kBaselineSet) && driftMaxDb != 0.0f)) {
        juce::String l = "Drift: max delta " + juce::String (driftMaxDb, 1) + " dB vs sweep 1";
        if (hfShelfDb != 0.0f) l += ", HF shelf " + juce::String (hfShelfDb, 1) + " dB";
        lines.add (l);
    }
    if (flags & ShapeFlag::kHum)
        lines.add ("Mains hum: " + juce::String (humBaseHz) + " Hz base in the pre-sweep noise");
    if (flags & ShapeFlag::kResonance)
        lines.add ("Resonance: narrow spike near " + juce::String (resonanceHz, 0) + " Hz");
    if (flags & ShapeFlag::kSkew)
        lines.add ("Clock skew: main lobe " + juce::String (lobeWidth, 1) + " samples wide");
    if (flags & ShapeFlag::kStep)
        lines.add ("Level step: " + juce::String (stepDb, 1) + " dB mid-sweep");
    return lines.joinIntoString ("\n");
}

// ---- per-ear COMPACT summary (the captured branch's shared second line, #4) ----------------------
// One short clause per ear; the FULL detail (numbers + guidance) rides in the line's tooltip.
[[nodiscard]] inline juce::String earCompactSummary (const char* prefix, const EarGradeSnapshot& e) {
    const auto state     = (RefMonState) e.state;
    const juce::String p = juce::String (prefix) + ": ";
    const bool graded = state == RefMonState::GradedClean || state == RefMonState::GradedMarginal
                     || state == RefMonState::GradedSuspect;
    // The clip cut is the single most actionable per-ear datum — it overrides the state word.
    if (graded && e.peakDb >= 0.0f)
        return p + "clipped +" + juce::String (e.peakDb, 1) + " - lower ~"
             + juce::String (juce::roundToInt (std::ceil (e.peakDb)) + 3) + " dB";
    if (graded && e.peakDb >= -1.0f && e.peakDb > -119.0f)
        return p + "at clipping - ease the output down";
    const bool lowSnr = e.sweepSnrDb > 0.0f && e.sweepSnrDb < kMinSweepSnrDb;
    const juce::String snrTag = lowSnr ? juce::String (" (low SNR)") : juce::String();
    switch (state) {
        case RefMonState::GradedClean:
            return p + "verified " + juce::String (juce::roundToInt (e.irSnrDb)) + " dB" + snrTag;
        case RefMonState::GradedMarginal:    return p + "marginal" + snrTag;
        case RefMonState::GradedSuspect:     return p + "flagged" + snrTag;
        case RefMonState::ReferenceStale:    return p + "re-learn";
        case RefMonState::GradingOffHardware: return p + "grading off";
        default:                              return p + "waiting";
    }
}

// ---- the captured headline (#1): worst GRADED state decides text + tone --------------------------
// Severity: Clean < Marginal < Stale < Suspect. An UNGRADED other ear does not block green — after the
// first earcup grades clean, "safe to run the next sweep" is exactly the intended go-ahead for the
// second sweep. A graded ear that CLIPPED (peak >= 0 dBFS) escalates at least to Warn: a clipped
// capture must never read green either.
[[nodiscard]] inline StatusLineOut capturedHeadline (const EarGradeSnapshot& earL, const EarGradeSnapshot& earR) {
    auto severity = [] (const EarGradeSnapshot& e) -> int {
        if (! earIsGraded (e.state)) return -1;                 // not graded: doesn't vote
        switch ((RefMonState) e.state) {
            case RefMonState::GradedClean:    return 0;
            case RefMonState::GradedMarginal: return 1;
            case RefMonState::ReferenceStale: return 2;
            default:                          return 3;         // GradedSuspect
        }
    };
    const int worst = juce::jmax (severity (earL), severity (earR));
    const bool anyClipped = (earIsGraded (earL.state) && earL.peakDb >= 0.0f)
                         || (earIsGraded (earR.state) && earR.peakDb >= 0.0f);
    StatusLineOut out;
    if (worst >= 3) {
        out.text = "Sweep captured but flagged - noisy or distorted; re-measure";
        out.tone = StatusTone::Danger;
    } else if (worst == 2) {
        out.text = "Sweep didn't match the reference - re-learn or re-measure";
        out.tone = StatusTone::Warn;
    } else if (worst == 1) {
        out.text = "Sweep captured - marginal quality; usable, consider re-measuring";
        out.tone = StatusTone::Warn;
    } else if (anyClipped) {
        out.text = "Sweep captured - input clipped; lower the output (see the ear line)";
        out.tone = StatusTone::Warn;
    } else {
        out.text = "Sweep captured - safe to run the next sweep";
        out.tone = StatusTone::Ok;
    }
    return out;
}

// #44: the enforced form of refMonBlocksGreen's contract. True = VIOLATION: a green captured line is
// showing while some GRADED ear is in a state the user must not read as clean (or clipped full-scale).
[[nodiscard]] inline bool greenCaptureViolation (StatusTone headlineTone,
                                                 const EarGradeSnapshot& earL, const EarGradeSnapshot& earR) {
    if (headlineTone != StatusTone::Ok) return false;
    auto blocks = [] (const EarGradeSnapshot& e) {
        return (earIsGraded (e.state) && refMonBlocksGreen ((RefMonState) e.state))
            || (earIsGraded (e.state) && e.peakDb >= 0.0f);
    };
    return blocks (earL) || blocks (earR);
}

// ---- the RUNNING precedence ladder ---------------------------------------------------------------
[[nodiscard]] inline StatusOut runningStatus (const RunningSnapshot& s) {
    StatusOut out;   // line2 defaults blank/Warn-free (Dim)

    // 1. An invalidating condition is reported the instant it latches, regardless of phase.
    if (! s.cleanCapture) {
        out.line1 = { s.invalidMessage, {}, StatusTone::Danger };
    }
    // 2. LIVE in-sweep readout: the user needs the level / a clip AS IT HAPPENS.
    else if (! s.rateVeto && s.liveOwns) {
        out.line1 = { s.liveText, {}, StatusTone::Live };
    }
    // 3. Output hit full scale: the clamp saved the cable, but the sweep is distorted.
    else if (s.outputClip) {
        out.line1 = { "Output clipping - lower the level or avoid Sum", {}, StatusTone::Warn };
    }
    // 4. #21: SILENT INPUT, hoisted above the captured/activity branches it was shadowed by. The
    //    -50 dBFS floor sits far below room ambient (~-30 dB), so a live mic never trips this — a held
    //    silence means the signal path is dead NOW, which must beat a stale success confirmation.
    else if (s.silentHold) {
        out.line1 = { "Running - no input signal (check the EARS)", {}, StatusTone::Warn };
    }
    // 5. SNR guidance, before captured/waiting so a noisy sweep never reads "clean"/"safe to run the
    //    next". #15: name the OFFENDING EAR from the per-ear stores (combined snapshot = fallback).
    else if (s.lowSnrHold) {
        float worst = s.snrCombined;
        juce::String earTag;
        if (s.snrL > 0.0f && (s.snrR <= 0.0f || s.snrL <= s.snrR)) { worst = s.snrL; earTag = " (L)"; }
        else if (s.snrR > 0.0f)                                    { worst = s.snrR; earTag = " (R)"; }
        const int snrDb = juce::roundToInt (worst);
        out.line1.text = "Low SNR" + earTag + ": sweep only " + juce::String (snrDb)
                       + " dB over the room noise - re-measure";
        out.line1.tone = StatusTone::Warn;
        out.line1.tip  = "The Dirac sweep was only " + juce::String (snrDb)
                       + " dB above the room-noise floor" + (earTag.isEmpty() ? juce::String()
                       : " on the " + juce::String (earTag.contains ("L") ? "LEFT" : "RIGHT") + " ear")
                       + ". Quieten the room (close windows, stop fans/AC) or raise the level, "
                         "then re-measure for a cleaner correction.";
    }
    // 6. 48k-everywhere VETO: a wrong-rate/unreadable chain can never read "verified"/"safe".
    else if (s.rateVeto) {
        out.line1 = { s.rateSummary, {}, StatusTone::Warn };
    }
    // 7. CAPTURED (#1 #4 #44): a grade exists this run -> a PERSISTENT confirmation that never lies.
    else if (s.referenceLoaded && (earIsGraded (s.earL.state) || earIsGraded (s.earR.state))) {
        out.line1 = capturedHeadline (s.earL, s.earR);
        jassert (! greenCaptureViolation (out.line1.tone, s.earL, s.earR));   // #44: the promised assert
        // #4: BOTH ears on the shared second line (compact), full per-ear detail in the tooltip.
        const auto fullL = earStatusLine ("L", s.earL);
        const auto fullR = earStatusLine ("R", s.earR);
        out.line2.text = earCompactSummary ("L", s.earL) + "  /  " + earCompactSummary ("R", s.earR);
        out.line2.tip  = fullL.text + (fullL.tip.isNotEmpty() ? "\n" + fullL.tip : juce::String())
                       + "\n" + fullR.text + (fullR.tip.isNotEmpty() ? "\n" + fullR.tip : juce::String());
        // The compact line carries the WORST ear's tone so a red ear can't hide dim.
        auto toneRank = [] (StatusTone t) { return t == StatusTone::Danger ? 3 : t == StatusTone::Warn ? 2
                                                 : t == StatusTone::Ok     ? 1 : 0; };
        out.line2.tone = toneRank (fullL.tone) >= toneRank (fullR.tone) ? fullL.tone : fullR.tone;
    }
    // 8. Pre-sweep activity (validity isn't scoped to anything yet, so never claim "clean"). The graded
    //    case was intercepted above, so referenceLoaded here means "engaged, nothing graded yet".
    else if (s.phaseIdleOrPreflight) {
        out.line1.text = s.referenceLoaded
                       ? (s.gradeSignalPresent ? juce::String ("Sweep in progress...")
                                               : juce::String ("Listening for the Dirac sweep..."))
                       : juce::String ("Running - waiting for the Dirac sweep...");
        out.line1.tone = StatusTone::Dim;
    }
    // 9. Reference loaded, nothing graded yet, sweep running: the full per-ear waiting/verdict lines.
    else if (s.referenceLoaded) {
        out.line1 = earStatusLine ("L", s.earL);
        out.line2 = earStatusLine ("R", s.earR);
    }
    // 10. Sweep finished clean (non-adopters / hardware Dirac): the sweep-scoped combined summary.
    else if (s.phaseComplete) {
        const auto refState = (RefMonState) s.earL.state;   // combined/legacy = ear 0's published state
        juce::String msg = "Sweep captured - no clipping or dropouts detected";
        StatusTone tone = StatusTone::Ok;
        if (refState == RefMonState::GradingOffHardware) {
            msg  = "Sweep captured - reference grading off (hardware Dirac); per-ear calibration still active";
            tone = StatusTone::Dim;
        } else if (s.autoDetectedHardwareDirac && ! s.hardwareDiracSetting) {
            msg  = "Sweep captured - looks like a hardware Dirac processor; turn it on in Advanced to silence the grade";
            tone = StatusTone::Dim;
        } else if (refState == RefMonState::GradedClean) {
            msg = "Sweep captured + verified against the reference (captured earcup) - IR-SNR "
                + juce::String (juce::roundToInt (s.earL.irSnrDb)) + " dB, THD "
                + juce::String (juce::roundToInt (s.earL.thdPercent)) + "% (calibration pending)";
        } else if (refState == RefMonState::GradedMarginal) {
            msg  = "Sweep captured - marginal SNR; usable, consider re-measuring (captured earcup)";
            tone = StatusTone::Warn;
        } else if (refState == RefMonState::GradedSuspect) {
            msg  = "Sweep captured but flagged - noisy or distorted, re-measure (captured earcup)";
            tone = StatusTone::Danger;
        }
        if (s.osResampled) msg += " (OS-resampled - approximate)";
        out.line1 = { msg, {}, tone };
        jassert (! (tone == StatusTone::Ok && refMonBlocksGreen (refState)
                    && refState != RefMonState::NotLearned && refState != RefMonState::NotGraded
                    && refState != RefMonState::Learned));   // #44: the no-reference "captured clean" stays legal
    }
    // 11. Signal present but the capture never reached a healthy level: poor SNR reads "tin-can".
    else if (s.lowLevelHold) {
        out.line1 = { "Running - level low: turn your amp up to the green band", {}, StatusTone::Warn };
    }
    // 12. In-sweep, nothing latched: clean so far.
    else {
        out.line1 = { "Capturing the Dirac sweep - clean so far", {}, StatusTone::Ok };
    }

    // Chain advisory tail (#49-composed): decorates a CALM line 1 only (ok/dim), never a warn/error.
    // #68: only while the combined text still FITS the title bar at the app's minimum window — the
    // render gate ratifies this budget at 780 px (a long multi-part advisory used to clip the line).
    // An over-budget advisory rides in the tooltip instead: still discoverable, never truncated.
    static constexpr int kLine1AdvisoryCharBudget = 78;
    if (s.advisoryTail.isNotEmpty()
        && (out.line1.tone == StatusTone::Ok || out.line1.tone == StatusTone::Dim)) {
        if (out.line1.text.length() + s.advisoryTail.length() <= kLine1AdvisoryCharBudget)
            out.line1.text += s.advisoryTail;
        else {
            const auto advisory = s.advisoryTail.startsWith (" - ") ? s.advisoryTail.substring (3)
                                                                    : s.advisoryTail.trimStart();
            out.line1.tip = out.line1.tip.isEmpty() ? advisory : out.line1.tip + "\n" + advisory;
        }
    }
    return out;
}

} // namespace eb
