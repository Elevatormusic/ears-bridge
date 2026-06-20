#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"
#include <atomic>

namespace eb {

// Lock-free telemetry + dropout/drift state machine fed from both audio callbacks.
// Plan 2 created the canonical surface below; Plan 4 EXTENDS it additively (the Plan-2
// methods keep their exact names and behavior).
class HealthMonitor {
public:
    // ---- Plan 2 canonical surface (names/signatures unchanged) ----
    void reset();

    void reportXrun();                       // a device xrun / dropped callback
    void reportDroppedFrames (long long n);  // frames lost at the bridge (under/overrun)
    void setFifoFill (double frac);
    void setCaptureToRenderRatio (double r);
    void reportInLevels (float peakL, float peakR, bool clipL, bool clipR);
    void reportOutLevel  (float peakMono, bool clipOut);
    void reportRawRail (bool verified) noexcept;   // D2: raises OsResampled (guidance) when !verified

    Health snapshot() const;                 // now also carries flags + cleanCapture
    Levels levels()  const;

    // ---- Plan 4 additions (additive; do not remove or rename the Plan-2 methods above) ----

    // Thresholds (public constants so tests assert exact boundaries).
    static constexpr float  kClipLinear         = 0.8913f;   // -1.0 dBFS
    static constexpr float  kLowLevelLinear      = 0.00316f;  // -50 dBFS (peak over a sweep window)
    static constexpr float  kGoodLevelLinear     = 0.06310f;  // -24 dBFS (floor of a healthy capture level)
    static constexpr double kDriftRatioTol       = 0.005;     // +/-0.5% sustained
    static constexpr int    kDriftSustainBlocks  = 8;         // consecutive out-of-tol blocks to latch
    static constexpr int    kLowLevelGraceBlocks = 64;        // ignore initial silence before the sweep starts

    // Confirmed digital clip: a RUN of consecutive samples at/above the rail. A real ADC clip is a
    // flat-topped run; a clean full-scale sine touches the rail for a single sample (no run). On a
    // shared-mode FLOAT stream we cannot read exact integer rails, so this run-based proxy is the
    // strongest reliable signal (see docs/EARS_DIRAC_CLIPPING_AUDIT.md D2). 24-bit +FS ~= 0.99999988.
    static constexpr float kRailCeiling = 0.9999f;   // -0.00087 dBFS
    static constexpr int   kRailRunMin  = 3;         // consecutive rail samples => confirmed clip

    // A real digital clip flat-tops at the exact rail code (consecutive samples equal); a smooth
    // full-scale sine peak varies sample-to-sample. Require the rail run to be FLAT within this epsilon
    // so a clean loud low-frequency tone doesn't false-positive as a confirmed clip.
    static constexpr float kFlatRunEps = 1.0e-5f;

    // Configure per-run: stores the model (for gainProfile) + capacity + the NOMINAL
    // capture:render ratio (drift is measured against this, not 1.0), then reset()s all state.
    void prepare (EarsModel model, int fifoCapacityFrames, double nominalRatio = 1.0) noexcept;

    // One additive per-render-block observer. framesWanted vs framesGot (got<wanted => silence
    // fill => Dropout + FifoStarved); ratio + fill are forwarded to the existing setters so there
    // is exactly one copy of that state. Advances the block counter (low-level grace window).
    void observeRenderBlock (int framesWanted, int framesGot,
                             double captureToRenderRatio, double fifoFillFrac) noexcept;

    HealthFlag     flags() const noexcept;        // latched sticky condition flags
    bool           cleanCapture() const noexcept; // latched false on a measurement-invalidating condition

    // Edge-triggered "did the raw input clip since you last asked" — set on the audio thread, drained
    // (read-and-cleared) by the GUI poll. Unlike the sticky ClipInput flag this self-clears once
    // clipping stops, so a transient-warning hold can actually decay after the user lowers the gain,
    // while still never missing a one-block clip that occurred between two 30 Hz polls.
    bool recentInputClip() noexcept { return recentClip_.exchange (false); }
    DipGainProfile gainProfile() const noexcept { return DipGainProfile::forModel (model_); }

    // Scan a block for non-finite samples (NaN/Inf). Raises HealthFlag::NonFinite (invalidating)
    // and returns true if any are found, so the caller can zero the block before it reaches the cable.
    // RT-safe: a plain loop, no allocation. Called on the audio thread.
    bool scanAndFlagNonFinite (const float* buf, int n) noexcept;

    // Raise HealthFlag::NonFinite (invalidating) for a FIR-produced non-finite that ProcessingGraph
    // already sanitized -- analyzeInputBlock only sees the RAW input, so the callback uses process()'s
    // return to flag a non-finite the convolution/combine emitted. RT-safe: one atomic raise().
    void reportNonFinite() noexcept;

    // Peak + confirmed-clip-run + NaN/Inf analysis on the RAW per-channel input. Replaces the inline
    // peak loop in the capture callback; feeds the existing guidance path via reportInLevels at the
    // kClipLinear (-1 dBFS) near-rail threshold. RT-safe (plain loop + atomics).
    void analyzeInputBlock (const float* l, const float* r, int n) noexcept;
    bool clipConfirmed()   const noexcept { return clipConfirmed_.load(); }
    int  clipRailSamples() const noexcept { return railSamplesL_.load() + railSamplesR_.load(); }
    int  clipLongestRun()  const noexcept { return longestRunA_.load(); }

    // True once either ear has peaked at a healthy capture level (>= kGoodLevelLinear) since the last
    // reset. Lets the GUI separate a present-but-too-quiet capture (the low-SNR "tin-can" failure,
    // which sits ABOVE the -50 dBFS no-signal floor and so reads as "clean" today) from a good one.
    bool reachedGoodLevel() const noexcept { return reachedGood_.load(); }

private:
    void raise (HealthFlag f) noexcept;   // OR into flags; clear cleanCapture for invalidating flags

    EarsModel model_   = EarsModel::Unknown;
    int       capacity_ = 0;
    double    nominal_  = 1.0;   // nominal capture:render ratio; drift measured against THIS, not 1.0

    // Level/clip telemetry (peak*1000 fixed-point to keep the Plan-2 atomics-only contract).
    std::atomic<int>  inLm { 0 }, inRm { 0 }, outM { 0 };   // peak * 1000
    std::atomic<bool> cL { false }, cR { false }, cO { false };

    std::atomic<int>       xrunsA { 0 };
    std::atomic<long long> droppedA { 0 };
    std::atomic<int>       fifoFillMilli { 500 };   // fill * 1000
    std::atomic<int>       ratioMicro { 1000000 };  // ratio * 1e6

    std::atomic<unsigned>  flagBits { 0 };
    std::atomic<bool>      clean { true };
    std::atomic<bool>      recentClip_ { false };   // edge-triggered input clip, drained by recentInputClip()
    std::atomic<bool>      reachedGood_ { false };  // latched true once a healthy input peak is seen this run

    // Confirmed-clip detection. railRun*_ / longestRun_ are CAPTURE-THREAD-ONLY scratch (written only
    // in analyzeInputBlock); the *_A_ atomics publish to the GUI thread.
    int  railRunL_ = 0, railRunR_ = 0, longestRun_ = 0;
    float prevL_ = 0.0f, prevR_ = 0.0f;   // previous raw sample per channel (capture-thread scratch)
    std::atomic<int>  railSamplesL_ { 0 }, railSamplesR_ { 0 }, longestRunA_ { 0 };
    std::atomic<bool> clipConfirmed_ { false };

    std::atomic<int> driftRun { 0 };     // consecutive out-of-tol blocks
    std::atomic<int> blockCount { 0 };   // for the low-level grace window
};

} // namespace eb
