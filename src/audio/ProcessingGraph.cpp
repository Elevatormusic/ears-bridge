#include "audio/ProcessingGraph.h"
#include <cmath>
#include <vector>

namespace eb {

void ProcessingGraph::prepare (double sampleRate, int maxBlockSize) {
    sr = sampleRate; maxBlock = maxBlockSize;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
    convL.prepare (spec); convR.prepare (spec);
    scratch.setSize (2, maxBlockSize);
    envL_ = envR_ = 0.0f; activeEar_.store (0, std::memory_order_relaxed);   // fresh AutoPerEar state
    router_.reset(); autoEarAmbiguous_.store (false, std::memory_order_relaxed);
    relCoeff_ = std::exp (-(float) maxBlockSize / (float) (sampleRate * 0.08));  // ~80 ms release, off-thread
    lastMode_ = combine.load();
}

void ProcessingGraph::clearFir (int channel) {
    juce::AudioBuffer<float> unit (1, 1); unit.setSample (0, 0, 1.0f);   // unity passthrough
    if (channel == 0) peakGainL_ = 1.0f; else peakGainR_ = 1.0f;
    auto& conv = (channel == 0) ? convL : convR;
    conv.loadImpulseResponse (std::move (unit), sr,
        juce::dsp::Convolution::Stereo::no,
        juce::dsp::Convolution::Trim::no,
        juce::dsp::Convolution::Normalise::no);
    recomputeHeadroom();   // keep peaks + headroomGain in sync (setFir/clearFir are the only writers)
}

// Measure this ear's FIR peak and load it into the matching convolution. Does NOT recompute headroom
// -- the caller does that once (so setFirPair can install both ears under a single recompute). Shared
// by setFir + setFirPair so the "measure peak BEFORE the IR is moved + load with the canonical flags"
// logic lives in exactly one place (DRY). Message-thread only; sole writer of the peaks + convolutions.
void ProcessingGraph::loadIrInto (int channel, juce::AudioBuffer<float>&& ir) {
    const float pk = peakGainOfIr (ir);               // measure BEFORE the IR is moved into the conv
    if (channel == 0) peakGainL_ = pk; else peakGainR_ = pk;
    auto& conv = (channel == 0) ? convL : convR;
    conv.loadImpulseResponse (std::move (ir), sr,
        juce::dsp::Convolution::Stereo::no,
        juce::dsp::Convolution::Trim::no,
        juce::dsp::Convolution::Normalise::no);
}

void ProcessingGraph::setFir (int channel, juce::AudioBuffer<float> ir) {
    loadIrInto (channel, std::move (ir));
    recomputeHeadroom();
}

void ProcessingGraph::setFirPair (juce::AudioBuffer<float> leftIr, juce::AudioBuffer<float> rightIr) {
    // Install BOTH ears, then recompute the shared headroom EXACTLY ONCE, so peakGainL_/peakGainR_ and
    // headroomGain are updated as one batch -- the audio thread can read a one-block-stale headroom (as
    // for setFir) but never a left-from-the-new-pair / right-from-the-old-pair mix. Same sole-writer
    // invariant as setFir/clearFir; this is just both ears under a single recompute (not two).
    loadIrInto (0, std::move (leftIr));
    loadIrInto (1, std::move (rightIr));
    recomputeHeadroom();   // ONCE for the pair (reads both peaks) -- not once per ear
}

bool ProcessingGraph::convolutionsLoaded (int taps) const {
    // We compare against `taps`, not just > 0, because juce::dsp::Convolution is constructed with a
    // DEFAULT 1-sample unit-impulse IR -- so getCurrentIRSize() is 1 (not 0) the moment prepare() runs,
    // before any setFir/setFirPair. A bare > 0 would read "loaded" for that passthrough default. The
    // real cal FIRs are always >> 1 tap (8 in tests, thousands in production), so >= taps cleanly
    // excludes the default. getCurrentIRSize() reports the EXACT loaded size here (load SR == prepared
    // SR, Trim::no, Normalise::no => JUCE neither resamples nor trims), and it only reaches that size
    // once the async background load completes AND a process() block swaps the new engine into
    // currentEngine -- so this reads true only after both ears are genuinely installed and a block ran.
    return convL.getCurrentIRSize() >= taps && convR.getCurrentIRSize() >= taps;
}

void ProcessingGraph::setCombineMode (CombineMode mode) {
    combine.store ((int) mode);
}

void ProcessingGraph::setSweepSchedule (const SweepSchedule& schedule) {
    // Message thread, while STOPPED. router_ copies the schedule; the audio thread only READS it in process(),
    // so install before Start (same sole-writer discipline as setFir) - never swap the schedule mid-run.
    router_.loadSchedule (schedule);
}

// Max magnitude of the IR's transfer function, |H(f)| = max_k |FFT(ir)[k]|. For a (near-)sinusoidal
// Dirac sweep the steady-state output at frequency f is input*|H(f)|, so this peak bounds the worst
// case the sweep can produce. Runs off the audio thread (setFir is message-thread).
float ProcessingGraph::peakGainOfIr (const juce::AudioBuffer<float>& ir) {
    jassert (ir.getNumChannels() == 1);               // setFir always passes a mono IR
    const int n = ir.getNumSamples();
    if (n <= 0) return 1.0f;
    int order = 1; while ((1 << order) < n) ++order;  // FFT length >= IR length
    ++order;                                          // x2 zero-pad: sample |H(f)| between bin centers
    const int fftSize = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) fftSize * 2, 0.0f);   // real input in [0,fftSize); real-only xform
    const float* d = ir.getReadPointer (0);
    for (int i = 0; i < n; ++i) buf[(size_t) i] = d[i];
    fft.performRealOnlyForwardTransform (buf.data());
    float peak = 0.0f;
    for (int k = 0; k <= fftSize / 2; ++k) {
        const float re = buf[(size_t) k * 2], im = buf[(size_t) k * 2 + 1];
        peak = juce::jmax (peak, std::sqrt (re * re + im * im));
    }
    return juce::jmax (peak, 1.0e-6f);
}

void ProcessingGraph::recomputeHeadroom() {
    // Bound by the louder ear's FIR peak only -- removes any cal-FIR boost so a 0 dBFS *sweep tone*
    // can't exceed 0 dBFS through Auto/single-ear/Average (for which 0.5*(L+R) <= max(peak) too). This is
    // the steady-state sinusoid bound, which is the right one for Dirac's slow log sweep; a broadband
    // transient could still nudge over, but the measurement signal is a sweep. Sum's intentional +6 dB
    // is NOT compensated (it owns the clip-risk warning). Attenuate only past unity; flat/cut stays transparent.
    const float pk = juce::jmax (peakGainL_, peakGainR_);
    headroomGain.store (pk > 1.0f ? 1.0f / pk : 1.0f);
}

void ProcessingGraph::setOutputGain (float linear) {
    outGain.store (juce::jlimit (0.0f, 4.0f, linear));   // clamp; 0 = mute, 1 = unity
}

// Replace any non-finite sample with 0 in place; return whether any were found. RT-safe (plain loop,
// no alloc/lock/log/throw) -- runs on the audio thread.
static bool sanitizeNonFinite (float* buf, int n) noexcept {
    bool bad = false;
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (buf[i])) { buf[i] = 0.0f; bad = true; }
    return bad;
}

bool ProcessingGraph::process (const float* inL, const float* inR,
                               float* outMono, int numSamples) {
    auto* l = scratch.getWritePointer (0);
    auto* r = scratch.getWritePointer (1);
    juce::FloatVectorOperations::copy (l, inL, numSamples);
    juce::FloatVectorOperations::copy (r, inR, numSamples);
    // Keep non-finite OUT of the stateful FFT convolution: a single NaN would smear through the
    // overlap-add and poison every later block (permanent silence) until reset.
    bool bad = sanitizeNonFinite (l, numSamples);
    bad = sanitizeNonFinite (r, numSamples) || bad;

    { juce::dsp::AudioBlock<float> b (&l, 1, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (b); convL.process (ctx); }
    { juce::dsp::AudioBlock<float> b (&r, 1, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (b); convR.process (ctx); }

    const int modeNow = combine.load();
    if (modeNow != lastMode_) {                  // live combine-mode change (user moved the dropdown)
        envL_ = envR_ = 0.0f;                     // re-arm AutoPerEar from clean envelopes so entering it
        lastMode_ = modeNow;                      // doesn't inherit a stale active-ear from before
    }
    switch ((CombineMode) modeNow) {
        case CombineMode::LeftOnly:
            juce::FloatVectorOperations::copy (outMono, l, numSamples); break;
        case CombineMode::RightOnly:
            juce::FloatVectorOperations::copy (outMono, r, numSamples); break;
        case CombineMode::Sum:
            juce::FloatVectorOperations::copy (outMono, l, numSamples);
            juce::FloatVectorOperations::add  (outMono, r, numSamples); break;
        case CombineMode::Average:
            for (int i = 0; i < numSamples; ++i) outMono[i] = 0.5f * (l[i] + r[i]); break;
        case CombineMode::AutoPerEar: {
            // Per-ear measurement: Dirac drives ONE earcup at a time, so feed ONLY the directly-driven ear's
            // calibrated mic (no open-back leakage summed in). Detect on the RAW input (inL/inR) so per-ear FIR
            // gain differences don't bias the choice; output the post-FIR l/r.
            float pkL = 0.0f, pkR = 0.0f;
            for (int i = 0; i < numSamples; ++i) { pkL = juce::jmax (pkL, std::abs (inL[i])); pkR = juce::jmax (pkR, std::abs (inR[i])); }
            const int prev = activeEar_.load (std::memory_order_relaxed);
            int  ear = prev;
            bool hardSwitch = false;   // schedule path: switches land in the inter-sweep GAP (silence) -> hard + click-free
            if (router_.hasSchedule()) {
                // PRIMARY: drive the ear from the learned schedule's clock - covers the quiet sweep extremes the
                // envelope misses + the trailing L. floorLin=0 -> the router's default ~-50 dBFS gate (== the fallback).
                const RouterOut o = router_.process (pkL, pkR, 0.0f, (double) numSamples / sr);
                ear = o.ear;
                autoEarAmbiguous_.store (o.ambiguous, std::memory_order_relaxed);
                hardSwitch = true;
            } else {
                // FALLBACK (no schedule learned / macOS): the mic-envelope router - HOLD through the inter-sweep
                // silence; switch only when the OTHER ear is >= 2x (6 dB) louder so leakage can't flip it mid-sweep.
                const float rel = (numSamples == maxBlock) ? relCoeff_   // precomputed; avoid exp() on the hot path
                                : std::exp (-(float) numSamples / (float) (sr * 0.08));   // exact for a rare short block
                envL_ = juce::jmax (pkL, envL_ * rel);
                envR_ = juce::jmax (pkR, envR_ * rel);
                if (envL_ > 0.0032f || envR_ > 0.0032f) {            // ~ -50 dBFS gate
                    if (ear == 0) { if (envR_ > envL_ * 2.0f) ear = 1; }
                    else          { if (envL_ > envR_ * 2.0f) ear = 0; }
                }
                autoEarAmbiguous_.store (false, std::memory_order_relaxed);   // the envelope path has no ambiguity signal
            }
            if (ear != prev) activeEar_.store (ear, std::memory_order_relaxed);   // publish for the GUI
            const float* cur = (ear == 0) ? l : r;
            if (ear != prev && ! hardSwitch) {                   // crossfade old->new (anti-click) ONLY in the envelope fallback
                const float* old = (prev == 0) ? l : r;
                const float inv = 1.0f / (float) juce::jmax (1, numSamples);
                for (int i = 0; i < numSamples; ++i) { const float t = (float) i * inv; outMono[i] = old[i] * (1.0f - t) + cur[i] * t; }
            } else {
                juce::FloatVectorOperations::copy (outMono, cur, numSamples);
            }
            break;
        }
    }

    // Post-combine output trim (the GUI's "Output trim (dB)" slider) composed with the auto makeup
    // headroom (kept <= 1 so the cal FIR can't clip the cable). Skip the multiply only at exact unity
    // to keep the common path free of a needless pass. The two atomics are read as a non-atomic pair by
    // design: a simultaneous trim change + FIR swap could show one stale for a block, but both are
    // bounded (outGain<=4, headroom<=1) and the clip() below caps the product, so it self-corrects.
    const float g = outGain.load() * headroomGain.load();
    if (g != 1.0f) juce::FloatVectorOperations::multiply (outMono, g, numSamples);

    // Catch a FIR-produced non-finite BEFORE the clamp: on x86/SSE FloatVectorOperations::clip turns a
    // NaN into 1.0, so a post-clamp scan would miss it and a garbage full-scale sample would reach Dirac.
    bad = sanitizeNonFinite (outMono, numSamples) || bad;

    // Final hard ceiling: the makeup keeps the steady-state sweep <= 0 dBFS, but a broadband transient
    // or the brief window where an async FIR swap runs the OLD (louder) IR under the NEW headroom could
    // momentarily overshoot. Clamp so the cable Dirac records can never see a sample past full scale.
    juce::FloatVectorOperations::clip (outMono, outMono, -1.0f, 1.0f, numSamples);
    return bad;
}

void ProcessingGraph::reset() {
    convL.reset(); convR.reset();
    envL_ = envR_ = 0.0f; activeEar_.store (0, std::memory_order_relaxed);
    router_.reset(); autoEarAmbiguous_.store (false, std::memory_order_relaxed);
    lastMode_ = combine.load();
}

} // namespace eb
