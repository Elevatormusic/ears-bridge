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
    bool recommended = false;       // the two-pass single-ear workflow
    bool clipRiskWarning = false;   // Sum: +6 dB level risk
};

// Combine modes. ORDER MUST MATCH the CombineMode enum (the GUI indexes combo id == enum value):
// TwoPass-L, TwoPass-R, Average, Sum, AutoPerEar. AutoPerEar is the recommended Dirac headphone mode.
inline std::vector<CombineMenuItem> combineModeOrder() {
    return {
        { CombineMode::TwoPassLeft,  /*recommended*/ true,  /*clipRisk*/ false },
        { CombineMode::TwoPassRight, /*recommended*/ true,  /*clipRisk*/ false },
        { CombineMode::Average,      /*recommended*/ false, /*clipRisk*/ false },
        { CombineMode::Sum,          /*recommended*/ false, /*clipRisk*/ true  },
        { CombineMode::AutoPerEar,   /*recommended*/ true,  /*clipRisk*/ false },
    };
}

// Tap count for the FIR at a given rate. Back-compat GUI name; delegates to the single source of
// truth in audio/FirTaps.h (round-UP to the next power of two), so the displayed/derived length
// always matches the FIR the engine actually builds. 8192@48k, 16384@96k, 32768@192k.
inline int numTapsForRate (double rate) { return firTapsForRate (rate); }

} // namespace eb
