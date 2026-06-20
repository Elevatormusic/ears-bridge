#include <catch2/catch_test_macros.hpp>
#include "audio/ClockBridge.h"
#include "audio/HealthMonitor.h"
#include <vector>
#include <cmath>
#include <functional>

// Balanced driver: push capture to track consumption so the FIFO hovers near its primed fill
// (no overrun, no starvation), then collect the rendered output. capSampleFn(i) supplies capture
// sample i; the renderer reads renderBlocks blocks of renderBlock frames. Returns the rendered
// stream plus the bridge's under/overrun tallies.
static std::vector<float> runTransport (eb::ClockBridge& cb, double capRate, double renRate,
                                        int capBlock, int renderBlock, int renderBlocks,
                                        const std::function<float(long long)>& capSampleFn,
                                        int& underruns, int& overruns) {
    const double capPerRender = capRate / renRate;

    long long capPushed = 0;
    std::vector<float> capBuf ((size_t) capBlock);
    auto pushOne = [&]() {
        for (int i = 0; i < capBlock; ++i) capBuf[(size_t) i] = capSampleFn (capPushed + i);
        cb.pushCapture (capBuf.data(), capBlock);
        capPushed += capBlock;
    };
    // Prime the FIFO to the PI setpoint with REAL capture (NOT a primeToTarget() silence prime): the
    // balanced-feed target math below counts total real content pushed, so the prime must be real
    // content too. A silence prime here would both offset the impulse train downstream AND, because
    // primeToTarget() already seeds kTargetFill*capacity, double-prime the ring straight into overrun.
    // kTargetFill*8192 == 4096 frames; that fits the free space of every capacity this harness is
    // called with (>= 8192), so the FIFO starts at ~kTargetFill and the balanced loop holds it there.
    const long long primed = (long long) std::llround (eb::ClockBridge::kTargetFill * 8192.0);
    for (long long pushed = 0; pushed < primed; ) { pushOne(); pushed += capBlock; }

    std::vector<float> out ((size_t) renderBlock, 0.0f), rendered;
    rendered.reserve ((size_t) renderBlocks * renderBlock);
    long long renderDone = 0;
    for (int b = 0; b < renderBlocks; ++b) {
        const long long target = (long long) ((double) (renderDone + renderBlock) * capPerRender) + primed;
        while (capPushed < target) pushOne();
        cb.pullRender (out.data(), renderBlock);
        renderDone += renderBlock;
        for (int i = 0; i < renderBlock; ++i) rendered.push_back (out[(size_t) i]);
    }
    underruns = cb.underruns(); overruns = cb.overruns();
    return rendered;
}

TEST_CASE("ClockBridge transport: equal-rate impulse train arrives in order with no drop/dup") {
    // Marker impulses every `period` capture samples. At 1:1 the rendered stream must contain the
    // SAME number of impulses in monotonically increasing positions (no reorder), spaced ~period
    // apart (no drop = no gap of ~2*period; no dup = no two impulses < period/2 apart).
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    const int period = 480;   // 100 impulses/sec at 48k
    auto capSample = [period] (long long i) { return (i % period == 0) ? 1.0f : 0.0f; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 48000.0, 48000.0, 256, 256, 400, capSample, u, o);
    REQUIRE (u == 0); REQUIRE (o == 0);

    // Collect impulse positions (Lagrange smears one capture impulse to a small local cluster; take
    // the local-max index as the marker, with a refractory gap so the smear is not double-counted).
    std::vector<int> pos;
    const int n = (int) r.size();
    for (int i = 1; i < n - 1; ++i)
        if (r[(size_t) i] > 0.5f && r[(size_t) i] >= r[(size_t) i-1] && r[(size_t) i] > r[(size_t) i+1])
            if (pos.empty() || (i - pos.back()) > period / 2) pos.push_back (i);

    REQUIRE (pos.size() >= 4);
    for (size_t k = 1; k < pos.size(); ++k) {
        const int gap = pos[k] - pos[k-1];
        CHECK (gap > period * 3 / 4);       // no dropped impulse (would ~double the gap)
        CHECK (gap < period * 5 / 4);       // no duplicated impulse (would ~halve the gap)
    }
}

TEST_CASE("ClockBridge transport: 96k->48k conserves the impulse count (2:1 decimation)") {
    // Every `period` capture samples carries an impulse; at 96k->48k the rendered (48k) stream is
    // half as long, so it must carry ~half as many impulses (count conservation across the ratio).
    eb::ClockBridge cb; cb.prepare (96000.0, 48000.0, 1, 16384);
    const int capPeriod = 960;   // 100 impulses/sec at 96k
    auto capSample = [capPeriod] (long long i) { return (i % capPeriod == 0) ? 1.0f : 0.0f; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 96000.0, 48000.0, 256, 256, 400, capSample, u, o);
    REQUIRE (u == 0); REQUIRE (o == 0);

    // Detect impulses by ABS local-max + refractory gap at a LOW threshold. A `> 0.5f` single-sample
    // threshold is wrong here: at the 2:1 ratio each decimated impulse lands at a fractional grid
    // phase, so the LagrangeInterpolator splits its energy across two output samples and the peak
    // sample oscillates from ~1.0 down to ~0.2 (e.g. one real impulse appears as +0.31 followed by
    // -0.15). A `> 0.5f` detector silently drops those low-phase impulses (it found only 124), which
    // looks like a transport loss but is purely a detector artifact -- the impulses ARE all present
    // (last non-zero output sits at the very end, back-half peak ~1.0). A low-threshold abs local-max
    // with a refractory gap (~the output period) tracks the actual impulse spacing and is stable: it
    // returns 206 invariant to the threshold (0.05..0.10) and gap (240..400), so the count is real,
    // not a thresholding accident.
    const int outPeriod = capPeriod * 48000 / 96000;   // 480: capture period mapped through the 2:1 ratio
    int impulses = 0; int last = -100000; const int n = (int) r.size();
    for (int i = 1; i < n - 1; ++i)
        if (std::abs (r[(size_t) i]) > 0.05f
            && std::abs (r[(size_t) i]) >= std::abs (r[(size_t) i-1])
            && std::abs (r[(size_t) i]) >  std::abs (r[(size_t) i+1])
            && (i - last) > outPeriod / 2) { ++impulses; last = i; }

    // 400 render blocks * 256 = 102400 render samples / (48000/100) = ~213 impulses if every impulse
    // from t=0 rendered; the warm-up edge (FIFO prime + the few blocks the SRC ratio takes to settle)
    // costs a handful, so the conserved count lands ~206. A dropped or duplicated impulse stream would
    // move this by tens (a 1:1 vs 2:1 ratio confusion would ~double it), so a +/-12 band still proves
    // "the 2:1 ratio conserves the impulse count" while tolerating the warm-up edge.
    const int rendered = 400 * 256;
    const int expected = rendered / (48000 / 100);     // 213
    INFO ("impulses=" << impulses << " expected~=" << expected);
    CHECK (impulses > expected / 2 + expected / 4);     // > ~159: NOT halved/dropped (rules out a real loss)
    CHECK (std::abs (impulses - expected) <= 12);       // count conserved within the warm-up edge
}

// Transparency metric reused from the transport runs: frequency/phase-invariant resonator-residual
// (a clean unit-amplitude tone obeys r[i+1]+r[i-1] == 2cos(w)*r[i]); returns max(|peak-1|, residual/peak).
static double toneCleanliness (const std::vector<float>& r, int warm) {
    const int n = (int) r.size();
    double peak = 0.0;
    for (int i = warm; i < n; ++i) peak = std::max (peak, std::abs ((double) r[(size_t) i]));
    double num = 0.0, den = 0.0;
    for (int i = warm + 1; i < n - 1; ++i) {
        const double ri = (double) r[(size_t) i];
        num += ((double) r[(size_t) i+1] + (double) r[(size_t) i-1]) * ri;
        den += ri * ri;
    }
    const double k = (den > 1.0e-12) ? num / den : 0.0;
    double resid = 0.0;
    for (int i = warm + 1; i < n - 1; ++i)
        resid = std::max (resid, std::abs ((double) r[(size_t) i+1] + (double) r[(size_t) i-1] - k * (double) r[(size_t) i]));
    return std::max (std::abs (peak - 1.0), resid / std::max (peak, 1.0e-6));
}

TEST_CASE("ClockBridge transport: a small injected drift stays within the ASRC drift tolerance") {
    // Bridge nominal = 1:1, but the producer is FED at 48000/48010 (~0.02% slow). The PI loop must
    // trim to hold the FIFO; over a long run the published ratio stays within kDriftRatioTol and the
    // FIFO never runs away (no under/overrun) -- a real sub-tolerance drift the loop absorbs cleanly.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 8192);
    const double f0 = 997.0, fed = 48010.0;
    double ph = 0.0; const double d = 2.0 * juce::MathConstants<double>::pi * f0 / fed;
    auto capSample = [&] (long long) { const float s = (float) std::sin (ph); ph += d; return s; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 48010.0, 48000.0, 256, 256, 800, capSample, u, o);
    INFO ("ratio=" << cb.currentRatio() << " fill=" << cb.fifoFill() << " u=" << u << " o=" << o);
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (cb.fifoFill() > 0.05);
    CHECK (cb.fifoFill() < 0.95);
    CHECK (std::abs (cb.currentRatio() - 1.0) < eb::HealthMonitor::kDriftRatioTol);
}

TEST_CASE("ClockBridge transport: long soak -- sub-tolerance creep does not corrupt the stream") {
    // 4000 render blocks of 256 = ~21 s at 48k. With a ~0.02% feed offset the PI loop continuously
    // micro-trims the ratio; the audit's failure mode is that this continuous retiming smears the
    // stream while staying under the drift latch. Assert the rendered tone stays transparent
    // (steady amplitude, no clicks) over the whole soak -- i.e. creep does NOT silently corrupt it.
    eb::ClockBridge cb; cb.prepare (48000.0, 48000.0, 1, 16384);
    const double f0 = 997.0, fed = 48008.0;
    double ph = 0.0; const double d = 2.0 * juce::MathConstants<double>::pi * f0 / fed;
    auto capSample = [&] (long long) { const float s = (float) std::sin (ph); ph += d; return s; };

    int u = 0, o = 0;
    auto r = runTransport (cb, 48008.0, 48000.0, 256, 256, 4000, capSample, u, o);
    const double clean = toneCleanliness (r, juce::jmin ((int) r.size() / 2, 24 * 256));
    INFO ("soak cleanliness=" << clean << " u=" << u << " o=" << o << " fill=" << cb.fifoFill());
    CHECK (u == 0);
    CHECK (o == 0);
    CHECK (clean < 0.05);     // transparent across the full soak: no creep-induced retiming corruption
}
