#pragma once
#include <algorithm>
#include <cmath>

namespace eb {

// Fixed display band for cal thumbnails (design-spec §8.2): 20 Hz .. 20 kHz, log x.
inline constexpr float kPlotFreqLo = 20.0f;
inline constexpr float kPlotFreqHi = 20000.0f;

// Map a frequency to an x pixel in [0, widthPx] on a log scale, clamped to the band.
inline float freqToX (float freqHz, float widthPx) {
    const float f  = std::clamp (freqHz, kPlotFreqLo, kPlotFreqHi);
    const float lo = std::log10 (kPlotFreqLo);
    const float hi = std::log10 (kPlotFreqHi);
    const float t  = (std::log10 (f) - lo) / (hi - lo);   // 0..1
    return t * widthPx;
}

// Inverse of freqToX: x pixel -> frequency (Hz). Useful for hover read-outs.
inline float xToFreq (float xPx, float widthPx) {
    const float lo = std::log10 (kPlotFreqLo);
    const float hi = std::log10 (kPlotFreqHi);
    const float t  = (widthPx <= 0.0f) ? 0.0f : std::clamp (xPx / widthPx, 0.0f, 1.0f);
    return std::pow (10.0f, lo + t * (hi - lo));
}

// Map a dB value to a y pixel in [0, heightPx]; topDb sits at y=0, botDb at y=heightPx
// (screen y grows downward), clamped to the range.
inline float dbToY (float db, float heightPx, float topDb, float botDb) {
    const float d = std::clamp (db, botDb, topDb);
    const float t = (topDb - d) / (topDb - botDb);   // 0 at top, 1 at bottom
    return t * heightPx;
}

} // namespace eb
