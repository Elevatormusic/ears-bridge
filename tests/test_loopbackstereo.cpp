// Per-Ear Per-Channel Grading — Task 1: captureLoopbackStereo's PURE decode core.
//
// The WASAPI loopback capture itself is hardware/manual (Windows-only, on-device),
// so what we unit-test here is the pure per-channel decode primitive that backs it:
//   eb::detail::decodePacketPerChannel(data, frames, channels, bits, isFloat, outL, outR)
// which appends channel 0 to outL and channel 1 to outR WITHOUT downmixing — the no-mono-fold
// that keeps Dirac's hard-panned L/R sweep separation intact for per-ear grading.
//
// Cases:
//   - 32f interleaved 2ch: L = ramp, R = zeros -> samplesL == ramp, samplesR == 0
//   - 16-bit 2ch: a known L/R pair -> the right normalized floats per channel
//   - 24-bit 2ch: sign-extension correct per channel
//   - mono (1ch): the single channel duplicates into both L and R

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio/LoopbackReference.h"

// ---------------------------------------------------------------------------
// 32-bit float, 2ch: L = ramp, R = zeros.
// ---------------------------------------------------------------------------
TEST_CASE("decodePacketPerChannel: 32f stereo splits ch0->L (ramp) and ch1->R (zeros), no downmix") {
    constexpr unsigned int frames = 8;
    std::vector<float> interleaved ((size_t) frames * 2, 0.0f);
    for (unsigned int f = 0; f < frames; ++f) {
        interleaved[(size_t) f * 2 + 0] = (float) f * 0.1f;   // L ramp: 0, 0.1, 0.2, ...
        interleaved[(size_t) f * 2 + 1] = 0.0f;               // R silent
    }

    std::vector<float> L, R;
    eb::detail::decodePacketPerChannel (reinterpret_cast<const unsigned char*> (interleaved.data()),
                                        frames, /*channels*/ 2, /*bits*/ 32, /*isFloat*/ true, L, R);

    REQUIRE (L.size() == frames);
    REQUIRE (R.size() == frames);
    for (unsigned int f = 0; f < frames; ++f) {
        CHECK (L[f] == (float) f * 0.1f);   // L is the ramp, exactly (no averaging with R)
        CHECK (R[f] == 0.0f);               // R stays silent — proves no downmix bleed
    }
}

// ---------------------------------------------------------------------------
// 16-bit PCM, 2ch: a known L/R pair decodes to the right normalized floats.
// ---------------------------------------------------------------------------
TEST_CASE("decodePacketPerChannel: 16-bit stereo decodes each channel to its normalized float") {
    // L = +16384 (= +0.5 at /32768), R = -16384 (= -0.5). Distinct per channel.
    const int16_t Lval = 16384, Rval = -16384;
    constexpr unsigned int frames = 4;
    std::vector<int16_t> interleaved ((size_t) frames * 2);
    for (unsigned int f = 0; f < frames; ++f) {
        interleaved[(size_t) f * 2 + 0] = Lval;
        interleaved[(size_t) f * 2 + 1] = Rval;
    }

    std::vector<float> L, R;
    eb::detail::decodePacketPerChannel (reinterpret_cast<const unsigned char*> (interleaved.data()),
                                        frames, 2, /*bits*/ 16, /*isFloat*/ false, L, R);

    REQUIRE (L.size() == frames);
    REQUIRE (R.size() == frames);
    for (unsigned int f = 0; f < frames; ++f) {
        CHECK (L[f] == 16384.0f / 32768.0f);    // +0.5
        CHECK (R[f] == -16384.0f / 32768.0f);   // -0.5
    }
}

// ---------------------------------------------------------------------------
// 24-bit packed PCM, 2ch: sign-extension correct PER CHANNEL.
// ---------------------------------------------------------------------------
TEST_CASE("decodePacketPerChannel: 24-bit stereo sign-extends each channel correctly") {
    // L = -1 (0xFFFFFF, the all-ones negative) -> should decode to -1/8388608.
    // R = +8388607 (0x7FFFFF, the max positive)  -> should decode to +8388607/8388608.
    auto pack24 = [] (std::vector<unsigned char>& buf, int32_t v) {
        buf.push_back ((unsigned char) (v & 0xFF));
        buf.push_back ((unsigned char) ((v >> 8) & 0xFF));
        buf.push_back ((unsigned char) ((v >> 16) & 0xFF));
    };
    constexpr unsigned int frames = 3;
    std::vector<unsigned char> bytes;
    for (unsigned int f = 0; f < frames; ++f) {
        pack24 (bytes, -1);          // L = -1
        pack24 (bytes, 8388607);     // R = +max
    }

    std::vector<float> L, R;
    eb::detail::decodePacketPerChannel (bytes.data(), frames, 2, /*bits*/ 24, /*isFloat*/ false, L, R);

    REQUIRE (L.size() == frames);
    REQUIRE (R.size() == frames);
    for (unsigned int f = 0; f < frames; ++f) {
        CHECK (L[f] == -1.0f / 8388608.0f);          // sign-extended negative, not a large positive
        CHECK (R[f] == 8388607.0f / 8388608.0f);     // max positive ~ +1.0
    }
}

// ---------------------------------------------------------------------------
// Mono (1ch): the single channel duplicates into BOTH L and R.
// ---------------------------------------------------------------------------
TEST_CASE("decodePacketPerChannel: mono endpoint duplicates the single channel into L and R") {
    constexpr unsigned int frames = 6;
    std::vector<float> mono ((size_t) frames);
    for (unsigned int f = 0; f < frames; ++f) mono[f] = -0.25f + (float) f * 0.1f;

    std::vector<float> L, R;
    eb::detail::decodePacketPerChannel (reinterpret_cast<const unsigned char*> (mono.data()),
                                        frames, /*channels*/ 1, /*bits*/ 32, /*isFloat*/ true, L, R);

    REQUIRE (L.size() == frames);
    REQUIRE (R.size() == frames);
    for (unsigned int f = 0; f < frames; ++f) {
        CHECK (L[f] == mono[f]);
        CHECK (R[f] == mono[f]);   // mono -> R is a copy of L (both ears see the one stream)
    }
}

// ---------------------------------------------------------------------------
// APPEND semantics: two packets accumulate (the capture loop calls this per packet).
// ---------------------------------------------------------------------------
TEST_CASE("decodePacketPerChannel: successive packets APPEND (oldest-to-newest) into the vectors") {
    std::vector<float> L, R;
    const float p1[] = { 0.1f, 0.9f };   // 1 frame, 2ch: L=0.1, R=0.9
    const float p2[] = { 0.2f, 0.8f };   // 1 frame, 2ch: L=0.2, R=0.8
    eb::detail::decodePacketPerChannel (reinterpret_cast<const unsigned char*> (p1), 1, 2, 32, true, L, R);
    eb::detail::decodePacketPerChannel (reinterpret_cast<const unsigned char*> (p2), 1, 2, 32, true, L, R);

    REQUIRE (L.size() == 2);
    REQUIRE (R.size() == 2);
    CHECK (L[0] == 0.1f); CHECK (L[1] == 0.2f);
    CHECK (R[0] == 0.9f); CHECK (R[1] == 0.8f);
}

// ---------------------------------------------------------------------------
// Task 2 — the reload decision (classifyReferenceFiles). This is the PURE core behind
// MainComponent::loadStoredReference: both per-channel files present -> loadable for per-ear
// grading; only the OLD mono reference (or just one channel) -> force a RE-LEARN, never a false
// grade; nothing present -> idle. (The file I/O itself is on-device, but the decision is testable.)
// ---------------------------------------------------------------------------
TEST_CASE("classifyReferenceFiles: both channels present -> loadable per-channel") {
    CHECK (eb::classifyReferenceFiles (/*L*/ true, /*R*/ true, /*mono*/ false) == eb::ReferenceFilesState::PerChannel);
    // A leftover old mono file alongside a valid pair is irrelevant — the pair wins.
    CHECK (eb::classifyReferenceFiles (true, true, /*mono*/ true) == eb::ReferenceFilesState::PerChannel);
}

TEST_CASE("classifyReferenceFiles: only the old mono reference -> re-learn (not loadable, no false grade)") {
    CHECK (eb::classifyReferenceFiles (/*L*/ false, /*R*/ false, /*mono*/ true) == eb::ReferenceFilesState::ReLearnNeeded);
}

TEST_CASE("classifyReferenceFiles: a half-written pair (one channel missing) -> re-learn") {
    CHECK (eb::classifyReferenceFiles (/*L*/ true,  /*R*/ false, /*mono*/ false) == eb::ReferenceFilesState::ReLearnNeeded);
    CHECK (eb::classifyReferenceFiles (/*L*/ false, /*R*/ true,  /*mono*/ false) == eb::ReferenceFilesState::ReLearnNeeded);
}

TEST_CASE("classifyReferenceFiles: nothing on disk -> none (stay idle)") {
    CHECK (eb::classifyReferenceFiles (false, false, false) == eb::ReferenceFilesState::None);
}

// ---------------------------------------------------------------------------
// findActiveSpan — trim a hard-panned per-channel reference to JUST its own sweep,
// dropping the silent half (where the OTHER ear sweeps). The on-device reality is
// ref_L = [L-sweep ~10 s][silence ~11 s] and ref_R = [silence ~11 s][R-sweep ~10 s];
// grading against the FULL reference divides the silent half by ~zero and drags IR-SNR
// negative, so we trim to the sweep before validate/store. Block-RMS + a -40 dB threshold
// + a ~50 ms margin; valid=false on an all-silent channel or an implausibly short (<2 s) span.
// ---------------------------------------------------------------------------
namespace {

constexpr double kRate = 48000.0;

// A flat-envelope band of pseudo-random "sweep" energy at `level` for `seconds`.
// Flat-ish on purpose: it stresses that the WHOLE sweep clears the -40 dB threshold
// (a real ESS has a near-constant envelope), not just a transient peak.
void appendSweep (std::vector<float>& buf, double seconds, float level) {
    const int n = (int) std::llround (kRate * seconds);
    unsigned int s = 0x12345u;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;                       // LCG
        const float r = ((float) (s >> 9) / (float) (1u << 23)) * 2.0f - 1.0f;   // [-1, 1)
        buf.push_back (r * level);
    }
}

// A silent half at the hard-panned floor (~-120 dB: where the OTHER ear sweeps).
void appendSilence (std::vector<float>& buf, double seconds, float level = 1.0e-6f) {
    const int n = (int) std::llround (kRate * seconds);
    for (int i = 0; i < n; ++i) buf.push_back ((i & 1) ? level : -level);   // ~-120 dBFS
}

} // namespace

TEST_CASE("findActiveSpan: [sweep 10s][silence 11s] -> span starts at 0, ends ~near the sweep end, valid") {
    std::vector<float> ch;
    appendSweep (ch, 10.0, 0.2f);     // L-sweep first
    appendSilence (ch, 11.0);         // then the silent half (the R-ear sweep)

    const auto span = eb::findActiveSpan (ch.data(), (int) ch.size(), kRate);
    REQUIRE (span.valid);
    // Starts at the very beginning (margin clamps to 0), ends just past the 10 s sweep (within a margin).
    CHECK (span.first == 0);
    const double endS = (double) span.last / kRate;
    CHECK (endS > 9.9);
    CHECK (endS < 10.2);              // the 11 s of silence is dropped, not included
    const double lenS = (double) (span.last - span.first) / kRate;
    CHECK (lenS > 9.0);              // ~10 s of sweep kept, not the full 21 s
    CHECK (lenS < 11.0);
}

TEST_CASE("findActiveSpan: [silence 11s][sweep 10s] -> span starts ~near 11s, ends near the buffer end, valid") {
    std::vector<float> ch;
    appendSilence (ch, 11.0);         // the silent half first (the L-ear sweep)
    appendSweep (ch, 10.0, 0.2f);     // then the R-sweep

    const auto span = eb::findActiveSpan (ch.data(), (int) ch.size(), kRate);
    REQUIRE (span.valid);
    const double startS = (double) span.first / kRate;
    CHECK (startS > 10.8);            // the leading 11 s of silence is dropped (within a margin)
    CHECK (startS < 11.1);
    CHECK (span.last == (int) ch.size());   // ends at the buffer end (margin clamps to n)
    const double lenS = (double) (span.last - span.first) / kRate;
    CHECK (lenS > 9.0);
    CHECK (lenS < 11.0);
}

TEST_CASE("findActiveSpan: all-silence -> valid=false (no sweep to trim to)") {
    std::vector<float> ch;
    appendSilence (ch, 21.0);
    const auto span = eb::findActiveSpan (ch.data(), (int) ch.size(), kRate);
    CHECK_FALSE (span.valid);
}

TEST_CASE("findActiveSpan: a full-length sweep (no silence) -> span is ~the whole buffer, valid") {
    std::vector<float> ch;
    appendSweep (ch, 12.0, 0.2f);     // entirely sweep, no silent half
    const auto span = eb::findActiveSpan (ch.data(), (int) ch.size(), kRate);
    REQUIRE (span.valid);
    CHECK (span.first == 0);
    CHECK (span.last == (int) ch.size());   // both margins clamp to the buffer ends
    const double lenS = (double) (span.last - span.first) / kRate;
    CHECK (lenS > 11.9);
}

TEST_CASE("findActiveSpan: a too-short blip (<2 s of sweep) -> valid=false") {
    std::vector<float> ch;
    appendSilence (ch, 5.0);
    appendSweep (ch, 1.0, 0.2f);      // only ~1 s of sweep — below the 2 s plausibility floor
    appendSilence (ch, 5.0);
    const auto span = eb::findActiveSpan (ch.data(), (int) ch.size(), kRate);
    CHECK_FALSE (span.valid);
}

TEST_CASE("findActiveSpan: null / empty / zero-rate -> valid=false (no data)") {
    std::vector<float> ch;
    appendSweep (ch, 10.0, 0.2f);
    CHECK_FALSE (eb::findActiveSpan (nullptr, 100, kRate).valid);
    CHECK_FALSE (eb::findActiveSpan (ch.data(), 0, kRate).valid);
    CHECK_FALSE (eb::findActiveSpan (ch.data(), (int) ch.size(), 0.0).valid);
}
