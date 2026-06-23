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
