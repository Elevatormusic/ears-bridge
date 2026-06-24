#pragma once

// Mirror of hig-thresholds.json (the single source consumed by the apple-hig native-review.mjs). The golden
// parity test (tests/test_hig_parity.cpp) fails if these drift from native-review's behaviour, so the C++
// build gate and the JS advisory never disagree on a finding. Keep the two in sync; change both + the JSON.
namespace eb::hig {

inline constexpr double kContrastFloorNormal = 4.5;   // WCAG 1.4.3 normal text
inline constexpr double kContrastFloorLarge  = 3.0;   // WCAG 1.4.3 large text
inline constexpr double kLargeFontPt         = 18.0;  // >= this pt -> large floor
inline constexpr double kLargeBoldFontPt     = 14.0;  // >= this pt AND bold -> large floor
inline constexpr double kHighSeverityDelta   = 1.0;   // ratio < floor - this -> 'high' (else 'medium')
inline constexpr int    kMinTargetPx         = 24;    // WCAG 2.5.8 pointer target floor
inline constexpr int    kOverlapNoisePx      = 2;     // report overlap only when depth > this (anti-alias noise)

} // namespace eb::hig
