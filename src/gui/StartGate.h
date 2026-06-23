#pragma once

namespace eb {

// Pure Start-gate predicate (Task 3 / #3). Pulled out of MainComponent::updateStartGate
// so the truth table is unit-testable without a JUCE window.
//
//   haveDevs       -- an input AND an output device are selected
//   haveCals       -- a valid, fully-built, atomically-applied calibration generation
//                     is in effect (engine.calibrationApplied()) -- the correctness gate
//   wrongMode      -- a real EARS + cable with a non-AutoPerEar combine mode (D7/R17)
//   physicalOutput -- a real EARS into an output that is NOT a verified virtual sink (P1-09)
//   override       -- the user's opt-in "Allow non-Dirac use" advanced toggle
//   noCalsLoaded   -- NO cal file is loaded for EITHER ear. The engine then runs a neutral UNITY
//                     passthrough (clearLeftCalFir/clearRightCalFir restore unity), which is a VALID
//                     (if uncalibrated) state -- so Start is allowed, with a UI warning. This is
//                     DISTINCT from a cal that IS loaded but not yet validly applied (half-built /
//                     stale): that still blocks (haveCals=false AND noCalsLoaded=false) because a
//                     half-built generation would corrupt capture.
//
// The override relaxes ONLY the two POLICY gates (wrongMode, physicalOutput) so an advanced user can
// run a non-Dirac configuration. It must NEVER bypass haveDevs. The calibration requirement is
// satisfied by EITHER a valid applied cal (haveCals) OR no cal at all (noCalsLoaded = unity passthrough);
// only a loaded-but-unapplied cal blocks.
[[nodiscard]] inline bool startReady (bool haveDevs,
                                      bool haveCals,
                                      bool wrongMode,
                                      bool physicalOutput,
                                      bool override,
                                      bool noCalsLoaded = false) noexcept {
    return haveDevs
        && (haveCals || noCalsLoaded)
        && (override || ! wrongMode)
        && (override || ! physicalOutput);
}

} // namespace eb
