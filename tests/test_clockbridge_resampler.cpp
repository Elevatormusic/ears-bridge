#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "audio/ClockBridge.h"
#include <vector>
#include <cmath>

using Catch::Approx;

// Integration: ClockBridge driving the windowed-sinc PolyphaseResampler via a phase accumulator. These
// verify the bookkeeping (needIn / advance / history retention) keeps the FIFO stable and bounded — the
// resampler's signal quality itself is proven in test_resampler.cpp.

TEST_CASE ("ClockBridge: accumulator conservation at unity (no FIFO drift)", "[clockbridge][resampler]") {
    eb::ClockBridge b; b.prepare (48000.0, 48000.0, 1, 1 << 15); b.primeToTarget();
    std::vector<float> in (256, 0.25f), out (256);
    for (int blk = 0; blk < 400; ++blk) { b.pushCapture (in.data(), 256); b.pullRender (out.data(), 256); }
    // A wrong advance would drift the FIFO -> under/overruns and fill away from target.
    CHECK (b.underruns() == 0);
    CHECK (b.overruns()  == 0);
    CHECK (b.fifoFill() == Approx (0.5).margin (0.15));
}

TEST_CASE ("ClockBridge: 2x and 4x downsample fill the request, no crash/spiral", "[clockbridge][resampler]") {
    for (double cap : { 96000.0, 192000.0 }) {
        eb::ClockBridge b; b.prepare (cap, 48000.0, 1, 1 << 16); b.primeToTarget();
        const int inBlk = (int) (cap / 48000.0) * 512;          // feed enough capture per render block
        std::vector<float> in ((size_t) inBlk, 0.1f), out (512);
        for (int blk = 0; blk < 200; ++blk) {
            b.pushCapture (in.data(), inBlk);
            const int got = b.pullRender (out.data(), 512);
            REQUIRE (got == 512);                               // always fills the request
        }
        CHECK (b.fifoFill() == Approx (0.5).margin (0.25));
    }
}

TEST_CASE ("ClockBridge: a tone resampled at unity stays bounded + non-garbage", "[clockbridge][resampler]") {
    eb::ClockBridge b; b.prepare (48000.0, 48000.0, 1, 1 << 15); b.primeToTarget();
    const int F = 256; std::vector<float> in (F), out (F);
    double peak = 0.0;
    for (int blk = 0; blk < 200; ++blk) {
        for (int i = 0; i < F; ++i) in[(size_t) i] = (float) std::sin (2.0 * 3.14159265 * 1000.0 * (blk * F + i) / 48000.0);
        b.pushCapture (in.data(), F);
        b.pullRender (out.data(), F);
        if (blk > 20) for (int i = 0; i < F; ++i) peak = std::max (peak, std::abs ((double) out[(size_t) i]));
    }
    CHECK (peak > 0.9);     // the tone passes through at full amplitude
    CHECK (peak < 1.1);     // and is not amplified/garbage
}

TEST_CASE ("ClockBridge: a block larger than kMaxRenderBlock is chunked (no OOB)", "[clockbridge][resampler]") {
    eb::ClockBridge b; b.prepare (96000.0, 48000.0, 1, 1 << 18); b.primeToTarget();   // 2x downsample
    const int F = 12000;                                        // > kMaxRenderBlock (8192)
    std::vector<float> in (48000, 0.2f), out ((size_t) F);      // DC; plenty of capture for the big block
    for (int blk = 0; blk < 6; ++blk) {
        b.pushCapture (in.data(), (int) in.size());
        REQUIRE (b.pullRender (out.data(), F) == F);            // fills the whole oversized block via chunks
    }
    double mx = 0.0;
    for (float s : out) mx = std::max (mx, std::abs ((double) s));
    CHECK (mx < 0.5);                                           // DC passed through (~0.2), bounded, no garbage
}

TEST_CASE ("ClockBridge: currentGroupDelay reports the resampler latency", "[clockbridge][resampler]") {
    eb::ClockBridge b; b.prepare (48000.0, 48000.0, 1, 1 << 14);
    CHECK (b.currentGroupDelay() == Approx (47.5));   // (L-1)/2 at unity
}

namespace {
// Peak |y[i+1] - 2y[i] + y[i-1]| over [lo,hi): a click/step discontinuity spikes the 2nd difference far
// above a smooth sine's (~amp*omega^2); a slope-only change (the freeze edge) does not.
double secondDiffPeak (const std::vector<float>& y, int lo, int hi) {
    double m = 0.0;
    for (int i = std::max (1, lo); i < std::min ((int) y.size() - 1, hi); ++i)
        m = std::max (m, std::abs ((double) y[(size_t) i + 1] - 2.0 * (double) y[(size_t) i] + (double) y[(size_t) i - 1]));
    return m;
}
} // namespace

TEST_CASE ("ClockBridge: click-free across the D6 freeze edge", "[clockbridge][resampler][continuity]") {
    // Small capacity so the prime (0.5*capacity = 4096 silence) drains and the tone is actually flowing
    // at the freeze edge (output block 40 = sample 10240, well past the 4096-sample prime latency).
    eb::ClockBridge b; b.prepare (48000.0, 48000.0, 1, 1 << 13); b.primeToTarget();
    const int F = 256; std::vector<float> in (F), out (F), cap;
    for (int blk = 0; blk < 80; ++blk) {
        for (int i = 0; i < F; ++i)
            in[(size_t) i] = 0.5f * (float) std::sin (2.0 * 3.14159265358979 * 1000.0 * (blk * F + i) / 48000.0);
        b.pushCapture (in.data(), F);
        if (blk == 40) b.setSweepActive (true);                // engage the D6 freeze MID-STREAM (the edge)
        b.pullRender (out.data(), F);
        cap.insert (cap.end(), out.begin(), out.end());
    }
    const double base = secondDiffPeak (cap, 20 * F, 35 * F);          // steady-state tone, past the prime
    const double edge = secondDiffPeak (cap, 40 * F - 8, 40 * F + 8);  // tight window around the freeze sample
    INFO ("steady-state 2nd-diff=" << base << "  freeze-edge 2nd-diff=" << edge);
    // readPhase_ persists across the swap (only inc's slope changes), so there is no step -> edge ~ baseline.
    CHECK (edge <= base * 2.0 + 1e-3);
}
