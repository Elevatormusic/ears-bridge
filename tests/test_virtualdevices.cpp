#include <catch2/catch_test_macros.hpp>
#include "sim/SimSignals.h"
#include "sim/VirtualDevices.h"
#include "audio/AudioEngine.h"
#include <cmath>

// The rig's two-clock streaming feeder (spec: 2026-07-02-synthetic-measurement-rig-design, unit 2).
// It drives the REAL engine callbacks; these tests pin the feeder's own mechanics (frame accounting,
// passthrough fidelity, honest behaviour under drift) before the orchestrator composes it.

static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("VirtualDevices: capture-frame accounting realises the requested bridge ppm") {
    // 60 virtual seconds at +500 ppm: total captured frames must exceed rendered frames by ~x1.0005.
    ebsim::StereoTimeline mic; mic.fs = 48000.0;
    const int renderFrames = 48000 * 60;
    mic.L.assign ((size_t) (renderFrames + 48000), 0.0f);   // headroom for the extra capture frames
    mic.R = mic.L;

    eb::AudioEngine e;
    e.setLeftCalFir (unitImpulse (8)); e.setRightCalFir (unitImpulse (8));   // BEFORE prepare (#12)
    e.prepareCallbacksForTest (48000.0, 480, 1 << 16);

    ebsim::FeederSpec f; f.blockFrames = 480; f.bridgePpm = +500.0;
    const auto out = ebsim::streamSession (e, mic, f);
    // Drift is realised as extra WHOLE capture blocks (production: callback RATE, never block size):
    // +500 ppm over ~60 s = 100 blocks/s * 60 s * 5e-4 = ~3 extra 480-frame blocks.
    const long long expected = (long long) std::llround ((double) out.captureFramesFed / 1.0005);
    INFO ("captured=" << out.captureFramesFed << " rendered=" << out.mono.size());
    CHECK (std::llabs ((long long) out.mono.size() - expected) <= 2 * 480);   // within a couple blocks
    CHECK (out.captureFramesFed > (long long) out.mono.size());               // capture ran FAST
}

TEST_CASE("VirtualDevices: equal clocks pass the mic signal through to the rendered cable output") {
    // A constant 0.3/0.1 pair through unity FIRs + Average combine -> the rendered mono settles at 0.2.
    ebsim::StereoTimeline mic; mic.fs = 48000.0;
    mic.L.assign (48000 * 2, 0.3f); mic.R.assign (48000 * 2, 0.1f);

    eb::AudioEngine e;
    e.setLeftCalFir (unitImpulse (8)); e.setRightCalFir (unitImpulse (8));
    e.setCombineMode (eb::CombineMode::Average);
    e.prepareCallbacksForTest (48000.0, 480, 1 << 15);

    const auto out = ebsim::streamSession (e, mic, {});
    REQUIRE (out.mono.size() > 48000);
    // The FIFO primes to half-full, so the tail of the render is well past any ramp/settle.
    const float tail = out.mono[out.mono.size() - 1];
    CHECK (std::abs (tail - 0.2f) < 5e-3f);
    CHECK (e.cleanCapture());
    CHECK (e.health().droppedFrames == 0);
}

TEST_CASE("VirtualDevices: heavy drift behaves HONESTLY end-to-end (the #40 invariant)") {
    // ±400 ppm (beyond crystal spec) for ~10 virtual seconds: either the bridge absorbs it cleanly
    // (fill bounded, no flags) or it degrades with an EXPLAINING invalidating flag - never a silent
    // corruption. This is the composed cleanCapture-derives-from-flags contract, live.
    for (double ppm : { +400.0, -400.0 }) {
        ebsim::StereoTimeline mic; mic.fs = 48000.0;
        mic.L.assign (48000 * 11, 0.05f); mic.R = mic.L;
        eb::AudioEngine e;
        e.setLeftCalFir (unitImpulse (8)); e.setRightCalFir (unitImpulse (8));
        e.prepareCallbacksForTest (48000.0, 480, 1 << 16);
        ebsim::FeederSpec f; f.bridgePpm = ppm;
        (void) ebsim::streamSession (e, mic, f);
        const auto h = e.health();
        INFO ("ppm=" << ppm << " clean=" << h.cleanCapture << " flags=" << (unsigned) h.flags);
        constexpr auto invalidating = eb::HealthFlag::Xrun | eb::HealthFlag::Dropout
            | eb::HealthFlag::ExcessDrift | eb::HealthFlag::FifoStarved | eb::HealthFlag::ClipConfirmed
            | eb::HealthFlag::NonFinite | eb::HealthFlag::SweepRetimed | eb::HealthFlag::FormatChanged;
        CHECK (h.cleanCapture == ! eb::any (h.flags & invalidating));
    }
}
