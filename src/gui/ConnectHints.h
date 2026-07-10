#pragma once

// Production copy for the Connect warning surfaces, shared between MainComponent (the live
// setters) and the T10 no-scroll/displacement gates + EB_HIG_STATES harness (which must
// measure the REAL strings, not test stand-ins - copy drift would silently un-gate a longer
// warning). ASCII only (P4 owns the dash/ellipsis pass).
namespace eb::hints {
inline constexpr const char* kDiracHiFiCable =
    "The Hi-Fi Cable connects to Dirac but won't carry audio through it "
    "(no sample-rate converter). Use the standard CABLE Input instead.";
inline constexpr const char* kDiracStdCableExclusive =
    "Dirac records this standard cable in exclusive mode, which it can't do "
    "(error 600007). Click below to set Dirac to shared mode:";
inline constexpr const char* kDiracStdCableShared =
    "Dirac is set to shared mode, so this cable works. If Dirac is open, "
    "fully close and reopen it once.";
inline constexpr const char* kDiracOtherVirtual =
    "If Dirac can't open this cable (error 600007), set Dirac to shared mode "
    "or use the standard VB-CABLE.";
inline const juce::String kRateResample = "Not native" + kDash + "will be resampled.";
} // namespace eb::hints
