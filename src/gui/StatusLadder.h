#pragma once
#include "audio/RefMonitor.h"   // RefMonState / refMonBlocksGreen / QualityVerdict / classify* / qualityNote
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
    // #44: a green per-ear line is only ever the GradedClean verdict.
    jassert (out.tone != StatusTone::Ok || state == RefMonState::GradedClean);
    return out;
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
