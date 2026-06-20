#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("AudioEngine seam: session starts Idle and arms after a sustained loud run") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::Idle);
    std::vector<float> loud (8, 0.5f), sil (8, 0.0f), mono (8, 0.0f);
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i)
        e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
    CHECK (e.sessionPhase() == eb::SessionPhase::SweepActive);
    CHECK (e.cleanCapture());            // a clean loud run stays valid
}

TEST_CASE("AudioEngine seam: a clip ON the sweep-onset block invalidates") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 8);
    // Two clean-loud ramp blocks, then a clip block that COMPLETES the arm: the onset block's clip is
    // analyzed AFTER the latch re-scope, so it stands.
    std::vector<float> loud (8, 0.5f), sil (8, 0.0f), mono (8, 0.0f);
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };
    e.processCaptureBlockForTest (loud.data(), sil.data(), mono.data(), 8);
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
    std::vector<float> clip { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (loud);  // arm
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
    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) run (0.5f);   // arm
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

    for (int i = 0; i < eb::MeasurementSession::kArmSustainBlocks; ++i) runL (loud);  // LEFT sweep
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

TEST_CASE("AudioEngine: a sustained loud sweep freezes the ClockBridge ratio, the gap releases it") {
    eb::AudioEngine e;
    e.prepareForTest (48000.0, 512);
    std::vector<float> loud (512, 0.3f), mono (512, 0.0f), quiet (512, 0.0f);  // 0.3 > kSweepStartLinear (-24 dBFS)
    CHECK_FALSE (e.bridgeSweepFrozen());
    // kArmSustainBlocks (3) sustained loud blocks arm SweepActive -> the capture sync freezes the bridge.
    for (int b = 0; b < 4; ++b) e.processCaptureBlockForTest (loud.data(), loud.data(), mono.data(), 512);
    CHECK (e.bridgeSweepFrozen());
    CHECK (e.sweepActive());
    // Sustained silence completes the segment -> session leaves SweepActive -> the bridge releases.
    for (int b = 0; b < 200; ++b) e.processCaptureBlockForTest (quiet.data(), quiet.data(), mono.data(), 512);
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
    eb::CalibrationGeneration bad; bad.id = 9; bad.valid = false; bad.diagnostic = "HEQ blocked";
    e.setRequestedGeneration (9);
    e.applyCalibrationGeneration (bad);
    CHECK_FALSE (e.calibrationApplied());
    CHECK (e.calibrationDiagnostic().containsIgnoreCase ("HEQ"));
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
