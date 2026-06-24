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

TEST_CASE ("ClockBridge: 4x downsample at the ratioTrim ceiling + small capacity stays in-bounds", "[clockbridge][resampler][oob]") {
    // Review regression (2026-06-23): the FIR scratch was sized for kMaxRatio(4.0), but the PI raises the
    // effective ratio to 4.0*kMaxRatioTrim(1.03)=4.12. A small-capacity caller (prepare() permits down to 1024)
    // then had the resampler read ~970 floats PAST the scratch on a full kMaxRenderBlock block. Overfeeding pins
    // ratioTrim to its ceiling; a full 8192-output block then exercises the worst-case read span. With the fix
    // (scratch sized for kMaxRatio*kMaxRatioTrim) every read stays in bounds -> finite, bounded output.
    eb::ClockBridge b; b.prepare (192000.0, 48000.0, 1, 4096); b.primeToTarget();   // small capacity, 4x ratio
    std::vector<float> in (8192, 0.1f), out (8192);
    for (int blk = 0; blk < 300; ++blk) {                          // overfeed -> fill pins high -> ratioTrim -> 1.03 (inc 4.12)
        b.pushCapture (in.data(), 8192);
        b.pullRender  (out.data(), 1024);
    }
    for (int blk = 0; blk < 8; ++blk) {
        b.pushCapture (in.data(), 8192);
        REQUIRE (b.pullRender (out.data(), 8192) == 8192);          // a FULL block at the high inc: the worst-case read span
        for (float s : out) { REQUIRE (std::isfinite (s)); REQUIRE (std::abs ((double) s) < 2.0); }   // an OOB heap read -> garbage/NaN
    }
}

TEST_CASE ("ClockBridge: sustained render-starvation does not run readPhase away (no OOB)", "[clockbridge][resampler][oob]") {
    // Review regression: on a fully-starved block `advance` clamps to 0, so readPhase_ is not retired while the
    // output loop keeps advancing it - sustained starvation (render pulling while capture is dead) ran readPhase_
    // past the scratch end. The fix resets readPhase_ to the prime point when it would overrun. 500 starved pulls
    // must stay finite (silence) with no OOB read.
    eb::ClockBridge b; b.prepare (192000.0, 48000.0, 1, 4096); b.primeToTarget();
    std::vector<float> out (1024);
    for (int blk = 0; blk < 500; ++blk) {                          // capture is DEAD: never pushCapture
        REQUIRE (b.pullRender (out.data(), 1024) == 1024);
        for (float s : out) REQUIRE (std::isfinite (s));
    }
}
