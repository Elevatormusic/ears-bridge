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

inline constexpr double kMinFontPt = 7.5;   // T10 min-font floor, PROBE units (getHeightInPoints).
                                            // Calibrated so the 11px ramp floor clears it with
                                            // >=0.5 margin on both CI platforms (see the [T10]
                                            // min-font test); recalibrate (measured-0.5), never drop.

// P4 state-sweep checker constants - mirror native-review.mjs stateFindings (plugin v1.10.0).
// Parity-locked by the state-* fixtures in tests/fixtures/hig; change both + regenerate.
inline constexpr int    kSweepRgbTol          = 2;     // per-channel |d| <= 2 counts as identical
inline constexpr double kSweepAlphaTol        = 0.01;
inline constexpr double kSweepLowAlpha        = 0.05;  // below = not measurable (excluded, never compared)
inline constexpr double kContrastLouderMargin = 0.75;  // Apple's own sanctioned louder-deltas reach ~0.57
inline constexpr double kAlphaLouderMargin    = 0.05;
inline constexpr double kHueSwapDeg           = 60.0;  // beyond = a colour SWAP, not a dimming failure

} // namespace eb::hig
