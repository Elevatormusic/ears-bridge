#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "audio/CombineMode.h"
#include <atomic>
#include <cmath>
namespace eb {
class ProcessingGraph {
public:
    void prepare (double sampleRate, int maxBlockSize);
    void setFir  (int channel, juce::AudioBuffer<float> ir);
    // Install BOTH ear FIRs and recompute the shared headroom in ONE message-thread call, so the
    // audio thread never observes a mixed (new-left/old-right or new-FIR/old-headroom) pair. This is
    // the GUI build path; setFir stays for clearFir's unity path and single-ear tests. Same sole-writer
    // invariant as setFir/clearFir (convL/convR/peaks/headroomGain), just batched into one update.
    void setFirPair (juce::AudioBuffer<float> leftIr, juce::AudioBuffer<float> rightIr);
    void clearFir (int channel);   // load a unit-impulse (passthrough) IR + reset that ear's headroom

    // Best-effort poll: true once BOTH convolutions report the loaded cal IR via getCurrentIRSize (which
    // only updates after a process() block has run, so it lags the install by one block). RESERVED for the
    // deferred in-stream readiness barrier; it is NOT consulted by calibrationApplied() (that gate is owned
    // entirely by the generation-id match, requested == built == applied).
    // `taps` is the expected FIR length: we require getCurrentIRSize() >= taps, NOT just > 0, because a
    // freshly prepared() Convolution already holds JUCE's DEFAULT 1-sample passthrough IR (size 1 before
    // any setFir/setFirPair) -- a > 0 check would wrongly report that default as "loaded". In our usage
    // (load SR == prepared SR, Trim::no, Normalise::no) JUCE neither resamples nor trims, so it reports
    // the EXACT loaded size and >= taps is precise. getCurrentIRSize() only reaches that size after a
    // process() block has run, so this returns false until the graph has processed at least one block
    // post-install (the tests settle by processing, mirroring the audio thread).
    bool convolutionsLoaded (int taps) const;
    void setCombineMode (CombineMode mode);
    void setOutputGain (float linear);   // applied to the mono output (the "Output trim" control)
    // Returns true if it replaced any non-finite sample (in the input scratch before convolution, or in
    // the output before the clamp), so the audio callback can flag a FIR-produced non-finite. Sanitizing
    // the input scratch keeps a NaN OUT of the stateful FFT convolution (its overlap-add would otherwise
    // be poisoned, silencing every later block).
    bool process (const float* inL, const float* inR, float* outMono, int numSamples);
    void reset();

    // AutoPerEar: which earcup the graph is currently feeding to the mono output (0 = left, 1 = right).
    // Published lock-free for the GUI's "active side" indicator. Meaningful only while AutoPerEar is the
    // live mode with signal present; the GUI gates on mode + level.
    int activeEar() const noexcept { return activeEar_.load (std::memory_order_relaxed); }

    // How much (>= 0 dB) the auto makeup-headroom is attenuating the output, i.e. -20*log10(headroomGain).
    // headroomGain is <= 1 (it only ever cuts), so this is always >= 0 dB; 0 when no attenuation (unity
    // FIRs or a cut/flat cal). The GUI surfaces this number so the user can add about that many dB of
    // positive Mic gain in Dirac to compensate for the attenuation EARS Bridge applies. Updates whenever a
    // cal loads (recomputeHeadroom runs on setFir/setFirPair/clearFir). Lock-free; GUI-timer read.
    float headroomAttenuationDb() const noexcept {
        const float g = headroomGain.load();
        return g > 0.0f ? -20.0f * std::log10 (g) : 0.0f;
    }
private:
    // Auto makeup-attenuation so the per-ear correction FIR can never push the mono output past
    // 0 dBFS, whatever the playback level. Because Dirac normalizes the measurement, only the
    // RELATIVE response matters, so a frequency-flat global attenuation changes nothing about the
    // resulting filter -- it is free headroom. The same gain is applied to BOTH ears (it is derived
    // from the louder ear's FIR peak), so the measured L/R balance is preserved. (Sum's intentional
    // +6 dB is left alone -- it owns the clip-risk warning.) Recomputed off-thread on every setFir().
    void  recomputeHeadroom();
    // Single-ear "measure peak + load into a conv" WITHOUT recomputing headroom. Shared by setFir and
    // setFirPair (DRY): each caller recomputes once afterwards (setFir once, setFirPair once for the pair),
    // so setFirPair can batch both ears under one recompute. Message-thread only (sole peaks/conv writer).
    void  loadIrInto (int channel, juce::AudioBuffer<float>&& ir);
    static float peakGainOfIr (const juce::AudioBuffer<float>& ir);   // max |H(f)| of an IR

    juce::dsp::Convolution convL, convR;
    std::atomic<int> combine { (int) CombineMode::AutoPerEar };
    std::atomic<float> outGain { 1.0f };   // post-combine output gain (1.0 = unity); lock-free for the audio thread
    // Auto makeup attenuation (<= 1), composed with outGain. Deliberately NOT reset in prepare()/reset()
    // -- convL/convR keep their loaded IRs across those, so it stays bound to the current FIRs; setFir
    // must remain the sole writer of both the peaks and this gain so they never desync.
    std::atomic<float> headroomGain { 1.0f };
    float peakGainL_ = 1.0f, peakGainR_ = 1.0f; // per-ear FIR peak |H(f)| (message-thread only)
    juce::AudioBuffer<float> scratch;
    double sr = 48000.0; int maxBlock = 0;

    // AutoPerEar state (audio-thread only; reset in prepare()/reset()). Tracks per-ear input
    // envelopes so the mode can follow whichever earcup Dirac is currently sweeping.
    float envL_ = 0.0f, envR_ = 0.0f;
    std::atomic<int> activeEar_ { 0 };   // 0 = left mic, 1 = right mic (published for the GUI indicator)
    float relCoeff_ = 0.0f; // AutoPerEar envelope-release coefficient, precomputed in prepare() (no per-block exp)
    int   lastMode_ = (int) CombineMode::AutoPerEar;   // detect a live combine-mode change to re-arm AutoPerEar cleanly
};
}
