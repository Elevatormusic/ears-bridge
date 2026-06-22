// Task 3: TWO grade rings (left mic + right mic), fed every block by processGradeDetectionStereo via the
// AudioEngine capture seam. These tests prove, through the PUBLIC per-ear API
// (gradingResponseReady(ear) + snapshotGradeRing(ear, dst, max)), that:
//   - ear 0 holds the LEFT channel's samples and ear 1 holds the RIGHT channel's — each its OWN data;
//   - the snapshot is OLDEST->NEWEST and survives a ring WRAP (the run far exceeds 28 s of ring);
//   - the two rings are INDEPENDENT (distinct L/R patterns never bleed across);
//   - the OLD single-ring API is a LEFT alias (copyGradingResponse == snapshotGradeRing(0,...),
//     gradingResponseReady() == gradingResponseReady(0)).
//
// The seam is driven exactly like a real run: a reference must be loaded for the rings to fill (the audio
// thread no-ops grading otherwise), and the consumer must DRAIN a ready snapshot before the capture thread
// re-publishes (the per-ear ready flag is the producer->consumer handoff). We feed a per-channel RAMP so a
// snapshot's tail (the newest samples) is a known monotonic sequence we can verify positionally.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "audio/AudioEngine.h"
#include <vector>
#include <cmath>

namespace {

// A small per-channel block: left[i] = lBase + i*0.001, right[i] = rBase + i*0.001 (distinct, finite,
// well below the activity floor's irrelevance — level does NOT gate the ring write).
static void fillRamp (std::vector<float>& l, std::vector<float>& r, float lBase, float rBase) {
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = lBase + (float) i * 0.001f;
        r[i] = rBase + (float) i * 0.001f;
    }
}

// Feed `blocks` blocks of distinct L/R ramps through the seam, then KEEP feeding identical "marker" blocks
// while draining until a fresh snapshot is published for BOTH ears, returning those snapshots. We drain each
// ear's ready flag as it appears so the capture thread re-publishes the most-recent window.
struct Snaps { std::vector<float> l, r; int gotL = 0, gotR = 0; };

static Snaps driveAndSnapshot (eb::AudioEngine& e, int N, double fs, int warmBlocks,
                               float lBase, float rBase) {
    std::vector<float> l ((size_t) N), r ((size_t) N), mono ((size_t) N, 0.0f);
    const int cap = (int) std::lround (fs * 28.0) + N;   // >= ring capacity, the snapshot fits
    Snaps s; s.l.assign ((size_t) cap, 0.0f); s.r.assign ((size_t) cap, 0.0f);

    // Warm-up feed: distinct L/R ramps. Drain any mid-run snapshot so the FINAL published window reflects
    // the most-recent ring state (the newest tail is the ramp we assert on).
    for (int b = 0; b < warmBlocks; ++b) {
        fillRamp (l, r, lBase, rBase);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), N);
        if (e.gradingResponseReady (0)) e.snapshotGradeRing (0, s.l.data(), cap);
        if (e.gradingResponseReady (1)) e.snapshotGradeRing (1, s.r.data(), cap);
    }
    // Keep feeding the SAME ramp; capture the next published snapshot for each ear (do NOT drain it away).
    for (int b = 0; b < 4000 && (s.gotL == 0 || s.gotR == 0); ++b) {
        fillRamp (l, r, lBase, rBase);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), N);
        if (s.gotL == 0 && e.gradingResponseReady (0)) s.gotL = e.snapshotGradeRing (0, s.l.data(), cap);
        if (s.gotR == 0 && e.gradingResponseReady (1)) s.gotR = e.snapshotGradeRing (1, s.r.data(), cap);
    }
    return s;
}

} // namespace

TEST_CASE("Grade rings: ear 0 holds the LEFT channel, ear 1 holds the RIGHT (oldest->newest), independent") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);
    e.setReferenceLoaded (true);   // the rings fill only when a reference is loaded

    // Distinct bases so a cross-channel leak would be obvious. ~200 blocks ~= 2.1 s — fits inside the 28 s
    // ring (no wrap yet), so the snapshot is the in-order prefix and its tail is the steady ramp.
    auto s = driveAndSnapshot (e, N, fs, /*warmBlocks*/ 200, /*lBase*/ 0.10f, /*rBase*/ 0.70f);

    REQUIRE (s.gotL > 0);
    REQUIRE (s.gotR > 0);

    // The NEWEST N samples (the snapshot tail) are the last block we fed: a per-channel ramp from base.
    // ear 0 must read the LEFT base (0.10), ear 1 the RIGHT base (0.70) — proving each ring carries its own
    // channel, and that they are independent (no bleed).
    for (int i = 0; i < N; ++i) {
        CHECK (s.l[s.gotL - N + i] == Catch::Approx (0.10f + (float) i * 0.001f).margin (1e-4f));
        CHECK (s.r[s.gotR - N + i] == Catch::Approx (0.70f + (float) i * 0.001f).margin (1e-4f));
    }
    // And the channels are genuinely DIFFERENT at the same position (independence, not a shared buffer).
    CHECK (std::abs (s.l[s.gotL - 1] - s.r[s.gotR - 1]) > 0.5f);
}

TEST_CASE("Grade rings: oldest->newest order survives a ring WRAP") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);
    e.setReferenceLoaded (true);

    // Feed > 28 s of audio so each ring WRAPS (28 s @ 48k / 512 ~= 2625 blocks). After wrap the snapshot is
    // the full ring; we only assert the NEWEST N samples (the steady ramp tail), which the wrap-aware
    // oldest->newest copy must place at the very END of the snapshot.
    const int wrapBlocks = (int) std::lround (fs * 30.0 / N);   // ~30 s -> guarantees a wrap
    auto s = driveAndSnapshot (e, N, fs, wrapBlocks, /*lBase*/ 0.20f, /*rBase*/ 0.55f);

    REQUIRE (s.gotL > 0);
    REQUIRE (s.gotR > 0);
    // After a full wrap the snapshot length is the whole ring (~28 s), so it is longer than the pre-wrap
    // prefix would be — a sanity check that we are truly past the wrap.
    CHECK (s.gotL >= (int) std::lround (fs * 27.0));
    CHECK (s.gotR >= (int) std::lround (fs * 27.0));

    // The tail (newest samples) is still the in-order ramp for each channel — order preserved across wrap.
    for (int i = 0; i < N; ++i) {
        CHECK (s.l[s.gotL - N + i] == Catch::Approx (0.20f + (float) i * 0.001f).margin (1e-4f));
        CHECK (s.r[s.gotR - N + i] == Catch::Approx (0.55f + (float) i * 0.001f).margin (1e-4f));
    }
}

TEST_CASE("Grade rings: the OLD single-ring API aliases the LEFT ear (ear 0)") {
    eb::AudioEngine e;
    const int    N  = 512;
    const double fs = 48000.0;
    e.prepareForTest (fs, N);
    e.setReferenceLoaded (true);

    std::vector<float> l ((size_t) N), r ((size_t) N), mono ((size_t) N, 0.0f);
    const int cap = (int) std::lround (fs * 28.0) + N;
    std::vector<float> via ((size_t) cap, 0.0f);

    // Feed distinct L/R ramps until a LEFT snapshot is ready, then read it through the OLD API. The OLD API
    // must (a) report ready == gradingResponseReady(0) and (b) return the LEFT channel's tail.
    for (int b = 0; b < 4000; ++b) {
        fillRamp (l, r, 0.30f, 0.90f);
        e.processCaptureBlockForTest (l.data(), r.data(), mono.data(), N);
        if (e.gradingResponseReady (0)) {
            CHECK (e.gradingResponseReady());   // old no-arg ready == ear-0 ready
            const int got = e.copyGradingResponse (via.data(), cap);   // old copy == snapshotGradeRing(0,...)
            REQUIRE (got > 0);
            for (int i = 0; i < N; ++i)         // it returned the LEFT ramp, not the right
                CHECK (via[got - N + i] == Catch::Approx (0.30f + (float) i * 0.001f).margin (1e-4f));
            return;   // proven
        }
    }
    FAIL ("no LEFT snapshot was ever published");
}
