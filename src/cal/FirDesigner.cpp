#include "cal/FirDesigner.h"
#include <cmath>
#include <algorithm>

namespace eb {

// log-frequency linear interpolation of the cal magnitude (dB) at an arbitrary Hz,
// holding endpoint values outside [first,last].
static double interpCalDb (const std::vector<CalPoint>& pts, double hz) {
    if (hz <= pts.front().freqHz) return pts.front().splDb;
    if (hz >= pts.back().freqHz)  return pts.back().splDb;
    // binary search for the bracketing pair
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (pts[mid].freqHz <= hz) lo = mid; else hi = mid;
    }
    const double lf = std::log10 (pts[lo].freqHz), hf = std::log10 (pts[hi].freqHz);
    const double t  = (std::log10 (hz) - lf) / (hf - lf);
    return pts[lo].splDb + t * (pts[hi].splDb - pts[lo].splDb);
}

std::vector<float> FirDesigner::targetMagnitudeLinear (const CalFile& cal,
                                                       const FirDesignParams& p,
                                                       int fftSize) {
    const int nBins = fftSize / 2 + 1;
    std::vector<float> mag ((size_t) nBins, 1.0f);
    for (int k = 0; k < nBins; ++k) {
        double hz = (double) k * p.sampleRate / (double) fftSize;
        if (hz < 1.0) hz = 1.0; // avoid log(0) at DC; endpoint-hold covers it
        double db = interpCalDb (cal.points, hz);
        if (p.invert) db = -db;
        if (db > p.maxBoostDb) db = p.maxBoostDb;   // clamp boosts
        mag[(size_t) k] = (float) std::pow (10.0, db / 20.0);
    }
    return mag;
}

juce::AudioBuffer<float> FirDesigner::design (const CalFile&, const FirDesignParams& p) {
    // implemented in Task 5/6
    return juce::AudioBuffer<float> (1, p.numTaps);
}

} // namespace eb
