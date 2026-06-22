#pragma once
#include "audio/NoiseFloorMath.h"
#include <array>
#include <atomic>

namespace eb {

constexpr double kFloorSustainSec = 0.5;   // sustained quiet before a window folds (AES-2id pre-sweep >=0.5s)

// Per-channel measured ambient/amp-hiss floor. Fed the per-ear block PEAK every capture block; folds a
// sustained quiet window (>= kFloorSustainSec of consecutive below-ceiling blocks) into the floor via a
// median + slow blend. The FIRST sustained window is the pre-sweep-silence baseline; later windows
// (inter-sweep gaps) refine it. RT-safe: plain loops + atomics; the only non-trivial op is a bounded
// std::sort over <=kWindowCap elements at fold time (infrequent, no allocation).
class NoiseFloorTracker {
public:
    void prepare (double sampleRate, int /*maxBlock*/) noexcept { sr_ = sampleRate; reset(); }

    void reset() noexcept {
        for (int c = 0; c < 2; ++c) {
            count_[c] = 0; quietSec_[c] = 0.0;
            floorMilli_[c].store (0);
        }
        valid_.store (false);
    }

    void observeBlock (float levelL, float levelR, double blockSeconds) noexcept {
        const float lv[2] = { levelL, levelR };
        for (int c = 0; c < 2; ++c) {
            if (isQuietBlock (lv[c], kQuietCeilingLin)) {
                if (count_[c] < kWindowCap) win_[c][(size_t) count_[c]++] = lv[c];
                quietSec_[c] += blockSeconds;
                if (quietSec_[c] >= kFloorSustainSec) {
                    const float est  = robustLowFloor (win_[c].data(), count_[c]);
                    const float cur  = (float) floorMilli_[c].load() / 1.0e6f;
                    const float next = blendFloor (cur, est, kFloorBlendAlpha);
                    floorMilli_[c].store ((int) std::lround (next * 1.0e6f));
                    valid_.store (true);
                    count_[c] = 0; quietSec_[c] = 0.0;   // start the next window fresh
                }
            } else {
                count_[c] = 0; quietSec_[c] = 0.0;       // a loud / non-quiet block breaks the run
            }
        }
    }

    float floorLinear (int ch) const noexcept { return (float) floorMilli_[ch & 1].load() / 1.0e6f; }
    float floorDb (int ch) const noexcept {
        return 20.0f * std::log10 (std::max (floorLinear (ch), 1.0e-9f));
    }
    float floorDbAveraged() const noexcept { return averageFloorDb (floorLinear (0), floorLinear (1)); }
    bool  valid() const noexcept { return valid_.load(); }

private:
    static constexpr int kWindowCap = 256;   // ~2.5 s of 10 ms blocks; bounds the per-fold sort
    double sr_ = 48000.0;
    std::array<std::array<float, kWindowCap>, 2> win_ {};
    int    count_[2] { 0, 0 };
    double quietSec_[2] { 0.0, 0.0 };
    std::atomic<int>  floorMilli_[2] { {0}, {0} };   // floor * 1e6 (linear), atomic-published
    std::atomic<bool> valid_ { false };
};

} // namespace eb
