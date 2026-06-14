#pragma once
#include "audio/EngineTypes.h"
#include <atomic>
namespace eb {

// Lock-free telemetry sink written from both audio callbacks (atomics only) and read by
// the GUI timer via snapshot()/levels(). cleanCapture latches false on the first fault.
class HealthMonitor {
public:
    void reset();

    // Called from audio threads (atomic stores/increments only).
    void reportXrun();                       // a device xrun / dropped callback
    void reportDroppedFrames (long long n);  // frames lost at the bridge (under/overrun)
    void setFifoFill (double frac);
    void setCaptureToRenderRatio (double r);
    void reportInLevels (float peakL, float peakR, bool clipL, bool clipR);
    void reportOutLevel  (float peakMono, bool clipOut);

    // Read from the GUI thread.
    Health snapshot() const;
    Levels levels()  const;

private:
    std::atomic<int>       xruns { 0 };
    std::atomic<long long> dropped { 0 };
    std::atomic<int>       fifoFillMilli { 500 };   // fill * 1000
    std::atomic<int>       ratioMicro { 1000000 };  // ratio * 1e6
    std::atomic<bool>      clean { true };

    std::atomic<int> inLm { 0 }, inRm { 0 }, outM { 0 };       // peak * 1000
    std::atomic<bool> cL { false }, cR { false }, cO { false };
};

} // namespace eb
