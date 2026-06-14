#include "audio/HealthMonitor.h"
#include <algorithm>
#include <cmath>
namespace eb {

void HealthMonitor::reset() {
    xruns.store (0); dropped.store (0);
    fifoFillMilli.store (500); ratioMicro.store (1000000);
    clean.store (true);
    inLm.store (0); inRm.store (0); outM.store (0);
    cL.store (false); cR.store (false); cO.store (false);
}

void HealthMonitor::reportXrun() { xruns.fetch_add (1); clean.store (false); }

void HealthMonitor::reportDroppedFrames (long long n) {
    if (n > 0) { dropped.fetch_add (n); clean.store (false); }
}

void HealthMonitor::setFifoFill (double frac) {
    fifoFillMilli.store ((int) std::lround (juce::jlimit (0.0, 1.0, frac) * 1000.0));
}

void HealthMonitor::setCaptureToRenderRatio (double r) {
    ratioMicro.store ((int) std::lround (r * 1.0e6));
}

void HealthMonitor::reportInLevels (float peakL, float peakR, bool clipL, bool clipR) {
    inLm.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakL) * 1000.0f));
    inRm.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakR) * 1000.0f));
    cL.store (clipL); cR.store (clipR);
}

void HealthMonitor::reportOutLevel (float peakMono, bool clipOut) {
    outM.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakMono) * 1000.0f));
    cO.store (clipOut);
}

Health HealthMonitor::snapshot() const {
    Health h;
    h.xruns = xruns.load();
    h.droppedFrames = dropped.load();
    h.fifoFill = fifoFillMilli.load() / 1000.0;
    h.captureToRenderRatio = ratioMicro.load() / 1.0e6;
    h.cleanCapture = clean.load();
    return h;
}

Levels HealthMonitor::levels() const {
    Levels lv;
    lv.inL = inLm.load() / 1000.0f;
    lv.inR = inRm.load() / 1000.0f;
    lv.outMono = outM.load() / 1000.0f;
    lv.clipL = cL.load(); lv.clipR = cR.load(); lv.clipOut = cO.load();
    return lv;
}

} // namespace eb
