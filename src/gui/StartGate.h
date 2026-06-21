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
//
// The override relaxes ONLY the two POLICY gates (wrongMode, physicalOutput) so an advanced
// user can run a non-Dirac configuration. It must NEVER bypass haveDevs or haveCals: you
// cannot run the engine without devices, and an unapplied/invalid calibration would corrupt
// any capture regardless of intent.
[[nodiscard]] inline bool startReady (bool haveDevs,
                                      bool haveCals,
                                      bool wrongMode,
                                      bool physicalOutput,
                                      bool override) noexcept {
    return haveDevs && haveCals
        && (override || ! wrongMode)
        && (override || ! physicalOutput);
}

} // namespace eb
