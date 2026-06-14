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

    Health snapshot() const;                 // now also carries flags + cleanCapture
    Levels levels()  const;

    // ---- Plan 4 additions (additive; do not remove or rename the Plan-2 methods above) ----

    // Thresholds (public constants so tests assert exact boundaries).
    static constexpr float  kClipLinear         = 0.8913f;   // -1.0 dBFS
    static constexpr float  kLowLevelLinear      = 0.00316f;  // -50 dBFS (peak over a sweep window)
    static constexpr double kDriftRatioTol       = 0.005;     // +/-0.5% sustained
    static constexpr int    kDriftSustainBlocks  = 8;         // consecutive out-of-tol blocks to latch
    static constexpr int    kLowLevelGraceBlocks = 64;        // ignore initial silence before the sweep starts

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
    DipGainProfile gainProfile() const noexcept { return DipGainProfile::forModel (model_); }

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

    std::atomic<int> driftRun { 0 };     // consecutive out-of-tol blocks
    std::atomic<int> blockCount { 0 };   // for the low-level grace window
};

} // namespace eb
