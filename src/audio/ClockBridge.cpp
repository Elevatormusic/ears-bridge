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
    resampler_.prepare (captureRate, renderRate);
    readPhase_ = resampler_.halfLength() - 1;            // first output centers on primed history
    resizeScratch();
    smoothedFill = kTargetFill; ratioTrim = 1.0; integ = 0.0; avgRatioTrim_ = 1.0; avgCount_ = 0;
    sweepActive_.store (false); emergencyCorrection_.store (false);
    freezeArmed_ = false; frozenRatio_ = captureRate / juce::jmax (1.0, renderRate);
    underrunCount.store (0); overrunCount.store (0); droppedFrameCount.store (0);
    publishedFill.store (kTargetFill);
    publishedRatio.store (captureRate / juce::jmax (1.0, renderRate));
}

void ClockBridge::setRenderRate (double r) {
    renderRate = r;
    resampler_.prepare (captureRate, renderRate);        // rebuild the prototype for the new ratio (setup thread)
    readPhase_ = resampler_.halfLength() - 1;
    resizeScratch();   // #19: a granted rate can push the nominal ratio past the fixed sizing floor; this runs
                       // in audioDeviceAboutToStart (setup thread, pre-stream) where realloc is already done above
}

// Scratch must hold the worst-case needIn so the stateless FIR never reads OOB (even on starve): up to
// (worstRatio*kMaxRatioTrim)*kMaxRenderBlock output span + the filter length. #19: worstRatio = the LARGER of
// the fixed floor kMaxRatio and the ACTUAL nominal ratio - the fixed 4.0 was billed as "the worst case" but
// 192k->44.1k is already 4.354, and a device unilaterally granting a very low render rate goes higher still.
// The *kMaxRatioTrim is essential - the PI raises the effective ratio to nominal*1.03 (review fix).
void ClockBridge::resizeScratch() {
    const double nominal = captureRate / juce::jmax (1.0, renderRate);
    const double worst   = juce::jmax (kMaxRatio, nominal) * kMaxRatioTrim;
    srcInput.assign ((size_t) juce::jmax (capacity,
                     (int) std::ceil (worst * kMaxRenderBlock) + eb::PolyphaseResampler::kMaxLen + 8), 0.0f);
}

void ClockBridge::reset() {
    fifo.reset();
    std::fill (ring.begin(), ring.end(), 0.0f);
    readPhase_ = resampler_.halfLength() - 1;
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
    const double fillFrac = (double) fifo.getNumReady() / (double) juce::jmax (1, capacity);   // guard capacity==0 (pre-prepare)
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
        ratioTrim = juce::jlimit (kMinRatioTrim, kMaxRatioTrim, 1.0 + (2.0e-2 * errFill + integ));
        ratio = nominal * ratioTrim;
        // Running mean early (alpha 1/n), settling to a slow EMA (floor 0.001) once warm. The 1/n term makes
        // avgRatioTrim_ converge to the true ratio within a second or two of free-running, so a sweep that arrives
        // soon after start() snapshots the real ratio, not the stale 1.0 init (the cold-start ppm-skew trap).
        const double a = juce::jmax (0.001, 1.0 / (double) (++avgCount_));
        avgRatioTrim_ += a * (ratioTrim - avgRatioTrim_);
    }
    publishedRatio.store (ratio);                             // expose to currentRatio() (lock-free)

    // --- Windowed-sinc polyphase resample via the consumer-owned phase accumulator. Produced in chunks
    // of at most kMaxRenderBlock so the FIR scratch (sized to that worst case) is never read out of bounds
    // for ANY caller block size; readPhase_ carries continuously across chunks. The PI/freeze ratio above
    // is computed once per call. Normal blocks (<= kMaxRenderBlock) run a single iteration. ---
    const double inc = ratio;                                  // nominal*ratioTrim (free) or frozenRatio_ (frozen)
    const int    Lh  = resampler_.halfLength();
    for (int done = 0; done < numFrames; ) {
        const int chunk = juce::jmin (numFrames - done, kMaxRenderBlock);
        // Highest srcInput index the resampler reads for these `chunk` outputs (= floor(last readPhase)+L/2).
        int needIn = (int) std::floor (readPhase_ + (chunk - 1) * inc) + Lh + 1;
        if (needIn > (int) srcInput.size()) {
            // readPhase_ ran away: SUSTAINED render-starvation (render pulling while capture is stalled/dead) never
            // retires readPhase_ (advance clamps to toRead=0), so it grows by chunk*inc per block until the FIR would
            // read past the scratch. Reset to the safe prime point - output is the zero-padded scratch (silence) until
            // capture resumes, which is correct for a dead input. With srcInput sized for kMaxRatio*kMaxRatioTrim a
            // NON-starved readPhase_ (~Lh) never trips this branch; only the runaway does.
            readPhase_ = (double) (Lh - 1);
            needIn = (int) std::floor (readPhase_ + (chunk - 1) * inc) + Lh + 1;
        }
        needIn = juce::jmin (needIn, (int) srcInput.size());
        const int avail  = fifo.getNumReady();
        const int toRead = juce::jmin (needIn, avail);

        // Peek toRead samples into the scratch WITHOUT advancing the read pointer; the resampler needs the
        // L/2-1 left history that stays in the FIFO between blocks (it is stateless).
        int s1, sz1, s2, sz2;
        fifo.prepareToRead (toRead, s1, sz1, s2, sz2);
        if (sz1 > 0) juce::FloatVectorOperations::copy (srcInput.data(),       ring.data() + s1, sz1);
        if (sz2 > 0) juce::FloatVectorOperations::copy (srcInput.data() + sz1, ring.data() + s2, sz2);
        if (toRead < needIn) {                                 // FIFO starved: zero-pad the tail the FIR reads
            juce::FloatVectorOperations::clear (srcInput.data() + toRead, (int) srcInput.size() - toRead);
            underrunCount.fetch_add (1);
        }

        for (int j = 0; j < chunk; ++j) { out[done + j] = resampler_.sampleAt (srcInput.data(), readPhase_); readPhase_ += inc; }

        // Retire only input fully behind the filter's left support; carry L/2-1 samples as history. This is
        // the exact, deterministic replacement for the old stateful interpolator's returned usedIn.
        int advance = (int) std::floor (readPhase_) - (Lh - 1);
        advance = juce::jlimit (0, toRead, advance);
        fifo.finishedRead (advance);
        readPhase_ -= advance;
        done += chunk;
    }
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
