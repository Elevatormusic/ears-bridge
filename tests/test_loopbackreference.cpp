// Task 3 (Reference-Based Measurement Monitor): the LoopbackReference PURE
// validation + metadata. The WASAPI loopback capture itself is hardware/manual
// (mirrors eb_diag's loopcap; on-device only) — what we unit-test here is the
// validation core (the gate that decides whether a captured reference is a
// clean, full, single-source sweep) and the metadata population.
//
// Tests (synthetic inputs, reusing a local Farina-ESS generator):
//   - a full clean sweep (~25 s @ 48k, sane level)            -> ok
//   - too short (15 s, the L-then-R sequence cannot fit)      -> reject (length)
//   - a clipping run inserted into a clean sweep              -> reject (clip/level)
//   - the sweep scaled to ~-40 dBFS (too quiet)              -> reject (level)
//   - sweep + a loud steady tone (two sources)               -> reject via self-test
//   - metadata: rate/length + a stable content hash + version field

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>

#include "audio/LoopbackReference.h"

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kFs = 48000.0;

// Exponential (log) sine sweep, Farina form (mirrors the Task-1 harness). A
// short raised-cosine fade removes edge clicks. Amplitude `amp` lets a test set
// the level (peak ≈ amp before the fade).
static std::vector<float> makeEss (int n, double fs, float amp = 0.5f,
                                   double f1 = 20.0, double f2 = 20000.0) {
    std::vector<float> x ((size_t) n, 0.0f);
    const double T  = (double) n / fs;
    const double w1 = 2.0 * kPi * f1;
    const double w2 = 2.0 * kPi * f2;
    const double K  = std::log (w2 / w1);
    const double A  = w1 * T / K;
    for (int i = 0; i < n; ++i) {
        const double t   = (double) i / fs;
        const double phi = A * (std::exp ((t / T) * K) - 1.0);
        x[(size_t) i] = amp * (float) std::sin (phi);
    }
    const int fade = std::min (n / 4, (int) std::lround (0.002 * fs));
    for (int i = 0; i < fade; ++i) {
        const float w = 0.5f * (1.0f - std::cos ((float) kPi * (float) i / (float) fade));
        x[(size_t) i]           *= w;
        x[(size_t) (n - 1 - i)] *= w;
    }
    return x;
}

// A full Dirac-style reference: the L-then-R sequence is ~25 s, so generate
// 26 s of sweep content here (two back-to-back ESS halves to mimic L then R).
static std::vector<float> makeFullReference (double seconds, float amp = 0.5f) {
    const int total = (int) std::lround (seconds * kFs);
    const int half  = total / 2;
    auto left  = makeEss (half, kFs, amp);
    auto right = makeEss (total - half, kFs, amp);
    std::vector<float> x;
    x.reserve ((size_t) total);
    x.insert (x.end(), left.begin(),  left.end());
    x.insert (x.end(), right.begin(), right.end());
    return x;
}

static float peakOf (const std::vector<float>& x) {
    float p = 0.0f;
    for (float v : x) p = std::max (p, std::abs (v));
    return p;
}

// ---------------------------------------------------------------------------
// ACCEPT — a full, clean, sane-level sweep
// ---------------------------------------------------------------------------
TEST_CASE("validateReferenceCapture ACCEPTS a full clean sweep at a sane level") {
    auto x = makeFullReference (26.0, 0.5f);        // 26 s, peak ~ -6 dBFS
    INFO ("peak=" << peakOf (x));
    auto r = eb::validateReferenceCapture (x.data(), (int) x.size(), kFs);
    INFO ("reason=" << r.reason);
    CHECK (r.ok);
    CHECK (r.reason.isEmpty());
}

// ---------------------------------------------------------------------------
// REJECT — too short (the L-then-R sequence cannot fit in 15 s)
// ---------------------------------------------------------------------------
TEST_CASE("validateReferenceCapture REJECTS a capture that is too short") {
    auto x = makeFullReference (15.0, 0.5f);        // only 15 s -> missing the full sequence
    auto r = eb::validateReferenceCapture (x.data(), (int) x.size(), kFs);
    INFO ("reason=" << r.reason);
    CHECK_FALSE (r.ok);
    CHECK (r.reason.containsIgnoreCase ("short"));
}

// ---------------------------------------------------------------------------
// REJECT — a clipping run (consecutive full-scale samples)
// ---------------------------------------------------------------------------
TEST_CASE("validateReferenceCapture REJECTS a capture with a clipping run") {
    auto x = makeFullReference (26.0, 0.5f);
    // Insert a run of full-scale samples mid-capture (a clipped overload).
    const int start = (int) x.size() / 2;
    for (int i = 0; i < 256; ++i) x[(size_t) (start + i)] = (i & 1) ? 1.0f : -1.0f;
    auto r = eb::validateReferenceCapture (x.data(), (int) x.size(), kFs);
    INFO ("reason=" << r.reason);
    CHECK_FALSE (r.ok);
    const bool mentionsClipOrLevel = r.reason.containsIgnoreCase ("clip")
                                  || r.reason.containsIgnoreCase ("level");
    CHECK (mentionsClipOrLevel);
}

// ---------------------------------------------------------------------------
// REJECT — too quiet (~-40 dBFS)
// ---------------------------------------------------------------------------
TEST_CASE("validateReferenceCapture REJECTS a capture that is too quiet") {
    auto x = makeFullReference (26.0, 0.01f);       // peak ~ -40 dBFS
    INFO ("peak=" << peakOf (x));
    auto r = eb::validateReferenceCapture (x.data(), (int) x.size(), kFs);
    INFO ("reason=" << r.reason);
    CHECK_FALSE (r.ok);
    const bool mentionsLevel = r.reason.containsIgnoreCase ("quiet")
                            || r.reason.containsIgnoreCase ("level")
                            || r.reason.containsIgnoreCase ("low");
    CHECK (mentionsLevel);
}

// ---------------------------------------------------------------------------
// REJECT — contamination (sweep + a loud steady tone = two sources) via self-test
// ---------------------------------------------------------------------------
TEST_CASE("validateReferenceCapture REJECTS a contaminated capture (two sources)") {
    auto x = makeFullReference (26.0, 0.4f);
    // Add a loud steady 1 kHz tone on top of the sweep — a second source. This
    // breaks the clean-single-sweep self-test (the autocorrelation main lobe is
    // no longer dominant once a strong periodic component is mixed in).
    const double f = 1000.0;
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += 0.4f * (float) std::sin (2.0 * kPi * f * (double) i / kFs);
    // Normalize into the SANE level band (peak ~ -6 dBFS) so the capture is NOT
    // rejected on level/clip — the self-test must be the rule that fires.
    const float pk = peakOf (x);
    if (pk > 0.0f) for (auto& v : x) v *= (0.5f / pk);
    auto r = eb::validateReferenceCapture (x.data(), (int) x.size(), kFs);
    INFO ("reason=" << r.reason);
    CHECK_FALSE (r.ok);
    const bool mentionsSelfTest = r.reason.containsIgnoreCase ("single")
                               || r.reason.containsIgnoreCase ("clean")
                               || r.reason.containsIgnoreCase ("contaminat")
                               || r.reason.containsIgnoreCase ("sweep")
                               || r.reason.containsIgnoreCase ("source");
    CHECK (mentionsSelfTest);
}

// ---------------------------------------------------------------------------
// REJECT — noise only (no sweep) also fails the self-test
// ---------------------------------------------------------------------------
TEST_CASE("validateReferenceCapture REJECTS noise-only (no sweep)") {
    const int n = (int) std::lround (26.0 * kFs);
    std::vector<float> x ((size_t) n, 0.0f);
    std::mt19937 rng (42u);
    std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
    for (auto& v : x) v = dist (rng);
    auto r = eb::validateReferenceCapture (x.data(), (int) x.size(), kFs);
    INFO ("reason=" << r.reason);
    CHECK_FALSE (r.ok);
}

// ---------------------------------------------------------------------------
// METADATA — rate/length + a stable content hash + version field present
// ---------------------------------------------------------------------------
TEST_CASE("makeReferenceMetadata populates rate, length, and a stable content hash") {
    auto x = makeFullReference (26.0, 0.5f);
    auto m = eb::makeReferenceMetadata (x.data(), (int) x.size(), kFs);
    CHECK (m.rate == kFs);
    CHECK (m.lengthSamples == (int) x.size());
    CHECK (m.contentHash.isNotEmpty());

    // Same bytes -> same hash (stable / deterministic).
    auto m2 = eb::makeReferenceMetadata (x.data(), (int) x.size(), kFs);
    CHECK (m.contentHash == m2.contentHash);

    // A changed sample -> a different hash (the digest actually covers the data).
    auto y = x;
    y[100] += 0.25f;
    auto m3 = eb::makeReferenceMetadata (y.data(), (int) y.size(), kFs);
    CHECK (m.contentHash != m3.contentHash);
    // diracVersion is best-effort (empty when Dirac isn't installed) — just a string.
}

// ---------------------------------------------------------------------------
// Dirac deviceType classifier (Task 4) — pure, read-only mode detection
// ---------------------------------------------------------------------------
TEST_CASE("diracDeviceTypeIsWindowsAudio recognises WASAPI shared, rejects ASIO/unknown") {
    CHECK (eb::diracDeviceTypeIsWindowsAudio ("Windows Audio"));
    CHECK (eb::diracDeviceTypeIsWindowsAudio ("windows audio"));        // case-insensitive
    CHECK (eb::diracDeviceTypeIsWindowsAudio ("Windows Audio (Shared)"));
    CHECK (eb::diracDeviceTypeIsWindowsAudio ("WASAPI"));
    CHECK_FALSE (eb::diracDeviceTypeIsWindowsAudio ("ASIO"));            // loopback is silent in ASIO
    CHECK_FALSE (eb::diracDeviceTypeIsWindowsAudio ("ASIO4ALL v2"));
    CHECK_FALSE (eb::diracDeviceTypeIsWindowsAudio (""));               // unknown -> NOT confirmed (cautious)
}

// ---------------------------------------------------------------------------
// CANCEL TOKEN — a pre-set cancel returns PROMPTLY with a cancelled (not failed)
// result, never running the full ~26 s capture. The live WASAPI loop is on-device
// (Windows-only); this exercises the cooperative-cancel contract, which is honoured
// up front on EVERY platform (the cancel is checked before any device work and in
// the stub), so it's headless-safe — no real endpoint required.
// ---------------------------------------------------------------------------
TEST_CASE("captureLoopback returns promptly with a cancelled result when the cancel token is pre-set") {
    std::atomic<bool> cancel { true };                  // already requested before we even call
    const auto t0 = std::chrono::steady_clock::now();
    // A 26 s nominal capture: if cancel were ignored this would block far past the budget below.
    auto cap = eb::captureLoopback ("CABLE", 26.0, 48000.0, &cancel);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                               std::chrono::steady_clock::now() - t0).count();

    CHECK_FALSE (cap.ok);                               // a cancel is not a success
    CHECK (cap.cancelled);                              // ...and it's flagged as a user-cancel, not a failure
    CHECK (cap.reason == juce::String ("cancelled"));   // neutral reason the GUI can recognise
    CHECK (elapsedMs < 2000);                           // promptly — nowhere near the 26 s capture window
    INFO ("elapsedMs=" << elapsedMs);
}
