#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "audio/CombineMode.h"
#include <atomic>
namespace eb {
class ProcessingGraph {
public:
    void prepare (double sampleRate, int maxBlockSize);
    void setFir  (int channel, juce::AudioBuffer<float> ir);
    void clearFir (int channel);   // load a unit-impulse (passthrough) IR + reset that ear's headroom
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
private:
    // Auto makeup-attenuation so the per-ear correction FIR can never push the mono output past
    // 0 dBFS, whatever the playback level. Because Dirac normalizes the measurement, only the
    // RELATIVE response matters, so a frequency-flat global attenuation changes nothing about the
    // resulting filter -- it is free headroom. The same gain is applied to BOTH ears (it is derived
    // from the louder ear's FIR peak), so the measured L/R balance is preserved. (Sum's intentional
    // +6 dB is left alone -- it owns the clip-risk warning.) Recomputed off-thread on every setFir().
    void  recomputeHeadroom();
    static float peakGainOfIr (const juce::AudioBuffer<float>& ir);   // max |H(f)| of an IR

    juce::dsp::Convolution convL, convR;
    std::atomic<int> combine { (int) CombineMode::LeftOnly };
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
    int   lastMode_ = (int) CombineMode::LeftOnly;   // detect a live combine-mode change to re-arm AutoPerEar cleanly
};
}
