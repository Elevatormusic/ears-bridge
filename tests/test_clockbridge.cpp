#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/ClockBridge.h"
#include "audio/HealthMonitor.h"
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

// Drive the bridge with a producer feeding a capture-time sine at f0, BALANCED against the
// consumer so the FIFO hovers near its primed fill: we generate capture on demand and push
// just enough each block to keep cumulative push ~= consumed * capPerRender + prime. That
// avoids the two ways a naive harness lies about the bridge: over-feeding (-> overruns) and
// then exhausting a fixed capture budget (-> starvation/underruns).
//
// The FEED ratio is captureRate/renderRate. The bridge's OWN nominal ratio comes from however
// the test prepared it; when the two differ (the drift case) the PI fill-control loop must trim
// to hold the FIFO, which this harness then exercises.
//
// Error metric: the LagrangeInterpolator imposes a constant (fractional) group delay, so a
// sample-by-sample compare against an un-delayed ideal sine is dominated by that delay (~0.3 at
// 997 Hz) and says nothing about transparency. Instead we least-squares-fit a single tone at f0
// (any phase) over the steady region and return max(|amplitude - 1|, peak residual): delay- and
// phase-invariant, and sensitive to amplitude error, distortion, and the discontinuities that
// any under/overrun would inject.
static double runBridge (eb::ClockBridge& cb, double captureRate, double renderRate,
                         double f0, int capBlock, int renderBlock, int renderBlocks,
                         int& underruns, int& overruns) {
    const double capPerRender = captureRate / renderRate;          // feed ratio
    double capPhase = 0.0;
    const double dCap = 2.0 * juce::MathConstants<double>::pi * f0 / captureRate;

    std::vector<float> capBuf ((size_t) capBlock);
    auto pushOneBlock = [&]() {
        for (int i = 0; i < capBlock; ++i) { capBuf[(size_t) i] = (float) std::sin (capPhase); capPhase += dCap; }
        cb.pushCapture (capBuf.data(), capBlock);
    };

    const int primeBlocks = juce::jmax (1, 4096 / capBlock);       // ~4096 samples primed
    for (int i = 0; i < primeBlocks; ++i) pushOneBlock();

    std::vector<float> out ((size_t) renderBlock, 0.0f);
    std::vector<float> rendered;
    rendered.reserve ((size_t) renderBlocks * renderBlock);

    long long capPushed  = (long long) primeBlocks * capBlock;
    long long renderDone = 0;
    for (int b = 0; b < renderBlocks; ++b) {
        const long long target = (long long) ((double) (renderDone + renderBlock) * capPerRender)
                               + (long long) primeBlocks * capBlock;
        while (capPushed < target) { pushOneBlock(); capPushed += capBlock; }

        cb.pullRender (out.data(), renderBlock);
        renderDone += renderBlock;
        for (int i = 0; i < renderBlock; ++i) rendered.push_back (out[(size_t) i]);
    }
    underruns = cb.underruns(); overruns = cb.overruns();

    // Transparency metric, frequency/phase-invariant (so it is NOT fooled by the SRC's tiny
    // fill-control retune, which slightly shifts the output frequency): over the steady region,
    // a pure tone of ANY frequency obeys the resonator recurrence r[i+1] + r[i-1] = 2cos(w)*r[i].
    // Estimate k = 2cos(w) by least squares and take the worst residual of that recurrence
    // (catches distortion / clicks / dropouts), normalised by peak, combined with the peak-
    // amplitude error (catches gain change). ~0 for a clean unit-amplitude tone.
    const int n    = (int) rendered.size();
    const int warm = juce::jmin (n / 2, 24 * renderBlock);
    double peak = 0.0;
    for (int i = warm; i < n; ++i)
        peak = std::max (peak, std::abs ((double) rendered[(size_t) i]));

    double num = 0.0, den = 0.0;
    for (int i = warm + 1; i < n - 1; ++i) {
        const double ri = (double) rendered[(size_t) i];
        num += ((double) rendered[(size_t) i + 1] + (double) rendered[(size_t) i - 1]) * ri;
        den += ri * ri;
    }
    const double k = (den > 1.0e-12) ? num / den : 0.0;          // == 2 cos(w)
    double resid = 0.0;
    for (int i = warm + 1; i < n - 1; ++i)
        resid = std::max (resid, std::abs ((double) rendered[(size_t) i + 1]
                                         + (double) rendered[(size_t) i - 1]
                                         - k * (double) rendered[(size_t) i]));
    const double cleanliness = resid / std::max (peak, 1.0e-6);
    return std::max (std::abs (peak - 1.0), cleanliness);
}

TEST_CASE("ClockBridge: equal nominal rates, no under/overflow, bounded fill") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    int u = 0, o = 0;
    double err = runBridge (cb, 48000.0, 48000.0, 997.0, 256, 256, 400, u, o);
    INFO ("err=" << err << " under=" << u << " over=" << o << " fill=" << cb.fifoFill());
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (cb.fifoFill() > 0.05);
    CHECK (cb.fifoFill() < 0.95);
    CHECK (err < 0.05);   // async SRC transparent at 997 Hz
}

TEST_CASE("ClockBridge: 96k capture -> 48k render downsample passes a sine") {
    eb::ClockBridge cb; cb.prepare (96000.0, 48000.0, 1, 16384);
    int u = 0, o = 0;
    double err = runBridge (cb, 96000.0, 48000.0, 997.0, 256, 256, 400, u, o);
    INFO ("err=" << err << " under=" << u << " over=" << o << " fill=" << cb.fifoFill());
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (cb.fifoFill() > 0.05);
    CHECK (cb.fifoFill() < 0.95);
    CHECK (err < 0.05);
}

TEST_CASE("ClockBridge: small clock drift is absorbed by the fill-control loop") {
    // Bridge prepared at nominal 1.0 but FED at 48000/48010 (~0.02% slow): the feed ratio
    // differs from the bridge's nominal, so the PI loop must trim to hold the FIFO -- a real
    // drift the loop has to absorb over a long run without the FIFO running away.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    int u = 0, o = 0;
    double err = runBridge (cb, 48000.0, 48010.0, 997.0, 256, 256, 800, u, o);
    INFO ("err=" << err << " fill=" << cb.fifoFill() << " under=" << u << " over=" << o);
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (cb.fifoFill() > 0.05);
    CHECK (cb.fifoFill() < 0.95);
}

TEST_CASE("ClockBridge: priming the FIFO prevents the startup underrun (field repro)") {
    // Field bug (eb_diag-confirmed): at startup the render callback pulls before the capture
    // callback has primed the FIFO, so the first ~149 ms of pulls find an empty FIFO and the
    // interpolator zero-pads -> underrun -> FifoStarved+Dropout latched for the whole session,
    // and the slow PI fill-loop then drags the SRC ratio off-nominal (latching ExcessDrift) while
    // it refills. eb_diag measured the startup gap at 7168 frames, which is < capacity/2. Priming
    // the FIFO to the PI controller's half-full target absorbs the gap with no underrun, and the
    // fill starting AT target keeps the ratio at nominal (no spurious drift).
    const int cap = 16384;
    std::vector<float> out (512, 0.0f);

    eb::ClockBridge empty; empty.prepare (48000.0, 48000.0, 1, cap);
    for (int i = 0; i < 14; ++i) empty.pullRender (out.data(), 512);    // 14*512 = 7168 frames, no capture yet
    CHECK (empty.underruns() > 0);                                      // reproduces the starvation

    eb::ClockBridge primed; primed.prepare (48000.0, 48000.0, 1, cap);
    primed.prime (cap / 2);                                            // <- the fix: pre-fill to the 0.5 target
    for (int i = 0; i < 14; ++i) primed.pullRender (out.data(), 512);   // same gap, still no capture
    CHECK (primed.underruns() == 0);                                   // the primed buffer covers it
    CHECK (primed.fifoFill()  > 0.0);                                  // and still has headroom left
    // Fill starting at target keeps the trimmed ratio within the drift tolerance (no ExcessDrift).
    CHECK (std::abs (primed.currentRatio() - 1.0) < eb::HealthMonitor::kDriftRatioTol);
}

TEST_CASE("ClockBridge: prime is bounded by free space and never overflows") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 4096);
    cb.prime (1 << 20);                 // ask for far more than capacity
    CHECK (cb.overruns() == 0);         // clamped to free space; no overflow event
    CHECK (cb.droppedCaptureFrames() == 0);
    CHECK (cb.fifoFill() > 0.0);
    CHECK (cb.fifoFill() <= 1.0);
}

TEST_CASE("ClockBridge: reset clears stats and fill") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 4096);
    std::vector<float> z (256, 0.0f), out (256, 0.0f);
    for (int i = 0; i < 4; ++i) cb.pushCapture (z.data(), 256);
    cb.pullRender (out.data(), 256);
    cb.reset();
    CHECK (cb.underruns() == 0);
    CHECK (cb.overruns()  == 0);
    CHECK (cb.droppedCaptureFrames() == 0);
}

TEST_CASE("ClockBridge: primeToTarget fills to the PI setpoint (prime depth == setpoint)") {
    // Guards against the two prime-depth sites silently diverging: primeToTarget must land the FIFO
    // exactly on kTargetFill, the same fraction pullRender steers toward, so the loop starts at
    // equilibrium and the SRC ratio stays at nominal (no startup ExcessDrift).
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    CHECK (std::abs (cb.fifoFill() - eb::ClockBridge::kTargetFill) < 0.02);
}

// Producer-side overrun: push far more than the FIFO can hold without draining. The bridge must
// (a) record overrun EVENTS, (b) account the actual FRAMES dropped, and (c) when those frames are
// forwarded to a HealthMonitor exactly as AudioEngine's capture callback does (diff-and-forward
// against a per-callback baseline), the monitor must report droppedFrames > 0 AND latch
// cleanCapture == false -- the spec §5.6 slip/dropped-frame trend reaching the Health snapshot.
TEST_CASE("ClockBridge overrun surfaces dropped frames into a HealthMonitor (cleanCapture latches false)") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 1024);   // small FIFO, never drained below
    eb::HealthMonitor hm; hm.prepare (eb::EarsModel::Ears, 1024);

    REQUIRE (cb.overruns() == 0);
    REQUIRE (cb.droppedCaptureFrames() == 0);
    REQUIRE (hm.cleanCapture());
    REQUIRE (hm.snapshot().droppedFrames == 0);

    std::vector<float> blk (2048, 0.5f);   // each push is 2x capacity -> guaranteed overflow
    long long baseline = 0;                 // mirrors AudioEngine::lastDroppedCapture_
    for (int i = 0; i < 8; ++i) {
        cb.pushCapture (blk.data(), (int) blk.size());
        const long long dropped = cb.droppedCaptureFrames();
        const long long delta   = dropped - baseline;     // exactly the capture-callback wiring
        baseline = dropped;
        if (delta > 0) hm.reportDroppedFrames (delta);
    }

    CHECK (cb.overruns() > 0);                     // events recorded
    CHECK (cb.droppedCaptureFrames() > 0);         // frames recorded (frame-accurate, not event count)
    CHECK (hm.snapshot().droppedFrames > 0);       // trend surfaced into the Health snapshot
    CHECK_FALSE (hm.cleanCapture());               // overflow invalidates the measurement
    CHECK (eb::any (hm.flags() & eb::HealthFlag::Dropout));  // and raises a flag (symmetric w/ underrun path)

    // The forwarded total equals the bridge's accounted frames (no double counting, no loss).
    CHECK (hm.snapshot().droppedFrames == cb.droppedCaptureFrames());
}

TEST_CASE("ClockBridge: frozen ratio is held constant during a sweep") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.25f), out (256, 0.0f);
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256);   // first frozen block snapshots
    const double frozen = cb.currentRatio();
    REQUIRE (cb.sweepActive());
    for (int i = 0; i < 200; ++i) {
        cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256);
        CHECK (cb.currentRatio() == frozen);                 // ratio frozen: zero creep
    }
    CHECK_FALSE (cb.consumeEmergencyCorrection());           // balanced feed => no forced correction
}

TEST_CASE("ClockBridge: freeze absorbs small drift with FIFO headroom (no emergency, no creep)") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.1f), out (256, 0.0f);
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    const double feed = 48024.0 / 48000.0;   // +0.05% fast producer
    double credit = 0.0; double held = 0.0;
    for (int b = 0; b < 300; ++b) {
        credit += 256.0 * feed;
        while (credit >= 256.0) { cb.pushCapture (blk.data(), 256); credit -= 256.0; }
        cb.pullRender (out.data(), 256);
        if (b == 0) held = cb.currentRatio();
        CHECK (cb.currentRatio() == held);                   // still frozen across the whole sweep
    }
    CHECK_FALSE (cb.consumeEmergencyCorrection());           // small drift absorbed by headroom
}

TEST_CASE("ClockBridge: freeze raises an emergency correction when drift outruns the headroom") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.1f), out (256, 0.0f);
    for (int i = 0; i < 4; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    bool raised = false;
    for (int i = 0; i < 40 && ! raised; ++i) {               // pull with no new capture -> FIFO drains
        cb.pullRender (out.data(), 256);
        raised = cb.consumeEmergencyCorrection();
    }
    CHECK (raised);                                          // raw near-empty crossing forced a correction
}

TEST_CASE("ClockBridge: unfreezing resumes PI steering and recenters") {
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    cb.primeToTarget();
    std::vector<float> blk (256, 0.1f), out (256, 0.0f);
    for (int i = 0; i < 8; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    cb.setSweepActive (true);
    const double frozen = cb.currentRatio();
    for (int i = 0; i < 20; ++i) { cb.pushCapture (blk.data(), 256); cb.pullRender (out.data(), 256); }
    CHECK (cb.currentRatio() == frozen);
    cb.setSweepActive (false);                              // recenter only between sweeps / in silence
    REQUIRE_FALSE (cb.sweepActive());
    bool moved = false;
    for (int i = 0; i < 50; ++i) {
        cb.pushCapture (blk.data(), 256); cb.pushCapture (blk.data(), 256);   // overfeed -> fill rises
        cb.pullRender (out.data(), 256);
        if (cb.currentRatio() != frozen) { moved = true; break; }
    }
    CHECK (moved);                                          // steering resumed
}
