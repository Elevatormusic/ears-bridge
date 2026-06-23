#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>   // SNR review fix: Catch::Approx for the per-ear sweep-peak checks
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include "audio/CalibrationGeneration.h"
#include "cal/FirDesigner.h"
#include <vector>
#include <cmath>
#include <limits>
using Catch::Matchers::WithinAbs;

TEST_CASE("firTapsForRate scales 8192@48k to power-of-two per rate") {
    CHECK (eb::firTapsForRate (48000.0)  == 8192);
    CHECK (eb::firTapsForRate (96000.0)  == 16384);
    CHECK (eb::firTapsForRate (192000.0) == 32768);
    CHECK (eb::firTapsForRate (44100.0)  == 8192);    // 7526 -> next pow2
    CHECK (eb::firTapsForRate (88200.0)  == 16384);
}

static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("AudioEngine headless seam: identity FIR + Average combine averages the ears") {
    eb::AudioEngine eng;
    const int N = 64;
    eng.prepareForTest (48000.0, N);
    eng.setLeftCalFir  (unitImpulse (8));
    eng.setRightCalFir (unitImpulse (8));
    eng.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f), out (N, 0.0f);
    // Spin a few times via the seam to settle the async IR load + gain ramp.
    bool settled = false;
    for (int rep = 0; rep < 3000 && ! settled; ++rep) {
        eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), N);
        if (std::abs (out[N-1] - 0.4f) < 1e-3f) settled = true;
        else juce::Thread::sleep (1);
    }
    REQUIRE (settled);
    CHECK_THAT (out[N-1], WithinAbs (0.4f, 1e-3));   // (0.5+0.3)/2
}

TEST_CASE("AudioEngine seam: a clipped raw input invalidates the measurement") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> inL { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };  // 3-sample rail run
    std::vector<float> inR (inL.size(), 0.0f);
    std::vector<float> mono (inL.size(), 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), (int) inL.size());
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine seam: a clean input stays valid") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> inL (8, 0.5f), inR (8, 0.3f), mono (8, 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), 8);
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine seam: session starts Idle and arms after a rise above the floor") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::Idle);
    std::vector<float> loud (8, 0.5f), sil (8, 0.0f), mono (8, 0.0f);
    // A quiet floor (warm-up settles low), then a sustained loud RISE: arms under the rise-over-floor rule.
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks + 2; ++i)
        e.processCaptureBlockForTest (sil.data(), sil.data(), mono.data(), 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::Preflight);
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i)
        e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    CHECK (e.cleanCapture());            // a clean loud run stays valid
}

TEST_CASE("AudioEngine seam: a clip ON the sweep-onset block invalidates") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    // A quiet floor (warm-up), then clean-loud ramp blocks, then a clip block that COMPLETES the arm:
    // the onset block's clip is analyzed AFTER the latch re-scope, so it stands.
    std::vector<float> loud (8, 0.5f), sil (8, 0.0f), mono (8, 0.0f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks + 2; ++i)
        e.processCaptureBlockForTest (sil.data(), sil.data(), mono.data(), 8);
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks - 1; ++i)   // qualifying blocks 1..N-1
        e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
    e.processCaptureBlockForTest (clip.data(), sil.data(), mono.data(), (int) clip.size());  // arms + clips
    CHECK (e.sessionPhase() == eb::SessionPhase::Invalid);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine: real R_HPN cal designed at 96k cuts the 4 kHz resonance") {
    auto f = juce::File (EB_TEST_DATA_DIR).getChildFile ("R_HPN_0000000.txt");
    REQUIRE (f.existsAsFile());
    auto cal = eb::CalFile::parse (f.loadFileAsString());

    eb::AudioEngine eng;
    const int N = 128;
    eng.prepareForTest (96000.0, N);
    eng.setSampleRate (96000.0);            // active rate drives tap count = 16384
    eng.loadRightCal (cal);                 // designs FIR at 96k internally
    eng.setCombineMode (eb::CombineMode::RightOnly);

    // Drive a 4 kHz sine and confirm it is strongly attenuated vs a 200 Hz reference.
    auto rms = [&](double freq) {
        std::vector<float> inL (N, 0.0f), inR (N, 0.0f), out (N, 0.0f);
        double ph = 0.0, d = 2.0 * juce::MathConstants<double>::pi * freq / 96000.0;
        double acc = 0.0; int blocks = 200;
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < N; ++i) { inR[i] = (float) std::sin (ph); ph += d; }
            eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), N);
            if (b > 40) for (int i = 0; i < N; ++i) acc += (double) out[i] * out[i];
            else juce::Thread::sleep (1);   // let IR settle during warm-up blocks
        }
        return std::sqrt (acc / (double) ((blocks - 41) * N));
    };
    double r200 = rms (200.0), r4k = rms (4000.0);
    INFO ("rms200=" << r200 << " rms4k=" << r4k);
    CHECK (r4k < 0.4 * r200);   // ~4 kHz resonance inverted -> strong cut
}

TEST_CASE("AudioEngine seam: a NaN input is sanitized and does NOT poison the FIR for later blocks") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    std::vector<float> bad (8, 0.3f); bad[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> r0  (8, 0.0f), mono (8, 0.0f);
    e.processCaptureBlockForTest (bad.data(), r0.data(), mono.data(), 8);
    for (float v : mono) CHECK (std::isfinite (v));               // output never non-finite
    CHECK (eb::any (e.health().flags & eb::HealthFlag::NonFinite));
    CHECK_FALSE (e.cleanCapture());

    // A subsequent CLEAN block must produce finite output (proves the convolution wasn't poisoned).
    std::vector<float> clean (8, 0.4f), r1 (8, 0.0f), mono2 (8, 0.0f);
    e.processCaptureBlockForTest (clean.data(), r1.data(), mono2.data(), 8);
    for (float v : mono2) CHECK (std::isfinite (v));
}

TEST_CASE("AudioEngine::rawRail defaults to unverified before any run") {
    eb::AudioEngine e;
    auto rr = e.rawRail();
    CHECK_FALSE (rr.verified);
    CHECK (rr.requestedRate == 0.0);
    CHECK (rr.mixRate == 0.0);
}

TEST_CASE("AudioEngine seam: a clip DURING the sweep invalidates and latches the session Invalid") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    auto run = [&] (std::vector<float> l) {
        std::vector<float> r (l.size(), 0.0f), mono (l.size(), 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), (int) l.size());
    };
    std::vector<float> loud (8, 0.5f);
    std::vector<float> quiet (8, 0.0f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks + 2; ++i) run (quiet);  // quiet floor + warm-up
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (loud);  // arm (rise above floor)
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    CHECK (e.cleanCapture());

    run (clip);                                                                      // mid-sweep clip
    CHECK_FALSE (e.cleanCapture());
    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.health().session == eb::SessionPhase::Invalid);
}

TEST_CASE("AudioEngine seam: a clean sweep then sustained silence reaches Complete and stays clean") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    auto run = [&] (float v) {
        std::vector<float> l (8, v), r (8, 0.0f), mono (8, 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), 8);
    };
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks + 2; ++i) run (0.0f);  // quiet floor + warm-up
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (0.5f);   // arm (rise above floor)
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    // prepareForTest configured the silence window for block=8 @ 48k (a large block count); drive enough
    // quiet blocks to cross it. Use the session's configured need via kSilenceCompleteSeconds.
    const int need = (int) std::lround (eb::MeasurementSession::kSilenceCompleteSeconds * 48000.0 / 8.0) + 2;
    for (int i = 0; i < need; ++i) run (0.0f);
    CHECK (e.sessionPhase() == eb::SessionPhase::Complete);
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine seam: the RIGHT-earcup sweep after a gap is still scored (no false-clean)") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    auto runL = [&] (std::vector<float> l) {
        std::vector<float> r (l.size(), 0.0f), mono (l.size(), 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), (int) l.size());
    };
    std::vector<float> loud (8, 0.5f), quiet (8, 0.0f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks + 2; ++i) runL (quiet);  // quiet floor + warm-up
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) runL (loud);  // LEFT sweep (rise above floor)
    const int need = (int) std::lround (eb::MeasurementSession::kSilenceCompleteSeconds * 48000.0 / 8.0) + 2;
    for (int i = 0; i < need; ++i) runL (quiet);                                      // long inter-sweep gap
    CHECK (e.sessionPhase() == eb::SessionPhase::Complete);                           // provisional
    CHECK (e.cleanCapture());

    // RIGHT earcup sweep arms again; a clip on it must STILL invalidate (the bug D5 must not have).
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks - 1; ++i) runL (loud);
    runL (clip);
    CHECK_FALSE (e.cleanCapture());
    CHECK (e.health().session == eb::SessionPhase::Invalid);
}

// SNR review fix (Finding 1/2/3): AutoPerEar runs TWO sweeps (left earcup, then a re-armed right earcup).
// The per-ear in-sweep peaks must be scoped to ONE sweep, reset at each SweepActive->Complete edge, so the
// SECOND sweep's verdict does NOT read max() across BOTH sweeps' peaks (the stale left peak leaking in),
// and the published verdict dB must reflect the sweep that actually completed.
TEST_CASE("AudioEngine seam: per-ear sweep peaks reset per Complete -- the LEFT sweep's peak does not leak into the RIGHT") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);

    // Drive one block with explicit L/R channel values (no symmetry assumption).
    auto run = [&] (float lVal, float rVal) {
        std::vector<float> l (8, lVal), r (8, rVal), mono (8, 0.0f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), 8);
    };
    const int gap = (int) std::lround (eb::MeasurementSession::kSilenceCompleteSeconds * 48000.0 / 8.0) + 2;

    // ---- Sweep 1 (LEFT earcup): a strong LEFT peak, the RIGHT mic quiet. ----
    for (int i = 0; i < eb::MeasurementSession::kArmWarmupBlocks + 2; ++i) run (0.0f, 0.0f);  // quiet floor + warm-up
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i)      run (0.30f, 0.0f);  // LEFT sweep
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    CHECK (e.maxSweepPeakLForTest() == Catch::Approx (0.30f).margin (0.002f));   // left numerator latched
    CHECK (e.maxSweepPeakRForTest() == Catch::Approx (0.0f).margin (0.002f));    // right ear was quiet
    for (int i = 0; i < gap; ++i) run (0.0f, 0.0f);                               // long inter-sweep gap -> Complete 1
    CHECK (e.sessionPhase() == eb::SessionPhase::Complete);

    // The Complete edge must have ZEROED the per-ear peaks (resetSweepPeaks), so the stale 0.30 left peak
    // is GONE before sweep 2 accumulates. This is the core no-leak guarantee.
    CHECK (e.maxSweepPeakLForTest() == Catch::Approx (0.0f).margin (0.002f));
    CHECK (e.maxSweepPeakRForTest() == Catch::Approx (0.0f).margin (0.002f));
    // Snapshot the verdict dB of sweep 1: min-ear is the QUIET right mic (peak 0.0 over the floor), so a
    // deeply NEGATIVE SNR. This is the published value the GUI would have shown for sweep 1.
    const float shownSweep1 = e.completedSweepSnrDb();
    CHECK (std::isfinite (shownSweep1));
    CHECK (shownSweep1 < 0.0f);   // sweep 1's min ear (silent right) is below its own floor

    // ---- Sweep 2 (RIGHT earcup): a strong RIGHT peak; the LEFT mic now only faintly crosstalks (0.05,
    // well below sweep 1's stale 0.30). If the peaks did NOT reset, maxSweepPeakL would still read 0.30. ----
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (0.05f, 0.40f);  // RIGHT sweep
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    // Sweep 2's per-ear peaks are the SECOND sweep's values -- the stale left 0.30 did NOT leak.
    CHECK (e.maxSweepPeakLForTest() == Catch::Approx (0.05f).margin (0.002f));   // NOT 0.30 (no leak)
    CHECK (e.maxSweepPeakRForTest() == Catch::Approx (0.40f).margin (0.002f));   // this sweep's right peak

    for (int i = 0; i < gap; ++i) run (0.0f, 0.0f);                               // gap -> Complete 2
    CHECK (e.sessionPhase() == eb::SessionPhase::Complete);

    // Sweep 2's published snapshot dB reflects SWEEP 2 (min-ear = the faint LEFT 0.05, a finite POSITIVE
    // SNR over sweep 2's quiet floor), NOT sweep 1's deeply-negative value. The snapshot being a different,
    // higher number than sweep 1's proves the GUI reads the per-Complete snapshot, not a stale recompute.
    const float shownSweep2 = e.completedSweepSnrDb();
    CHECK (std::isfinite (shownSweep2));
    CHECK (shownSweep2 > 0.0f);              // L=0.05 sits well above sweep 2's sub-0.01 noise floor
    CHECK (shownSweep2 > shownSweep1);       // sweep 2's verdict is distinct from (and not stuck on) sweep 1's
}

TEST_CASE("AudioEngine: the production FreezeGate freezes the ClockBridge ratio end-to-end, the gap releases it") {
    // Exercises the REAL freeze trigger wired into the capture path (eb::freezeGateStep -> bridge.setSweepActive),
    // not the dead session level-arm. processCaptureBlockForTest mirrors the live capture callback exactly.
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);
    std::vector<float> loud (512, 0.3f), mono (512, 0.0f), quiet (512, 0.0f);  // 0.3 ~ -10.5 dBFS, above the -24 dBFS gate
    CHECK_FALSE (e.bridgeSweepFrozen());
    // A few consecutive loud blocks (>= kFreezeAttackBlocks) arm the gate -> the bridge ratio freezes.
    for (int b = 0; b < eb::kFreezeAttackBlocks + 2; ++b)
        e.processCaptureBlockForTest (loud.data(), loud.data(), mono.data(), 512);
    CHECK (e.bridgeSweepFrozen());
    // The release HOLD (kFreezeReleaseSecs) bridges Dirac's quiet L<->R inter-sweep gap, so a SHORT silence must
    // NOT release it -> one frozen ratio spans BOTH earcup sweeps (consistent interaural group delay).
    const int holdBlocks = (int) (eb::kFreezeReleaseSecs * 48000.0 / 512.0);
    for (int b = 0; b < holdBlocks / 2; ++b) e.processCaptureBlockForTest (quiet.data(), quiet.data(), mono.data(), 512);
    CHECK (e.bridgeSweepFrozen());                                   // still frozen mid-gap
    // Quiet sustained PAST the hold releases it, so the PI loop re-centers before the next measurement.
    for (int b = 0; b < holdBlocks + 4; ++b) e.processCaptureBlockForTest (quiet.data(), quiet.data(), mono.data(), 512);
    CHECK_FALSE (e.bridgeSweepFrozen());
}

TEST_CASE("AudioEngine seam: format change after prepare raises FormatChanged and invalidates") {
    // The headless seam (prepareForTest) calls notifyPreparedFormat internally with 48k/32-bit/2ch.
    // We then call checkFormatChange (via simulateFormatChangeForTest) to simulate a mid-run OS
    // renegotiation. This tests that the wiring between HealthMonitor and AudioEngine::health() is live.
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    // A clean block: no format change.
    std::vector<float> inL (8, 0.1f), inR (8, 0.1f), mono (8, 0.0f);
    e.processCaptureBlockForTest (inL.data(), inR.data(), mono.data(), 8);
    CHECK (e.cleanCapture());
    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::FormatChanged));
    // Simulate a mid-run device format renegotiation (96k vs the prepared 48k).
    e.simulateFormatChangeForTest (96000.0, 32, 2);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::FormatChanged));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine: calibrationApplied requires a valid generation with matching requested/built/applied ids") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);
    CHECK_FALSE (e.calibrationApplied());                 // nothing applied yet

    eb::CalibrationGeneration g;
    g.id = 7; g.sampleRate = 48000.0; g.taps = 8; g.valid = true;
    g.leftFir = juce::AudioBuffer<float> (1, 8);  g.leftFir.clear();  g.leftFir.setSample (0,0,1.0f);
    g.rightFir = juce::AudioBuffer<float> (1, 8); g.rightFir.clear(); g.rightFir.setSample (0,0,1.0f);

    e.setRequestedGeneration (7);
    e.applyCalibrationGeneration (g);
    CHECK (e.requestedGeneration() == 7);
    CHECK (e.appliedGeneration() == 7);
    CHECK (e.calibrationApplied());                       // requested==built==applied, valid

    // A newer request that hasn't been applied yet -> not ready (stale-protection: the gate closes).
    e.setRequestedGeneration (8);
    CHECK_FALSE (e.calibrationApplied());

    // An INVALID generation never reads as applied even if ids match.
    eb::CalibrationGeneration bad; bad.id = 9; bad.valid = false; bad.diagnostic = "Unknown calibration type";
    e.setRequestedGeneration (9);
    e.applyCalibrationGeneration (bad);
    CHECK_FALSE (e.calibrationApplied());
    CHECK (e.calibrationDiagnostic().containsIgnoreCase ("Unknown"));
}

TEST_CASE("AudioEngine: reconfigAllowed() reflects the live-capture (Running) state") {
    eb::AudioEngine e;
    // Fresh engine is Stopped -> reconfiguration is allowed (the normal pre-start apply path).
    CHECK (e.status() == eb::EngineStatus::Stopped);
    CHECK (e.reconfigAllowed());

    // Drive the engine to Running via the callback test seam (no real device).
    e.prepareCallbacksForTest (48000.0, 8, 1024);
    CHECK (e.status() == eb::EngineStatus::Running);
    CHECK_FALSE (e.reconfigAllowed());
}

TEST_CASE("AudioEngine: applyCalibrationGeneration is a no-op while Running (engine backstop)") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);

    // Apply a valid generation while Stopped -> the normal path WORKS and the gate opens.
    eb::CalibrationGeneration g1;
    g1.id = 11; g1.sampleRate = 48000.0; g1.taps = 8; g1.valid = true;
    g1.leftFir  = unitImpulse (8);
    g1.rightFir = unitImpulse (8);
    e.setRequestedGeneration (11);
    e.applyCalibrationGeneration (g1);
    CHECK (e.appliedGeneration() == 11);
    CHECK (e.builtGeneration()   == 11);
    CHECK (e.calibrationApplied());

    // Go live. While Running the engine must refuse to swap FIRs mid-capture.
    e.prepareCallbacksForTest (48000.0, 8, 1024);
    CHECK (e.status() == eb::EngineStatus::Running);

    eb::CalibrationGeneration g2;
    g2.id = 22; g2.sampleRate = 48000.0; g2.taps = 8; g2.valid = true;
    g2.leftFir  = unitImpulse (8);
    g2.rightFir = unitImpulse (8);
    e.setRequestedGeneration (22);
    e.applyCalibrationGeneration (g2);   // BLOCKED: status() == Running

    // No state was mutated by the blocked apply: built/applied still point at gen 11, not 22.
    CHECK (e.builtGeneration()   == 11);
    CHECK (e.appliedGeneration() == 11);
    CHECK (e.calibrationDiagnostic().isEmpty());   // appliedGen_ (gen 11, valid) was not overwritten
}

// ---- Noise-floor primitive (Task 4): the engine feeds the tracker + exposes the measured floor ----
TEST_CASE("AudioEngine: measured noise floor populates from quiet capture blocks") {
    eb::AudioEngine eng;
    eng.prepareForTest (48000.0, 480);
    std::vector<float> q (480, 0.004f), mono (480, 0.0f);
    CHECK_FALSE (eng.noiseFloorValid());
    for (int i = 0; i < 60; ++i)                       // 60 * 480/48000 = 0.6 s of quiet -> baselines
        eng.processCaptureBlockForTest (q.data(), q.data(), mono.data(), 480);
    CHECK (eng.noiseFloorValid());
    CHECK (eng.noiseFloorDbAveraged() < -24.0f);       // a real, quiet measured floor (~-48 dBFS)
}

TEST_CASE("AudioEngine: hardware-Dirac toggle publishes GradingOffHardware; auto-detect via injected signals", "[engine][hwdirac]") {
    eb::AudioEngine e;
    // The toggle (deterministic): ON -> calm GradingOffHardware on BOTH ears.
    e.setDiracHardwareProcessor (true);
    CHECK (e.diracHardwareProcessorActive());
    CHECK ((eb::RefMonState) e.refMonState (0) == eb::RefMonState::GradingOffHardware);
    CHECK ((eb::RefMonState) e.refMonState (1) == eb::RefMonState::GradingOffHardware);
    // PERSISTENCE (the verifier's bug): a base-state publish (learn / start / sweep-complete edge) while the
    // toggle is on must NOT revert the calm state. setReferenceLoaded publishes Learned/NotLearned via the same
    // publishBaseRefState the capture paths use -> it must stay GradingOffHardware.
    e.setReferenceLoaded (true);    // would publish Learned -> gated to GradingOffHardware
    CHECK ((eb::RefMonState) e.refMonState (0) == eb::RefMonState::GradingOffHardware);
    e.setReferenceLoaded (false);   // would publish NotLearned -> still GradingOffHardware
    CHECK ((eb::RefMonState) e.refMonState (0) == eb::RefMonState::GradingOffHardware);
    // OFF -> back to a non-graded state (the next sweep re-grades).
    e.setDiracHardwareProcessor (false);
    CHECK_FALSE (e.diracHardwareProcessorActive());
    CHECK ((eb::RefMonState) e.refMonState (0) != eb::RefMonState::GradingOffHardware);

    // Auto-detect ("the tool": inject the signals a real run would feed from OutputActivity).
    e.updateHardwareDiracAutoDetect (/*micHeard*/ true, /*maxOutputPeak*/ 0.0f, /*readable*/ true, /*validMode*/ true);
    CHECK (e.autoDetectedHardwareDirac());                                   // the hardware-Dirac signature
    e.updateHardwareDiracAutoDetect (true, 0.2f, true, true);
    CHECK_FALSE (e.autoDetectedHardwareDirac());                            // software Dirac: the output DID render
    e.updateHardwareDiracAutoDetect (true, -1.0f, false, true);
    CHECK_FALSE (e.autoDetectedHardwareDirac());                            // unreadable -> never auto-detect
    e.updateHardwareDiracAutoDetect (false, 0.0f, true, true);
    CHECK_FALSE (e.autoDetectedHardwareDirac());                            // no mic sweep -> no
}
