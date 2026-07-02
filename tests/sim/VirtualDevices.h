#pragma once
#include "SimSignals.h"   // same-directory: clang resolves quote-includes against THIS header's dir (MSVC's includer-chain quirk masked "sim/..." on Windows)
#include "audio/AudioEngine.h"
#include <vector>
#include <functional>
#include <cmath>

// ==================================================================================================
// The rig's TWO-CLOCK streaming feeder (spec unit 2). Streams a mic-side StereoTimeline through the
// REAL engine callbacks — driveCaptureCallback / driveRenderCallback — in interleaved per-block
// cadence, with a controllable capture-vs-render clock skew (bridgePpm): the feeder accrues a
// fractional capture-frame debt of blockFrames*ppm*1e-6 per block and emits a one-frame-longer (or
// shorter) capture block whenever the debt crosses a whole frame. That makes the REAL ClockBridge
// SPSC FIFO + PI + FreezeGate fight a genuine two-clock load — the rig never models the bridge.
//
// ▸RV: block-cadence-quantized drift is faithful here — the PI loop's time constants (~1 s smoother,
// seconds-scale integrator) are two orders of magnitude slower than the 10 ms block cadence, so the
// per-block staircase is invisible to the loop. Realistic |ppm| <= 150 (crystal tolerances);
// 300-500 is a beyond-spec stress rung that must degrade HONESTLY, not silently.
// ==================================================================================================
namespace ebsim {

struct FeederSpec {
    int    blockFrames = 480;    // the WASAPI-shared tuning-point block (dtScale == 1 in ClockBridge)
    double bridgePpm   = 0.0;    // capture clock faster (+) / slower (-) than the render clock
};

struct RenderedOutput {
    std::vector<float> mono;             // the rendered cable feed (outL; the render is dual-mono L==R)
    long long          captureFramesFed = 0;
};

// Stream the WHOLE mic timeline. Each iteration: one capture push of N +/- the drift debt, then one
// render pull of exactly blockFrames (the render clock is the reference). The mic tail is zero-padded
// so the last partial block still streams. Deterministic: no threads, no timing — a plain loop.
// `perBlock` (optional) runs after each block — the orchestrator uses it to DRAIN the engine's grade
// snapshots at the GUI's cadence (the publish is single-buffered: it only refreshes once consumed).
inline RenderedOutput streamSession (eb::AudioEngine& e, const StereoTimeline& mic, const FeederSpec& f,
                                     const std::function<void()>& perBlock = {}) {
    RenderedOutput out;
    const int N = f.blockFrames > 0 ? f.blockFrames : 480;
    const size_t total = mic.L.size();
    out.mono.reserve (total + (size_t) N);

    jassert (f.bridgePpm > -1e6);   // ppm <= -1e6 would zero the block debt and spin forever (m3 guard)
    std::vector<float> capL ((size_t) N, 0.0f), capR ((size_t) N, 0.0f);
    std::vector<float> outL ((size_t) N, 0.0f),     outR ((size_t) N, 0.0f);

    // Drift is realised as callback RATE, never block SIZE: production capture callbacks always
    // deliver the negotiated block; a faster capture clock fires MORE callbacks per render block.
    // (The first cut varied nCap by +-1 frame - the engine's prepared per-block scratch rightly
    // flagged the over-size delivery as a fault, which no real driver produces.) blockDebt accrues
    // (1 + ppm) capture blocks per render block; whole blocks fire, the fraction carries.
    double blockDebt = 0.0;
    size_t pos = 0;             // mic read position

    auto pushCaptureBlock = [&] {
        for (int i = 0; i < N; ++i) {
            const size_t j = pos + (size_t) i;
            capL[(size_t) i] = j < total ? mic.L[j] : 0.0f;
            capR[(size_t) i] = j < total ? mic.R[j] : 0.0f;
        }
        pos += (size_t) N;
        out.captureFramesFed += N;
        e.driveCaptureCallback (capL.data(), capR.data(), N);
    };

    while (pos < total) {
        blockDebt += 1.0 + f.bridgePpm * 1e-6;
        while (blockDebt >= 1.0) { pushCaptureBlock(); blockDebt -= 1.0; }
        e.driveRenderCallback (outL.data(), outR.data(), N);
        out.mono.insert (out.mono.end(), outL.begin(), outL.end());
        if (perBlock) perBlock();
    }
    return out;
}

} // namespace ebsim
