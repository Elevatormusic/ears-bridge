#include "audio/ClockBridge.h"
#include <algorithm>
#include <cmath>
namespace eb {

void ClockBridge::prepare (double capRate, double renRate, int /*channels*/, int capacityFrames) {
    captureRate = capRate; renderRate = renRate;
    capacity = juce::jmax (1024, capacityFrames);
    ring.assign ((size_t) capacity, 0.0f);
    fifo.setTotalSize (capacity);
    fifo.reset();
    // Worst-case SRC needs ceil(ratio*numOut)+a few guard samples; size generously.
    srcInput.assign ((size_t) capacity, 0.0f);
    src.reset();
    smoothedFill = kTargetFill; ratioTrim = 1.0; integ = 0.0; avgRatioTrim_ = 1.0; avgCount_ = 0;
    sweepActive_.store (false); emergencyCorrection_.store (false);
    freezeArmed_ = false; frozenRatio_ = captureRate / juce::jmax (1.0, renderRate);
    underrunCount.store (0); overrunCount.store (0); droppedFrameCount.store (0);
    publishedFill.store (kTargetFill);
    publishedRatio.store (captureRate / juce::jmax (1.0, renderRate));
}

void ClockBridge::setRenderRate (double r) { renderRate = r; }

void ClockBridge::reset() {
    fifo.reset();
    std::fill (ring.begin(), ring.end(), 0.0f);
    src.reset();
    smoothedFill = kTargetFill; ratioTrim = 1.0; integ = 0.0; avgRatioTrim_ = 1.0; avgCount_ = 0;
    sweepActive_.store (false); emergencyCorrection_.store (false);
    freezeArmed_ = false; frozenRatio_ = captureRate / juce::jmax (1.0, renderRate);
    underrunCount.store (0); overrunCount.store (0); droppedFrameCount.store (0);
    publishedFill.store (kTargetFill);
    publishedRatio.store (captureRate / juce::jmax (1.0, renderRate));
}

void ClockBridge::prime (int silentFrames) {
    // Pre-fill with silence up to the requested frames, clamped to free space so it can never
    // overflow (no overrun event, no dropped-frame accounting). Seed the smoothed-fill state to
    // the actual primed fraction so the PI loop starts at equilibrium (errFill ~ 0 -> ratioTrim ~ 1
    // -> ratio at nominal), rather than chasing a stale 0.5 while the FIFO is already there.
    const int n = juce::jlimit (0, fifo.getFreeSpace(), silentFrames);
    if (n > 0) {
        int s1, sz1, s2, sz2;
        fifo.prepareToWrite (n, s1, sz1, s2, sz2);
        if (sz1 > 0) juce::FloatVectorOperations::clear (ring.data() + s1, sz1);
        if (sz2 > 0) juce::FloatVectorOperations::clear (ring.data() + s2, sz2);
        fifo.finishedWrite (sz1 + sz2);
    }
    smoothedFill = (double) fifo.getNumReady() / (double) juce::jmax (1, capacity);
    publishedFill.store (smoothedFill);
}

void ClockBridge::primeToTarget() {
    prime ((int) std::lround (kTargetFill * (double) capacity));
}

void ClockBridge::pushCapture (const float* mono, int numFrames) {
    int free = fifo.getFreeSpace();
    if (numFrames > free) {                       // ring full: producer frames are lost
        overrunCount.fetch_add (1);               // one overrun EVENT
        droppedFrameCount.fetch_add ((long long) (numFrames - free));  // actual FRAMES lost
        numFrames = free;                         // write only what fits; lock-free, no alloc
    }
    int s1, sz1, s2, sz2;
    fifo.prepareToWrite (numFrames, s1, sz1, s2, sz2);
    if (sz1 > 0) juce::FloatVectorOperations::copy (ring.data() + s1, mono, sz1);
    if (sz2 > 0) juce::FloatVectorOperations::copy (ring.data() + s2, mono + sz1, sz2);
    fifo.finishedWrite (sz1 + sz2);
}

int ClockBridge::pullRender (float* out, int numFrames) {
    // --- Fill observation (always current, even while frozen, so fifoFill() stays honest). ---
    const double fillFrac = (double) fifo.getNumReady() / (double) capacity;
    smoothedFill += 0.01 * (fillFrac - smoothedFill);        // 1-pole smoother (steering only)
    publishedFill.store (smoothedFill);

    const double nominal = captureRate / renderRate;          // input samples per output sample
    double ratio;
    if (sweepActive_.load()) {
        // --- D6 FROZEN: hold the converged trim; do NOT advance the PI integrator (no creep). ---
        // Snapshot the slow AVERAGE of ratioTrim (~= the true clock ratio), NOT the instantaneous PI value.
        // The PI oscillates around the true ratio by up to ~500 ppm; freezing an instantaneous sample held a
        // sustained ratio error that accumulated a timing drift across the sweep -> worst at the HF end (a log
        // sweep ends high), degrading the top of the measurement. The average carries ~no sustained offset.
        if (! freezeArmed_) { frozenRatio_ = nominal * avgRatioTrim_; freezeArmed_ = true; }   // snapshot once
        ratio = frozenRatio_;
        // RAW fill (not the lagged smoother) for the imminence check: the held ratio can no longer keep
        // the FIFO and a real drop/insert is imminent -> flag NOW. Do NOT steer (that is the creep D6 forbids).
        if (fillFrac < kFreezeFloor || fillFrac > kFreezeCeil)
            emergencyCorrection_.store (true);
    } else {
        // --- FREE: normal PI fill-control, steering fill toward kTargetFill. ---
        freezeArmed_ = false;                                 // re-arm for the next sweep
        const double errFill = smoothedFill - kTargetFill;
        integ = juce::jlimit (-0.02, 0.02, integ + 1.0e-4 * errFill);
        ratioTrim = juce::jlimit (0.97, 1.03, 1.0 + (2.0e-2 * errFill + integ));
        ratio = nominal * ratioTrim;
        // Running mean early (alpha 1/n), settling to a slow EMA (floor 0.001) once warm. The 1/n term makes
        // avgRatioTrim_ converge to the true ratio within a second or two of free-running, so a sweep that arrives
        // soon after start() snapshots the real ratio, not the stale 1.0 init (the cold-start ppm-skew trap).
        const double a = juce::jmax (0.001, 1.0 / (double) (++avgCount_));
        avgRatioTrim_ += a * (ratioTrim - avgRatioTrim_);
    }
    publishedRatio.store (ratio);                             // expose to currentRatio() (lock-free)

    // Input samples the interpolator MAY need for numFrames outputs (upper bound).
    int needIn = (int) std::ceil (ratio * numFrames) + 4;
    needIn = juce::jmin (needIn, (int) srcInput.size());
    const int avail = fifo.getNumReady();
    const int toRead = juce::jmin (needIn, avail);

    // Peek toRead samples into the scratch buffer WITHOUT advancing the read pointer yet.
    int s1, sz1, s2, sz2;
    fifo.prepareToRead (toRead, s1, sz1, s2, sz2);
    if (sz1 > 0) juce::FloatVectorOperations::copy (srcInput.data(), ring.data() + s1, sz1);
    if (sz2 > 0) juce::FloatVectorOperations::copy (srcInput.data() + sz1, ring.data() + s2, sz2);

    if (toRead < needIn)
        underrunCount.fetch_add (1);   // FIFO starved: interpolator zero-pads the tail (wrapAround = 0)

    // process() RETURNS the actual number of input samples consumed (~ratio*numFrames, NOT toRead).
    // Advance the FIFO by exactly that, so the stateful interpolator's input stays continuous and the
    // FIFO drains at the true rate (advancing by toRead would skip samples and slowly empty the FIFO).
    const int usedIn = src.process (ratio, srcInput.data(), out, numFrames, toRead, 0);
    fifo.finishedRead (juce::jmin (usedIn, toRead));
    return numFrames;
}

double ClockBridge::fifoFill()  const { return publishedFill.load(); }
int    ClockBridge::underruns() const { return underrunCount.load(); }
int    ClockBridge::overruns()  const { return overrunCount.load(); }
long long ClockBridge::droppedCaptureFrames() const { return droppedFrameCount.load(); }
double ClockBridge::currentRatio() const { return publishedRatio.load(); }

void ClockBridge::setSweepActive (bool active) noexcept { sweepActive_.store (active); }
bool ClockBridge::sweepActive() const noexcept { return sweepActive_.load(); }
bool ClockBridge::consumeEmergencyCorrection() noexcept { return emergencyCorrection_.exchange (false); }

} // namespace eb
