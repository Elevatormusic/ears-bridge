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
