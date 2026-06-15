#pragma once
#include <cmath>

namespace eb {

// FIR tap count for a given sample rate: ~8192 * (rate / 48000), rounded UP to the next power of
// two. 8192@48k, 16384@96k, 32768@192k (44.1 / 88.2 / 176.4k round to the same). SINGLE SOURCE OF
// TRUTH shared by the engine (which actually builds the FIR) and the GUI (which displays/derives the
// length), so the two can never disagree at an odd rate. Previously this lived twice -- as
// firTapsForRate() in the engine (round-up) and numTapsForRate() in the GUI (round-nearest).
inline int firTapsForRate (double sampleRate) {
    const double scaled = 8192.0 * (sampleRate / 48000.0);
    const int v = (int) std::ceil (scaled);
    int p = 1; while (p < v) p <<= 1;
    return p;
}

} // namespace eb
