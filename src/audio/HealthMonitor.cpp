#include "audio/HealthMonitor.h"
#include <algorithm>
#include <cmath>

namespace eb {

// ---- Plan 4 addition: flag bookkeeping ------------------------------------------------
void HealthMonitor::raise (HealthFlag f) noexcept {
    flagBits.fetch_or (static_cast<unsigned> (f));
    // Conditions that invalidate a measurement clear cleanCapture; pure guidance warnings do not.
    const unsigned invalidating =
        static_cast<unsigned> (HealthFlag::Xrun)          |
        static_cast<unsigned> (HealthFlag::Dropout)       |
        static_cast<unsigned> (HealthFlag::ExcessDrift)   |
        static_cast<unsigned> (HealthFlag::FifoStarved)   |
        static_cast<unsigned> (HealthFlag::ClipConfirmed) |
        static_cast<unsigned> (HealthFlag::NonFinite);
    if ((static_cast<unsigned> (f) & invalidating) != 0u)
        clean.store (false);
}

// ---- Plan 4 addition: per-run configure -----------------------------------------------
void HealthMonitor::prepare (EarsModel m, int fifoCapacityFrames, double nominalRatio) noexcept {
    model_ = m;
    capacity_ = fifoCapacityFrames;
    nominal_ = (nominalRatio > 0.0 ? nominalRatio : 1.0);
    reset();
}

// ---- Plan 2 canonical surface (bodies preserved; flag logic folded in where noted) -----
void HealthMonitor::reset() {
    xrunsA.store (0); droppedA.store (0);
    fifoFillMilli.store (500); ratioMicro.store (1000000);
    clean.store (true);
    inLm.store (0); inRm.store (0); outM.store (0);
    cL.store (false); cR.store (false); cO.store (false);
    flagBits.store (0);                 // Plan 4: clear the sticky flags on a fresh run
    recentClip_.store (false);
    reachedGood_.store (false);         // a fresh run has not yet reached a healthy capture level
    railRunL_ = railRunR_ = longestRun_ = 0;
    railSamplesL_.store (0); railSamplesR_.store (0); longestRunA_.store (0);
    clipConfirmed_.store (false);
    driftRun.store (0); blockCount.store (0);
}

void HealthMonitor::reportXrun() {
    xrunsA.fetch_add (1);
    raise (HealthFlag::Xrun);           // Plan 4: also latches cleanCapture=false (invalidating)
}

void HealthMonitor::reportDroppedFrames (long long n) {
    if (n > 0) {
        droppedA.fetch_add (n);
        // Dropped frames (capture-side overrun OR render-side underrun) are a Dropout: raise the
        // flag AND invalidate cleanCapture (raise() clears clean for invalidating flags). This keeps
        // the overrun path symmetric with the render-side FifoStarved+Dropout path -- previously an
        // overrun-only run latched cleanCapture=false but surfaced NO flag to explain why.
        raise (HealthFlag::Dropout);
    }
}

void HealthMonitor::setFifoFill (double frac) {
    fifoFillMilli.store ((int) std::lround (juce::jlimit (0.0, 1.0, frac) * 1000.0));
}

void HealthMonitor::setCaptureToRenderRatio (double r) {
    ratioMicro.store ((int) std::lround (r * 1.0e6));
}

void HealthMonitor::analyzeInputBlock (const float* l, const float* r, int n) noexcept {
    float pkL = 0.0f, pkR = 0.0f;
    int   railL = 0, railR = 0;
    bool  nonFinite = false, confirmed = false;
    for (int i = 0; i < n; ++i) {
        const float a = l[i], b = r[i];
        const bool fa = std::isfinite (a), fb = std::isfinite (b);
        if (! fa || ! fb) nonFinite = true;
        if (fa) {
            const float ma = std::abs (a);
            pkL = juce::jmax (pkL, ma);
            railRunL_ = (ma >= kRailCeiling) ? railRunL_ + 1 : 0;
            if (ma >= kRailCeiling) ++railL;
        } else {
            railRunL_ = 0;                 // a non-finite sample breaks THIS channel's run only
        }
        if (fb) {
            const float mb = std::abs (b);
            pkR = juce::jmax (pkR, mb);
            railRunR_ = (mb >= kRailCeiling) ? railRunR_ + 1 : 0;
            if (mb >= kRailCeiling) ++railR;
        } else {
            railRunR_ = 0;
        }
        longestRun_ = juce::jmax (longestRun_, juce::jmax (railRunL_, railRunR_));
        if (railRunL_ >= kRailRunMin || railRunR_ >= kRailRunMin) confirmed = true;
    }
    if (railL > 0) railSamplesL_.fetch_add (railL);
    if (railR > 0) railSamplesR_.fetch_add (railR);
    longestRunA_.store (longestRun_);
    if (nonFinite)  raise (HealthFlag::NonFinite);
    if (confirmed) { clipConfirmed_.store (true); raise (HealthFlag::ClipConfirmed); }
    // Meter CLIP LED latches at the rail (kRailCeiling, ~full scale) so a clean sweep peaking between
    // -1 and 0 dBFS does not read red before any sample is clamped. reportInLevels still raises the
    // ClipInput GUIDANCE flag at kClipLinear (-1 dBFS) via its internal `|| peak >= kClipLinear` check,
    // so only the visual latch moves to the rail; guidance is unchanged.
    reportInLevels (pkL, pkR, pkL >= kRailCeiling, pkR >= kRailCeiling);
}

void HealthMonitor::reportInLevels (float peakL, float peakR, bool clipL, bool clipR) {
    inLm.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakL) * 1000.0f));
    inRm.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakR) * 1000.0f));
    cL.store (clipL); cR.store (clipR);
    // Plan 4: raise ClipInput if the caller flagged a clip OR the peak crosses the threshold.
    if (clipL || clipR || peakL >= kClipLinear || peakR >= kClipLinear) {
        raise (HealthFlag::ClipInput);  // sticky (for measurement validity); does NOT invalidate cleanCapture
        recentClip_.store (true);       // edge-triggered companion for the self-clearing GUI warning
    }
    // Plan 4: low-level only AFTER the grace window has fully elapsed (strictly greater, so the
    // 64-block warm-up does not itself trip it) and only when both ears are quiet.
    if (blockCount.load() > kLowLevelGraceBlocks
        && peakL < kLowLevelLinear && peakR < kLowLevelLinear)
        raise (HealthFlag::LowLevel);   // guidance

    // Latch "reached a healthy capture level" the first time either ear peaks at/above the good floor.
    // Monotonic within a run (only set true here, cleared in reset()); no grace gate -- a peak this
    // loud is unambiguous signal, not warm-up noise. The GUI uses this to warn when a capture stays
    // too quiet for good SNR (sits above the no-signal floor but never reaches a usable level).
    if (peakL >= kGoodLevelLinear || peakR >= kGoodLevelLinear)
        reachedGood_.store (true);
}

void HealthMonitor::reportOutLevel (float peakMono, bool clipOut) {
    outM.store ((int) std::lround (juce::jlimit (0.0f, 8.0f, peakMono) * 1000.0f));
    cO.store (clipOut);
    if (clipOut || peakMono >= kClipLinear)
        raise (HealthFlag::ClipOutput); // Plan 4: guidance warning
}

bool HealthMonitor::scanAndFlagNonFinite (const float* buf, int n) noexcept {
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (buf[i])) { raise (HealthFlag::NonFinite); return true; }
    return false;
}

void HealthMonitor::reportNonFinite() noexcept { raise (HealthFlag::NonFinite); }

Health HealthMonitor::snapshot() const {
    Health h;
    h.xruns = xrunsA.load();
    h.droppedFrames = droppedA.load();
    h.fifoFill = fifoFillMilli.load() / 1000.0;
    h.captureToRenderRatio = ratioMicro.load() / 1.0e6;
    h.cleanCapture = clean.load();
    h.flags = static_cast<HealthFlag> (flagBits.load());   // Plan 4: surface sticky flags
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

// ---- Plan 4 addition: per-render-block observer (folds onto the Plan-2 setters) --------
void HealthMonitor::observeRenderBlock (int framesWanted, int framesGot,
                                        double captureToRenderRatio, double fifoFillFrac) noexcept {
    blockCount.fetch_add (1);
    setCaptureToRenderRatio (captureToRenderRatio);   // reuse the Plan-2 setter (one copy of state)
    setFifoFill (fifoFillFrac);                        // reuse the Plan-2 setter

    if (framesGot < framesWanted) {
        droppedA.fetch_add (static_cast<long long> (framesWanted - framesGot));
        raise (HealthFlag::FifoStarved);
        raise (HealthFlag::Dropout);
    }

    // Sustained drift state machine: count consecutive ratios deviating from the NOMINAL
    // capture:render ratio (not from 1.0 — a 96k->48k run has nominal ~2.0); latch at threshold.
    const double drift = (nominal_ > 0.0) ? std::abs (captureToRenderRatio / nominal_ - 1.0)
                                          : std::abs (captureToRenderRatio - 1.0);
    if (drift > kDriftRatioTol) {
        const int run = driftRun.fetch_add (1) + 1;
        if (run >= kDriftSustainBlocks) raise (HealthFlag::ExcessDrift);
    } else {
        driftRun.store (0);
    }
}

HealthFlag HealthMonitor::flags() const noexcept { return static_cast<HealthFlag> (flagBits.load()); }
bool       HealthMonitor::cleanCapture() const noexcept { return clean.load(); }

} // namespace eb
