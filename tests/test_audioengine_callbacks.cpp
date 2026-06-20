#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/AudioEngine.h"
#include <vector>
#include <cmath>
#include <limits>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

using Catch::Matchers::WithinAbs;

// A unit-impulse FIR makes the per-ear convolution a passthrough so the test can reason about
// the forwarded mono numerically. Settles the async IR load by spinning the capture callback.
static juce::AudioBuffer<float> unitImpulse (int taps) {
    juce::AudioBuffer<float> b (1, taps); b.clear(); b.setSample (0, 0, 1.0f); return b;
}

TEST_CASE("AudioEngine callbacks: capture forwards a clean block; render pulls it back") {
    eb::AudioEngine e;
    const int N = 64, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);
    e.setLeftCalFir  (unitImpulse (8));
    e.setRightCalFir (unitImpulse (8));
    e.setCombineMode (eb::CombineMode::Average);

    std::vector<float> inL (N, 0.5f), inR (N, 0.3f);
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    // Spin the capture callback (async Convolution IR load + gain ramp settle) then pull.
    bool settled = false;
    for (int rep = 0; rep < 3000 && ! settled; ++rep) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
        if (std::abs (outL[N-1] - 0.4f) < 2e-3f) settled = true;   // (0.5+0.3)/2 through the FIFO
        else juce::Thread::sleep (1);
    }
    REQUIRE (settled);
    CHECK (e.cleanCapture());                          // a clean run stays valid
    CHECK (e.health().droppedFrames == 0);
    CHECK_THAT (outR[N-1], WithinAbs (outL[N-1], 1e-6));   // render duplicates mono to L == R
}

TEST_CASE("AudioEngine callbacks: a confirmed rail-run through the real capture callback invalidates") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    // 3 consecutive samples at/above kRailCeiling on L == kRailRunMin -> ClipConfirmed (invalidating).
    std::vector<float> inL { 0.2f, 1.0f, 1.0f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f };
    std::vector<float> inR (inL.size(), 0.0f);
    e.driveCaptureCallback (inL.data(), inR.data(), (int) inL.size());

    CHECK (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: an isolated single full-scale sample does NOT confirm a clip") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    // A clean full-scale sine touches the rail for ONE sample -> no run -> not a confirmed clip.
    std::vector<float> inL { 0.2f, 1.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::vector<float> inR (inL.size(), 0.0f);
    e.driveCaptureCallback (inL.data(), inR.data(), (int) inL.size());

    CHECK_FALSE (eb::any (e.health().flags & eb::HealthFlag::ClipConfirmed));
    CHECK (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a NaN in the raw input invalidates and is not forwarded") {
    eb::AudioEngine e;
    const int N = 8;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    std::vector<float> inL (N, 0.1f), inR (N, 0.1f);
    inL[3] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    e.driveCaptureCallback (inL.data(), inR.data(), N);
    e.driveRenderCallback  (outL.data(), outR.data(), N);

    CHECK (eb::any (e.health().flags & eb::HealthFlag::NonFinite));
    CHECK_FALSE (e.cleanCapture());
    for (int i = 0; i < N; ++i) CHECK (std::isfinite (outL[i]));   // no NaN reaches the cable
}

TEST_CASE("AudioEngine callbacks: over-feeding capture overruns the FIFO and invalidates") {
    eb::AudioEngine e;
    const int N = 1024, cap = 1024;          // each capture block is the whole FIFO; render never drains here
    e.prepareCallbacksForTest (48000.0, N, cap);

    std::vector<float> inL (N, 0.2f), inR (N, 0.2f);
    for (int b = 0; b < 8; ++b) e.driveCaptureCallback (inL.data(), inR.data(), N);   // push, never pull

    CHECK (e.health().droppedFrames > 0);                 // bridge overrun frames surfaced into Health
    CHECK (eb::any (e.health().flags & eb::HealthFlag::Dropout));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a starved render underruns and invalidates") {
    eb::AudioEngine e;
    const int N = 512, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);

    // Drain the primed FIFO without ever feeding capture: ~cap/2 primed / N pulls then it starves.
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);
    bool starved = false;
    for (int b = 0; b < 64 && ! starved; ++b) {
        e.driveRenderCallback (outL.data(), outR.data(), N);
        if (eb::any (e.health().flags & eb::HealthFlag::FifoStarved)) starved = true;
    }
    CHECK (starved);
    CHECK (eb::any (e.health().flags & eb::HealthFlag::Dropout));
    CHECK_FALSE (e.cleanCapture());
}

TEST_CASE("AudioEngine callbacks: a single-channel capture block reports an Xrun") {
    eb::AudioEngine e;
    const int N = 64;
    e.prepareCallbacksForTest (48000.0, N, 8192);
    std::vector<float> in0 (N, 0.1f);

    e.driveCaptureCallbackMono (in0.data(), N);     // numIn == 1 -> the numIn<2 guard fires

    CHECK (eb::any (e.health().flags & eb::HealthFlag::Xrun));
    CHECK_FALSE (e.cleanCapture());
    CHECK (e.health().xruns >= 1);
}

// --- Allocation counter: armed only inside the measured region, so it never perturbs other TUs. ---
namespace {
    std::atomic<bool>     g_countAllocs { false };
    std::atomic<long long> g_allocCount { 0 };
}
void* operator new (std::size_t sz) {
    if (g_countAllocs.load (std::memory_order_relaxed)) g_allocCount.fetch_add (1, std::memory_order_relaxed);
    if (auto* p = std::malloc (sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

TEST_CASE("AudioEngine callbacks: steady-state capture+render are allocation-free") {
    eb::AudioEngine e;
    const int N = 256, cap = 8192;
    e.prepareCallbacksForTest (48000.0, N, cap);
    e.setLeftCalFir  (unitImpulse (8));
    e.setRightCalFir (unitImpulse (8));
    e.setCombineMode (eb::CombineMode::AutoPerEar);

    std::vector<float> inL (N, 0.25f), inR (N, 0.25f);
    std::vector<float> outL (N, 0.0f), outR (N, 0.0f);

    // Warm-up OUTSIDE the measured region: settle the async Convolution IR load / gain ramp.
    for (int b = 0; b < 256; ++b) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
        juce::Thread::sleep (1);
    }

    g_allocCount.store (0);
    g_countAllocs.store (true);
    for (int b = 0; b < 500; ++b) {
        e.driveCaptureCallback (inL.data(), inR.data(), N);
        e.driveRenderCallback  (outL.data(), outR.data(), N);
    }
    g_countAllocs.store (false);

    INFO ("alloc count in 500 capture+render blocks = " << g_allocCount.load());
    CHECK (g_allocCount.load() == 0);
}
