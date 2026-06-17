#pragma once
#include "audio/CombineMode.h"
#include "audio/FirTaps.h"   // firTapsForRate() — single source of truth for tap count
#include <vector>
#include <algorithm>
#include <cmath>

namespace eb {

// One row of the sample-rate ComboBox model.
struct RateMenuItem {
    double rate = 0.0;
    bool   selected = false;
    bool   resampleWarning = false;   // true when this rate is NOT a device-native rate
};

// Build the rate menu for a device whose native rates are `native`, given the
// currently-selected rate. Native rates appear in order (clean). If `selectedSr` is
// not among the native rates, it is appended as a flagged (resample-warning) entry so
// the user still sees their forced selection. Exactly one item is marked selected.
inline std::vector<RateMenuItem> buildRateMenu (const std::vector<double>& native,
                                                double selectedSr) {
    std::vector<RateMenuItem> out;
    out.reserve (native.size() + 1);
    bool selectedIsNative = false;
    for (double r : native) {
        const bool sel = std::abs (r - selectedSr) < 0.5;
        if (sel) selectedIsNative = true;
        out.push_back ({ r, sel, /*resampleWarning*/ false });
    }
    if (! selectedIsNative)
        out.push_back ({ selectedSr, /*selected*/ true, /*resampleWarning*/ true });
    return out;
}

// One row of the combine-mode selector, ordered by methodological rigor.
struct CombineMenuItem {
    CombineMode mode;
    bool recommended = false;       // AutoPerEar, the recommended Dirac headphone mode (one routine, both ears)
    bool clipRiskWarning = false;   // Sum: +6 dB level risk
};

// Combine modes, in menu order. The persisted setting is the stable CombineMode ENUM VALUE, and the
// GUI restores by SEARCHING this list for that mode (not by assuming combo position == enum value), so
// this order may now be reordered freely. (The CombineMode enum values themselves stay append-only --
// they are what Settings stores.) AutoPerEar is the recommended Dirac headphone mode.
inline std::vector<CombineMenuItem> combineModeOrder() {
    // AutoPerEar first: the recommended Dirac headphone mode and the default. LeftOnly/RightOnly give
    // explicit single-ear control; Average/Sum mix both ears. The persisted setting is the stable
    // CombineMode enum VALUE and the GUI restores by SEARCHING this list, so the order is free.
    return {
        { CombineMode::AutoPerEar,   /*recommended*/ true,  /*clipRisk*/ false },   // recommended, on top
        { CombineMode::LeftOnly,     /*recommended*/ false, /*clipRisk*/ false },
        { CombineMode::RightOnly,    /*recommended*/ false, /*clipRisk*/ false },
        { CombineMode::Average,      /*recommended*/ false, /*clipRisk*/ false },
        { CombineMode::Sum,          /*recommended*/ false, /*clipRisk*/ true  },
    };
}

// Tap count for the FIR at a given rate. Back-compat GUI name; delegates to the single source of
// truth in audio/FirTaps.h (round-UP to the next power of two), so the displayed/derived length
// always matches the FIR the engine actually builds. 8192@48k, 16384@96k, 32768@192k.
inline int numTapsForRate (double rate) { return firTapsForRate (rate); }

} // namespace eb
