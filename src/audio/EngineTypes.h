#pragma once
#include <juce_core/juce_core.h>   // juce::String used by sibling spine headers; Plan 4 precondition
namespace eb {

enum class EarsModel  { Unknown, Ears, EarsPro };
enum class EngineStatus { Stopped, Running, Error };

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
    NonFinite     = 1u << 8  // INVALIDATING: a NaN/Inf sample reached the path
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
