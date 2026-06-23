#pragma once
#include <juce_core/juce_core.h>   // juce::String used by sibling spine headers; Plan 4 precondition
namespace eb {

enum class EarsModel  { Unknown, Ears, EarsPro };
enum class Ear { Left = 0, Right = 1 };   // which earcup (0=L, 1=R); the single canonical home, shared by
                                          // LrVerify and the AutoPerEar SweepSchedule/ScheduledEarRouter
enum class EngineStatus { Stopped, Running, Error };

// D5 / R18: the measurement-session phase, scoping validity to Dirac's sweep window (left earcup ->
// gap -> right earcup) instead of the whole engine run. Driven by the input level threshold in
// MeasurementSession. NOT persisted.
enum class SessionPhase { Idle, Preflight, SweepActive, Complete, Invalid };

struct Levels {
    float inL = 0, inR = 0, outMono = 0;
    bool  clipL = false, clipR = false, clipOut = false;
};

// --- Plan 4 addition: sticky health condition flags ---
// OR-ed as conditions are detected during a run; cleared on start()/reset().
enum class HealthFlag : unsigned {
    None        = 0,
    Xrun        = 1u << 0,
    Dropout     = 1u << 1,
    ExcessDrift = 1u << 2,
    ClipInput   = 1u << 3,   // guidance: near full scale (-1 dBFS); does NOT invalidate
    LowLevel    = 1u << 4,
    ClipOutput  = 1u << 5,   // guidance: program output hit the clamp; does NOT invalidate
    FifoStarved = 1u << 6,
    ClipConfirmed = 1u << 7, // INVALIDATING: a consecutive near-rail run = confirmed digital clip
    NonFinite     = 1u << 8, // INVALIDATING: a NaN/Inf sample reached the path
    OsResampled   = 1u << 9, // guidance: OS SRC resampled the INPUT (D2) -> clip detection approximate; NOT invalidating
    SweepRetimed  = 1u << 10, // INVALIDATING: a forced mid-sweep SRC correction retimed the sweep
    FormatChanged = 1u << 11, // INVALIDATING: sample rate / bit depth / channels changed mid-run
    LowSnr        = 1u << 12, // guidance: the sweep was not cleanly above the room-noise floor (SNR); NOT invalidating (warn now, ratify later)
    // Reference-Based Measurement Monitor (Plan 5). BOTH are GUIDANCE — NOT in the invalidating mask
    // (they never flip cleanCapture). They are raised at the completed-sweep edge ONLY when a learned
    // reference is loaded; the deconvolution/grade is computed OFF the audio thread (see AudioEngine).
    RefMismatch   = 1u << 13, // guidance: the measurement did NOT match the learned reference -> re-learn (gate failed)
    RefLowQuality = 1u << 14  // guidance: matched, but the IR quality (IR-SNR/THD) was below the PROVISIONAL cutoff
};
constexpr HealthFlag operator| (HealthFlag a, HealthFlag b) noexcept {
    return static_cast<HealthFlag> (static_cast<unsigned> (a) | static_cast<unsigned> (b));
}
constexpr HealthFlag operator& (HealthFlag a, HealthFlag b) noexcept {
    return static_cast<HealthFlag> (static_cast<unsigned> (a) & static_cast<unsigned> (b));
}
constexpr bool any (HealthFlag f) noexcept { return static_cast<unsigned> (f) != 0u; }

struct Health {
    int       xruns = 0;
    long long droppedFrames = 0;
    double    fifoFill = 0.0;
    bool      cleanCapture = true;
    double    captureToRenderRatio = 1.0;
    HealthFlag flags = HealthFlag::None;   // Plan 4 addition: latched sticky condition flags
    SessionPhase session = SessionPhase::Idle;   // D5: measurement-session phase snapshot
};

// --- D2: raw-rail capture state, captured once per run by AudioEngine::start ---
// verified == true => the EARS input ran at the endpoint mix rate (no OS SRC on our stream), so the
// float samples faithfully represent the device's integer rails. verified == false => OS-resampled or
// the mix rate could not be resolved -> clip detection is approximate.
struct RawRailState {
    bool   verified      = false;
    double requestedRate = 0.0;
    double mixRate       = 0.0;
};

// --- Plan 4 addition: hardware DIP-switch analog-gain range per model ---
// (no software control; used for low-level guidance).
struct DipGainProfile {
    double minDb = 0.0;
    double maxDb = 36.0;   // original EARS: 0-36 dB; EARS Pro: 0-45 dB
    static DipGainProfile forModel (EarsModel m) noexcept {
        return (m == EarsModel::EarsPro) ? DipGainProfile { 0.0, 45.0 }
                                         : DipGainProfile { 0.0, 36.0 };
    }
};

} // namespace eb
