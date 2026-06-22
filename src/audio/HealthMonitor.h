#pragma once
#include <juce_core/juce_core.h>
#include "audio/EngineTypes.h"
#include "audio/NoiseFloorTracker.h"
#include <atomic>
#include <cmath>   // std::isfinite / std::abs for the header-inline blockPeak()

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
    void reportSweepRetimed() noexcept;      // a forced mid-sweep SRC correction (D6): invalidating
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

    // D5: clear ONLY the measurement-validity latches (sticky flags EXCEPT the per-run OsResampled
    // guidance, cleanCapture, clip-run + flat-run scratch, drift run, edge clip) WITHOUT touching the
    // per-run config (model/capacity/nominal) or the level/ratio telemetry the GUI is rendering. Called
    // on the sweep-onset edge so a clip/dropout BEFORE the sweep doesn't brand the sweep invalid.
    void resetMeasurementLatches() noexcept;

    // One additive per-render-block observer. framesWanted vs framesGot (got<wanted => silence
    // fill => Dropout + FifoStarved); ratio + fill are forwarded to the existing setters so there
    // is exactly one copy of that state. Advances the block counter (low-level grace window).
    void observeRenderBlock (int framesWanted, int framesGot,
                             double captureToRenderRatio, double fifoFillFrac,
                             bool frozen = false) noexcept;   // D6: frozen => held ratio is intentional, skip drift accrual

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

    // No-latch max |sample| over both channels for the CURRENT block (skips non-finite). Lets the
    // engine feed MeasurementSession the current block's peak BEFORE analyzeInputBlock, so the sweep
    // arms with zero lag and a clip ON the onset block is re-detected after the validity re-scope.
    static float blockPeak (const float* l, const float* r, int n) noexcept {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (std::isfinite (l[i])) pk = juce::jmax (pk, std::abs (l[i]));
            if (std::isfinite (r[i])) pk = juce::jmax (pk, std::abs (r[i]));
        }
        return pk;
    }

    // No-latch per-ear max |sample| for the CURRENT block (skips non-finite). Lets the engine feed
    // observeSweepPeak the per-ear numerator only while the sweep is active, without re-running
    // analyzeInputBlock's per-ear scan. RT-safe: a plain loop, no allocation.
    static void blockPeakPerEar (const float* l, const float* r, int n,
                                 float& outL, float& outR) noexcept {
        float pkL = 0.0f, pkR = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (std::isfinite (l[i])) pkL = juce::jmax (pkL, std::abs (l[i]));
            if (std::isfinite (r[i])) pkR = juce::jmax (pkR, std::abs (r[i]));
        }
        outL = pkL; outR = pkR;
    }

    // True once either ear has peaked at a healthy capture level (>= kGoodLevelLinear) since the last
    // reset. Lets the GUI separate a present-but-too-quiet capture (the low-SNR "tin-can" failure,
    // which sits ABOVE the -50 dBFS no-signal floor and so reads as "clean" today) from a good one.
    bool reachedGoodLevel() const noexcept { return reachedGood_.load(); }

    // ---- Measured noise floor (replaces the fixed armFloor SNR denominator once valid) ----
    // The engine re-baselines the floor automatically via prepare() -> reset() -> floor_.reset(); this
    // explicit entry is for direct/unit setup of a bare HealthMonitor (the engine does not call it).
    void prepareNoiseFloor (double sampleRate, int maxBlock) noexcept { floor_.prepare (sampleRate, maxBlock); }
    // Fed every capture block with the per-ear block PEAK (the engine already computes spkL/spkR). The
    // tracker self-gates on quiet windows, so this is called unconditionally. RT-safe.
    void observeFloorBlock (float pkL, float pkR, double blockSeconds) noexcept { floor_.observeBlock (pkL, pkR, blockSeconds); }
    float measuredFloorLinear (int ch) const noexcept { return floor_.floorLinear (ch); }
    float measuredFloorDbAveraged() const noexcept { return floor_.floorDbAveraged(); }
    bool  floorValid() const noexcept { return floor_.valid(); }

    // ---- SNR addition: per-ear in-sweep peak latch (the SNR numerator) ----
    // The engine calls this ONCE per capture block ONLY while the Dirac sweep is active
    // (session_.sweepActive()), with this block's per-ear max |sample|. Each ear keeps its own
    // running max over the whole SweepActive window so an asymmetric noisy earcup is caught (GAP 6).
    // RT-safe: capture-thread single writer, std::max + an atomic store, no alloc/lock.
    void observeSweepPeak (float pkL, float pkR) noexcept;

    // Max |sample| per ear accumulated while the sweep was active; 0.0 until a sweep runs. Read /1000
    // (the project's int-milli atomic-publish idiom). Reset on resetMeasurementLatches() + reset().
    float maxSweepPeakL() const noexcept { return maxSweepPeakLMilli_.load() / 1000.0f; }
    float maxSweepPeakR() const noexcept { return maxSweepPeakRMilli_.load() / 1000.0f; }

    // SNR review fix: zero ONLY the two per-ear sweep-peak atomics, leaving every other latch (clip-run,
    // validity, drift, meters) untouched. Called by the engine at the SweepActive->Complete edge AFTER
    // the verdict is computed, so the NEXT earcup sweep accumulates a FRESH numerator instead of reading
    // max() across both sweeps. resetMeasurementLatches() is tied to the FIRST onset only, so the per-ear
    // peaks must be scoped to one sweep HERE (per Complete). RT-safe: two atomic stores, no alloc/lock.
    void resetSweepPeaks() noexcept {
        maxSweepPeakLMilli_.store (0); maxSweepPeakRMilli_.store (0);
    }

    // Raise the LowSnr GUIDANCE flag (NOT invalidating). Called by the engine (Task 3) on a low-SNR
    // verdict at the SweepActive->Complete edge. Public entry to the private raise() for that one flag.
    void raiseLowSnr() noexcept { raise (HealthFlag::LowSnr); }

    // SNR review fix (Finding 3): publish the verdict's min-ear dB SNAPSHOTTED at the SweepActive->Complete
    // edge (dB*1000 fixed-point), the instant the verdict is computed. The GUI reads THIS frozen value to
    // name the dB, NOT a live recompute from the maxSweepPeak*/armFloor atomics, so the displayed number
    // can never drift away from the dB that raised the sticky LowSnr flag once the next sweep mutates the
    // atomics. RT-safe: one atomic store on the capture thread. NaN/Inf clamped to a sentinel before store.
    void publishCompletedSnrDb (float snrDbMin) noexcept {
        const float clamped = std::isfinite (snrDbMin) ? juce::jlimit (-200.0f, 200.0f, snrDbMin) : 0.0f;
        completedSnrDbMilli_.store ((int) std::lround (clamped * 1000.0f));
    }
    // Read the published min-ear sweep SNR dB of the sweep that last completed; 0.0 until one completes.
    // Message-thread read of the lock-free snapshot (the project's int-milli publish idiom).
    float completedSnrDb() const noexcept { return completedSnrDbMilli_.load() / 1000.0f; }

    // ---- Reference-Based Measurement Monitor (Plan 5) ----
    // Raise the GUIDANCE reference flags (NEITHER is invalidating — raise() leaves cleanCapture alone for
    // both since they are not in the invalidating mask). Called by the engine when a measurement was graded
    // against a learned reference. RefMismatch = the match-gate failed; RefLowQuality = matched but suspect.
    void raiseRefMismatch()   noexcept { raise (HealthFlag::RefMismatch); }
    void raiseRefLowQuality() noexcept { raise (HealthFlag::RefLowQuality); }

    // Publish the reference-grade verdict SNAPSHOT (the SNR lesson): the workflow state (RefMonState's
    // underlying int) + the IR-SNR dB + the THD % the grade used, frozen TOGETHER the instant the grade is
    // computed, so the displayed numbers can never drift away from the flag that was raised. Reference
    // grading is OFFLINE (off the audio thread), so this is a message/worker-thread store, not the audio
    // thread — but the lock-free atomic publish keeps the GUI read consistent. dB/% use the int-milli idiom.
    void publishRefGrade (int refMonState, float irSnrDb, float thdPercent) noexcept {
        refMonState_.store (refMonState);
        const float snr = std::isfinite (irSnrDb)    ? juce::jlimit (-200.0f, 200.0f, irSnrDb)    : 0.0f;
        const float thd = std::isfinite (thdPercent) ? juce::jlimit (   0.0f, 1000.0f, thdPercent) : 0.0f;
        refIrSnrDbMilli_.store ((int) std::lround (snr * 1000.0f));
        refThdPctMilli_.store  ((int) std::lround (thd * 1000.0f));
    }
    int   refMonState()  const noexcept { return refMonState_.load(); }
    float refIrSnrDb()   const noexcept { return refIrSnrDbMilli_.load() / 1000.0f; }
    float refThdPercent() const noexcept { return refThdPctMilli_.load() / 1000.0f; }

    // ---- D8 addition: runtime format revalidation ----
    // Call once from RenderCallback::audioDeviceAboutToStart with the device's granted format.
    // Stores the reference snapshot. RT-safe (atomic stores). A zero rate sentinel means
    // checkFormatChange is a no-op until notifyPreparedFormat is called.
    void notifyPreparedFormat(double sampleRate, int bitDepth, int numChannels) noexcept;

    // Call at the top of each render block with the live device format.
    // Raises HealthFlag::FormatChanged (invalidating) on any mismatch vs the stored snapshot.
    // No-op if notifyPreparedFormat has not yet been called (prepared rate == 0).
    // RT-safe: three atomic loads + compare; no allocation.
    void checkFormatChange(double sampleRate, int bitDepth, int numChannels) noexcept;

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

    // SNR numerator: per-ear running max |sample| over the SweepActive window (peak*1000 fixed-point,
    // matching the level atomics). Capture-thread single writer (observeSweepPeak); reset on
    // resetMeasurementLatches() + reset() + resetSweepPeaks() (per-Complete scope). 0 until a sweep runs.
    std::atomic<int>       maxSweepPeakLMilli_ { 0 }, maxSweepPeakRMilli_ { 0 };

    // SNR review fix (Finding 3): the min-ear sweep SNR dB published at the SweepActive->Complete edge
    // (dB*1000 fixed-point). Capture-thread single writer (publishCompletedSnrDb at the edge); GUI reads
    // completedSnrDb(). Frozen at the moment the verdict is computed so the displayed dB can't drift away
    // from the dB that raised the sticky LowSnr flag. 0 until a sweep completes; cleared in reset().
    std::atomic<int>       completedSnrDbMilli_ { 0 };

    // Reference-Based Measurement Monitor (Plan 5): the published grade snapshot. refMonState_ holds
    // RefMonState's underlying int (default 0 == NotLearned). The dB/% are int-milli fixed-point. Written
    // TOGETHER in publishRefGrade off the audio thread (offline grading); GUI reads the lock-free trio.
    std::atomic<int>       refMonState_     { 0 };   // RefMonState::NotLearned == 0
    std::atomic<int>       refIrSnrDbMilli_ { 0 };
    std::atomic<int>       refThdPctMilli_  { 0 };

    // Confirmed-clip detection. railRun*_ / longestRun_ are CAPTURE-THREAD-ONLY scratch (written only
    // in analyzeInputBlock); the *_A_ atomics publish to the GUI thread.
    int  railRunL_ = 0, railRunR_ = 0, longestRun_ = 0;
    float prevL_ = 0.0f, prevR_ = 0.0f;   // previous raw sample per channel (capture-thread scratch)
    std::atomic<int>  railSamplesL_ { 0 }, railSamplesR_ { 0 }, longestRunA_ { 0 };
    std::atomic<bool> clipConfirmed_ { false };

    std::atomic<int> driftRun { 0 };     // consecutive out-of-tol blocks
    std::atomic<int> blockCount { 0 };   // for the low-level grace window

    // D8: prepared-format snapshot. preparedRateHz_ = 0 means "not set yet" (no-op sentinel).
    // Written once per run from notifyPreparedFormat (called on the message thread before streaming
    // begins). Read every render block from checkFormatChange (audio thread). The window between
    // the write and the first audio block is safe because JUCE starts the callback AFTER
    // audioDeviceAboutToStart returns, which is where we write.
    std::atomic<int> preparedRateHz_   { 0 };    // rate rounded to nearest Hz, 0 = sentinel "not set"
    std::atomic<int> preparedBitDepth_ { 0 };
    std::atomic<int> preparedChannels_ { 0 };

    // Noise-floor primitive: measured per-channel ambient floor, fed the per-ear block peak each block.
    NoiseFloorTracker floor_;
};

} // namespace eb
