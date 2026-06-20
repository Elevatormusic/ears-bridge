# Diagnostic Event-Capture Buffer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a pre-allocated lock-free circular buffer (`DiagEventBuffer`) that retains raw EARS audio samples (both channels, pre-FIR) in a ring large enough to hold audio before, during, and after a clip or corruption event. When `HealthMonitor::analyzeInputBlock` raises `ClipConfirmed` or `NonFinite`, the audio thread atomically marks a snapshot point; a non-RT consumer (the GUI timer at 30 Hz) serializes the ring into a `DiagEventSnapshot` struct (peaks, rail positions, timestamps, device format, channel identity) for after-the-fact diagnosis. The audio thread NEVER touches the heap, locks, disk, or the OS after `prepare()`. The non-RT side NEVER accesses the ring directly while the audio thread is writing.

**Architecture:** One new class `eb::DiagEventBuffer` (`src/audio/DiagEventBuffer.{h,cpp}`). The ring is a pair of pre-allocated `std::array`-of-float channels (L and R raw). A write cursor advances via `std::atomic<int>`. A separate `std::atomic<int>` `eventCursor_` is set to the current write position when an event fires; `std::atomic<bool>` `pendingEvent_` gates the GUI poll. `AudioEngine` owns a `DiagEventBuffer` instance; `CaptureCallback` writes into it every block (unconditionally — it is always rolling); `analyzeInputBlock` fires a signal via a new inline method `DiagEventBuffer::markEvent(const char* reason)` when a clip/corruption is confirmed. The GUI timer calls `AudioEngine::consumeDiagSnapshot()` which copies the ring into a `DiagEventSnapshot` and clears `pendingEvent_`. No lock — the ring copy is a memcpy of pre-allocated memory; because the write cursor is atomic and the ring is power-of-two sized, the read window is always coherent within the SPSC constraint.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`. No new libraries.

---

## Global Constraints

- **Build (tests):** `./tools/dev.cmd cmake --build build --target eb_tests` — run `tools/dev.cmd` **directly from Bash, never `cmd /c`** (loses MSVC env), never bare cmake.
- **Run a test:** `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (98 tests today, must stay green).
- **RT-safety:** audio-callback-reachable code has **NO** heap alloc, lock, syscall, logging, or exception — plain locals + `std::atomic` only.
- **HealthFlag** (`src/audio/EngineTypes.h`) is NOT persisted — append enum values freely. `CombineMode` IS persisted — never touch it here.
- No real EARS serial in any file (tests use synthetic buffers only).
- **Commit trailer (every commit):**
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```
- Work on a feature branch, not `main`.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `src/audio/DiagEventBuffer.h` | **Create** | `DiagEventBuffer` class + `DiagEventSnapshot` struct (ring sizing, atomic cursor/event, RT-safe write + markEvent, non-RT snapshot) |
| `src/audio/DiagEventBuffer.cpp` | **Create** | `prepare`, `pushBlock`, `markEvent`, `consumeSnapshot` implementations |
| `src/audio/AudioEngine.h` | Modify | Add `DiagEventBuffer diagBuf_` member; declare `consumeDiagSnapshot()` |
| `src/audio/AudioEngine.cpp` | Modify | `CaptureCallback` calls `diagBuf_.pushBlock(l, r, n)` then `diagBuf_.markEvent(...)` on confirmed clip/nonfinite; `AudioEngine::consumeDiagSnapshot()` delegates to `diagBuf_` |
| `tests/test_diageventbuffer.cpp` | **Create** | Catch2 tests for ring sizing, RT-safe write, snapshot-on-event, no snapshot without event, ring wrap |
| `tests/CMakeLists.txt` | Modify | Add `test_diageventbuffer.cpp` to `eb_tests` source list |

---

## Task 1: `DiagEventSnapshot` struct + `DiagEventBuffer` skeleton

**Files:**
- Create `src/audio/DiagEventBuffer.h`
- Create `src/audio/DiagEventBuffer.cpp`
- Create `tests/test_diageventbuffer.cpp`
- Modify `tests/CMakeLists.txt`

**Interfaces produced:**

```cpp
// src/audio/DiagEventBuffer.h
namespace eb {

// Device format snapshot taken at markEvent() time (all fields atomic-read on the audio thread;
// written only in prepare(), which happens before the stream starts, so they are stable during audio).
struct DiagDeviceFormat {
    double sampleRate    = 0.0;
    int    bitDepth      = 0;
    int    numChannels   = 0;   // always 2 for a stereo EARS capture
};

// Immutable snapshot handed to the non-RT consumer. Contains the full ring contents
// (pre + during + post event) plus per-channel statistics.
struct DiagEventSnapshot {
    static constexpr int kMaxRingSamples = 8192;  // 2 * 4096 at 48 kHz ≈ 170 ms; two channels
    // Raw audio: L[0..ringUsed-1] and R[0..ringUsed-1] in chronological order (oldest first).
    float           samplesL[kMaxRingSamples] = {};
    float           samplesR[kMaxRingSamples] = {};
    int             ringUsed     = 0;    // how many samples are valid (≤ kMaxRingSamples)
    int             eventOffset  = 0;    // index into samplesL/R where markEvent() fired (the "now" sample)
    float           peakL        = 0.f;  // max |sample| across the window, left channel
    float           peakR        = 0.f;
    float           railPeakL    = 0.f;  // max |sample| that was >= kRailCeiling, or 0 if none
    float           railPeakR    = 0.f;
    int             railCountL   = 0;    // number of samples >= kRailCeiling in window
    int             railCountR   = 0;
    juce::int64     eventTimeMs  = 0;    // juce::Time::currentTimeMillis() at markEvent()
    const char*     reason       = "";   // "ClipConfirmed" or "NonFinite" (string literal; no alloc)
    DiagDeviceFormat format;
    bool            valid        = false; // false = consumeSnapshot() found no pending event
};

// Pre-allocated single-producer / single-consumer rolling capture buffer.
// The audio thread calls pushBlock() every callback (unconditional rolling write).
// When a clip/corruption event fires, it calls markEvent(); the GUI timer calls consumeSnapshot()
// which copies the ring into a DiagEventSnapshot and rearms for the next event.
// RT CONTRACT: pushBlock() and markEvent() allocate nothing, lock nothing, throw nothing.
// NON-RT CONTRACT: consumeSnapshot() may only be called from the GUI / message thread.
class DiagEventBuffer {
public:
    // Ring capacity in samples PER CHANNEL. Must be a power of two; kMaxRingSamples is the ceiling.
    // Default 4096 = ~85 ms at 48 kHz; 8192 = ~170 ms (the max, matching DiagEventSnapshot::kMaxRingSamples).
    static constexpr int kDefaultCapacity = 4096;

    // Prepare (or re-prepare) the buffer for a new run. Stores format, resets cursors, zeros ring.
    // NOT RT-safe (may be called from the setup thread before the audio stream starts; allocates nothing
    // because the ring is a fixed-size array). blockSize is provided so prepare() can assert the ring
    // is large enough to hold at least two blocks (one pre-event, one post-event).
    void prepare(DiagDeviceFormat fmt, int blockSize, int capacitySamples = kDefaultCapacity) noexcept;

    // RT-safe rolling write. Call once per capture callback BEFORE analyzeInputBlock.
    // l and r are the raw per-channel input pointers; n = numSamples.
    void pushBlock(const float* l, const float* r, int n) noexcept;

    // RT-safe event marker. Call from the audio thread immediately after a ClipConfirmed or NonFinite
    // flag is raised (i.e. after analyzeInputBlock returns when clipConfirmed() or NonFinite is new).
    // reason must be a string literal (no allocation). Sets pendingEvent_ + eventCursor_ atomically.
    // Idempotent: a second call before the GUI drains does NOT overwrite the first event cursor
    // (the first event is the diagnostic-relevant one; subsequent ones during the same burst are noise).
    void markEvent(const char* reason) noexcept;

    // Non-RT. Copy the ring into a DiagEventSnapshot, compute statistics, clear pendingEvent_.
    // Returns a snapshot with valid=false if no event is pending. Safe to call every GUI poll cycle.
    DiagEventSnapshot consumeSnapshot() noexcept;

    // True if an event is pending (non-RT poll; the GUI can skip the memcpy if false).
    bool hasPendingEvent() const noexcept { return pendingEvent_.load(std::memory_order_acquire); }

private:
    // Fixed-size ring (per channel). The ring is always fully allocated; unused entries are zero.
    float ringL_[DiagEventSnapshot::kMaxRingSamples] = {};
    float ringR_[DiagEventSnapshot::kMaxRingSamples] = {};
    int   capacity_   = kDefaultCapacity;  // active capacity (≤ kMaxRingSamples, power of two)
    int   mask_       = kDefaultCapacity - 1;  // capacity_ - 1 for fast modulo

    std::atomic<int>  writeCursor_ { 0 };   // next write position (mod capacity_); audio thread only advances
    std::atomic<int>  eventCursor_ { 0 };   // writeCursor_ value at markEvent() time
    std::atomic<bool> pendingEvent_ { false };
    const char*       reason_       = "";   // set in markEvent(); string literal; no alloc

    DiagDeviceFormat  format_;              // stable after prepare(); read in consumeSnapshot() (non-RT)
};

} // namespace eb
```

- [ ] **Step 1a: Write the failing test (skeleton — confirms the struct and class exist and compile)**

Create `tests/test_diageventbuffer.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/DiagEventBuffer.h"
#include <cstring>   // memset

TEST_CASE("DiagEventBuffer: prepare + hasPendingEvent starts false") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 128);
    CHECK_FALSE (buf.hasPendingEvent());
}

TEST_CASE("DiagEventBuffer: no-event consumeSnapshot returns valid=false") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 128);
    auto snap = buf.consumeSnapshot();
    CHECK_FALSE (snap.valid);
}
```

- [ ] **Step 1b: Add `test_diageventbuffer.cpp` to `tests/CMakeLists.txt`**

In `tests/CMakeLists.txt`, add `test_diageventbuffer.cpp` to the `add_executable(eb_tests ...)` source list (after `test_clipstatus.cpp`):
```cmake
add_executable(eb_tests
    test_smoke.cpp
    test_calfile.cpp
    test_firdesigner.cpp
    test_processinggraph.cpp
    test_deviceid.cpp
    test_modeldetect.cpp
    test_clockbridge.cpp
    test_healthmonitor.cpp
    test_devicemanager.cpp
    test_audioengine.cpp
    test_settings.cpp
    test_plotmath.cpp
    test_ratemenu.cpp
    test_calbinder.cpp
    test_asiofallback.cpp
    test_lrverify.cpp
    test_levelmeter.cpp
    test_calslot.cpp
    test_updatecheck.cpp
    test_clipstatus.cpp
    test_diageventbuffer.cpp)
```

- [ ] **Step 1c: Run build to verify it fails (missing class)**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
```
Expected: compiler error — `DiagEventBuffer.h` not found.

- [ ] **Step 1d: Create the header `src/audio/DiagEventBuffer.h`**

Paste the full class definition shown in **Interfaces produced** above into `src/audio/DiagEventBuffer.h`. Add the required includes at the top:
```cpp
#pragma once
#include <atomic>
#include <juce_core/juce_core.h>   // juce::int64, juce::Time
#include "audio/HealthMonitor.h"   // kRailCeiling constant used in consumeSnapshot statistics
```
Then paste the struct and class bodies exactly as defined in **Interfaces produced**.

- [ ] **Step 1e: Create `src/audio/DiagEventBuffer.cpp`** with stub bodies (enough to link):

```cpp
#include "audio/DiagEventBuffer.h"
#include <algorithm>
#include <cstring>
#include <cmath>
namespace eb {

void DiagEventBuffer::prepare(DiagDeviceFormat fmt, int /*blockSize*/, int capacitySamples) noexcept {
    // Clamp to the fixed array size; round down to power of two.
    int cap = 1;
    while (cap * 2 <= capacitySamples && cap * 2 <= DiagEventSnapshot::kMaxRingSamples) cap *= 2;
    capacity_ = cap;
    mask_     = cap - 1;
    format_   = fmt;
    writeCursor_.store(0, std::memory_order_relaxed);
    eventCursor_.store(0, std::memory_order_relaxed);
    pendingEvent_.store(false, std::memory_order_relaxed);
    reason_ = "";
    std::memset(ringL_, 0, sizeof(ringL_));
    std::memset(ringR_, 0, sizeof(ringR_));
}

void DiagEventBuffer::pushBlock(const float* /*l*/, const float* /*r*/, int /*n*/) noexcept {}

void DiagEventBuffer::markEvent(const char* /*reason*/) noexcept {}

DiagEventSnapshot DiagEventBuffer::consumeSnapshot() noexcept { return {}; }

} // namespace eb
```

- [ ] **Step 1f: Run build and test to verify stubs compile + skeleton tests pass**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: 2 tests PASS (skeleton), 0 fail.

- [ ] **Step 1g: Commit**

```bash
git add src/audio/DiagEventBuffer.h src/audio/DiagEventBuffer.cpp tests/test_diageventbuffer.cpp tests/CMakeLists.txt
git commit -m "feat(diag): DiagEventBuffer skeleton + test scaffold

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `pushBlock` — RT-safe rolling write

**Files:**
- `src/audio/DiagEventBuffer.cpp` (implement `pushBlock`)
- `tests/test_diageventbuffer.cpp` (add rolling-write tests)

**Interfaces:**
- `void pushBlock(const float* l, const float* r, int n) noexcept` — writes `n` samples from `l`/`r` into `ringL_`/`ringR_` starting at `writeCursor_`, wrapping at `capacity_`. Advances `writeCursor_` by `n`. RT-safe: plain loop + one atomic store. No lock, no allocation. When `n > capacity_`, only the last `capacity_` samples are retained (the ring is always full after the first complete rotation — the oldest are overwritten first).

- [ ] **Step 2a: Write the failing tests**

Append to `tests/test_diageventbuffer.cpp`:
```cpp
TEST_CASE("DiagEventBuffer::pushBlock writes samples into the ring (single block, no wrap)") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 4, 16);   // capacity=16, block=4

    // Push one block: L=[1,2,3,4], R=[5,6,7,8]
    const float l[] = { 1.f, 2.f, 3.f, 4.f };
    const float r[] = { 5.f, 6.f, 7.f, 8.f };
    buf.pushBlock (l, r, 4);

    // Snapshot without event — valid=false; we verify ring contents via a mark trick
    buf.markEvent ("test");
    auto snap = buf.consumeSnapshot();
    REQUIRE (snap.valid);
    // The ring has only 4 written samples out of 16; ringUsed == capacity_ == 16
    // but the first 4 slots [0..3] hold our data and the rest are zero.
    // chronological order: oldest first (the ring has one block).
    // We expect samplesL[capacity-4..capacity-1] because the write started at cursor=0 and advanced to 4.
    // After consumeSnapshot reorders (oldest first), index 0..3 == the block we wrote.
    CHECK (snap.samplesL[0] == 1.f);
    CHECK (snap.samplesL[1] == 2.f);
    CHECK (snap.samplesL[3] == 4.f);
    CHECK (snap.samplesR[0] == 5.f);
}

TEST_CASE("DiagEventBuffer::pushBlock wraps correctly at ring boundary") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 4, 8);   // capacity=8, block=4

    // Fill the ring with two full blocks (8 samples = full capacity)
    const float blockA[4] = { 1.f, 2.f, 3.f, 4.f };
    const float blockB[4] = { 5.f, 6.f, 7.f, 8.f };
    const float zero[4]   = { 0.f, 0.f, 0.f, 0.f };
    buf.pushBlock (blockA, zero, 4);
    buf.pushBlock (blockB, zero, 4);   // wraps; cursor is now 0 (mod 8 = 8 = 0)

    // Push a third block, overwriting blockA (the oldest)
    const float blockC[4] = { 9.f, 10.f, 11.f, 12.f };
    buf.pushBlock (blockC, zero, 4);

    buf.markEvent ("test");
    auto snap = buf.consumeSnapshot();
    REQUIRE (snap.valid);
    // After wrap the oldest remaining block is blockB [5,6,7,8], then blockC [9,10,11,12]
    CHECK (snap.samplesL[0] == 5.f);
    CHECK (snap.samplesL[4] == 9.f);
    CHECK (snap.ringUsed == 8);
}
```

- [ ] **Step 2b: Run tests to verify they fail (pushBlock is a no-op)**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: the two new tests FAIL; the skeleton tests PASS.

- [ ] **Step 2c: Implement `pushBlock`**

Replace the stub in `src/audio/DiagEventBuffer.cpp`:
```cpp
void DiagEventBuffer::pushBlock(const float* l, const float* r, int n) noexcept {
    // RT-safe: plain loop, atomic load/store, no alloc/lock/syscall.
    // When n > capacity_ we skip the oldest (n - capacity_) samples so we never over-run our own ring.
    int skip = (n > capacity_) ? (n - capacity_) : 0;
    const float* sl = l + skip;
    const float* sr = r + skip;
    int count  = n - skip;
    int cursor = writeCursor_.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        ringL_[cursor & mask_] = sl[i];
        ringR_[cursor & mask_] = sr[i];
        ++cursor;
    }
    writeCursor_.store(cursor, std::memory_order_release);   // release: consumeSnapshot acquire-loads this
}
```

- [ ] **Step 2d: Implement `consumeSnapshot` (ring copy + reorder only — stats added in Task 4)**

Replace the stub:
```cpp
DiagEventSnapshot DiagEventBuffer::consumeSnapshot() noexcept {
    DiagEventSnapshot snap;
    if (!pendingEvent_.load(std::memory_order_acquire)) return snap;  // snap.valid == false

    const int writeCur   = writeCursor_.load(std::memory_order_acquire);
    const int eventCur   = eventCursor_.load(std::memory_order_relaxed);
    const int cap        = capacity_;

    // Linearise the ring: oldest sample is at (writeCur % cap), newest is just before writeCur.
    snap.ringUsed = cap;   // always full after ≥1 rotation; partial before that is handled below
    // Copy L then R in chronological order (oldest first).
    for (int i = 0; i < cap; ++i) {
        int idx = (writeCur - cap + i + (1 << 30)) & mask_;   // (writeCur - cap + i) mod cap, always positive
        snap.samplesL[i] = ringL_[idx];
        snap.samplesR[i] = ringR_[idx];
    }
    // eventOffset = how many samples before the END of the snapshot the event fired.
    // eventCur points to the writeCursor at markEvent() time.
    snap.eventOffset = (eventCur - (writeCur - cap) + (1 << 30)) & (cap - 1);
    snap.eventTimeMs = juce::Time::currentTimeMillis();
    snap.reason      = reason_;
    snap.format      = format_;
    snap.valid       = true;

    pendingEvent_.store(false, std::memory_order_release);
    return snap;
}
```

- [ ] **Step 2e: Run tests to verify they pass**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: all 4 tests PASS.

- [ ] **Step 2f: Commit**

```bash
git add src/audio/DiagEventBuffer.cpp tests/test_diageventbuffer.cpp
git commit -m "feat(diag): DiagEventBuffer pushBlock rolling write + consumeSnapshot ring copy

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: `markEvent` — RT-safe event marker

**Files:**
- `src/audio/DiagEventBuffer.cpp` (implement `markEvent`)
- `tests/test_diageventbuffer.cpp` (add markEvent tests)

**Interfaces:**
- `void markEvent(const char* reason) noexcept` — atomically captures `writeCursor_` into `eventCursor_`, stores `reason_` (string literal pointer, no alloc), and sets `pendingEvent_ = true` (with `memory_order_release`). Idempotent: if `pendingEvent_` is already `true` (i.e. the GUI has not consumed the previous snapshot), does nothing — the first event is the diagnostic-relevant one.

- [ ] **Step 3a: Write the failing tests**

Append to `tests/test_diageventbuffer.cpp`:
```cpp
TEST_CASE("DiagEventBuffer::markEvent sets hasPendingEvent and consumeSnapshot drains it") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 4, 8);

    CHECK_FALSE (buf.hasPendingEvent());

    const float l[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    const float z[4] = {};
    buf.pushBlock (l, z, 4);
    buf.markEvent ("ClipConfirmed");

    CHECK (buf.hasPendingEvent());

    auto snap = buf.consumeSnapshot();
    CHECK (snap.valid);
    CHECK (std::string(snap.reason) == "ClipConfirmed");
    CHECK_FALSE (buf.hasPendingEvent());   // consumed
}

TEST_CASE("DiagEventBuffer::markEvent is idempotent: first event wins, second is dropped") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 4, 8);

    const float la[4] = { 1.f, 1.f, 1.f, 1.f };
    const float lb[4] = { 2.f, 2.f, 2.f, 2.f };
    const float z[4]  = {};
    buf.pushBlock (la, z, 4);
    buf.markEvent ("ClipConfirmed");          // first event
    buf.pushBlock (lb, z, 4);
    buf.markEvent ("NonFinite");              // second event before GUI drains — should be ignored

    auto snap = buf.consumeSnapshot();
    REQUIRE (snap.valid);
    CHECK (std::string(snap.reason) == "ClipConfirmed");   // first event label is preserved
}

TEST_CASE("DiagEventBuffer::consumeSnapshot after markEvent without pushBlock returns valid=true with zero data") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 4, 8);
    buf.markEvent ("NonFinite");   // no pushBlock — ring is all zeros
    auto snap = buf.consumeSnapshot();
    CHECK (snap.valid);
    CHECK (snap.samplesL[0] == 0.f);
    CHECK (snap.format.sampleRate == 48000.0);
}
```

- [ ] **Step 3b: Run tests to verify the new tests fail**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: 3 new tests FAIL; previous 4 PASS.

- [ ] **Step 3c: Implement `markEvent`**

Replace the stub in `src/audio/DiagEventBuffer.cpp`:
```cpp
void DiagEventBuffer::markEvent(const char* reason) noexcept {
    // Idempotent: if a previous event hasn't been consumed, keep the first cursor.
    if (pendingEvent_.load(std::memory_order_acquire)) return;
    // Snapshot the write cursor BEFORE setting pendingEvent_ so the consumer sees a consistent pair.
    eventCursor_.store(writeCursor_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    reason_ = reason;
    pendingEvent_.store(true, std::memory_order_release);
}
```

- [ ] **Step 3d: Run tests to verify all pass**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: all 7 tests PASS.

- [ ] **Step 3e: Commit**

```bash
git add src/audio/DiagEventBuffer.cpp tests/test_diageventbuffer.cpp
git commit -m "feat(diag): markEvent idempotent RT-safe event marker

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: `consumeSnapshot` statistics (peak, rail positions, channel identity)

**Files:**
- `src/audio/DiagEventBuffer.cpp` (extend `consumeSnapshot` with per-channel stats)
- `tests/test_diageventbuffer.cpp` (add stats tests)

**Interfaces:**
- The stats fields of `DiagEventSnapshot` already declared in Task 1: `peakL`, `peakR`, `railPeakL`, `railPeakR`, `railCountL`, `railCountR`. These are computed on the **non-RT** path inside `consumeSnapshot()` by scanning the linearised `samplesL`/`samplesR` arrays after the ring copy. `HealthMonitor::kRailCeiling` (0.9999f) is reused as the rail threshold. No new atomics, no audio-thread work.

- [ ] **Step 4a: Write the failing tests**

Append to `tests/test_diageventbuffer.cpp`:
```cpp
TEST_CASE("DiagEventBuffer: consumeSnapshot computes per-channel peak and rail stats correctly") {
    using eb::HealthMonitor;
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 48000.0, 24, 2 };
    buf.prepare (fmt, 4, 8);

    // Block 1: L has a rail run of 3, R is normal
    const float lA[] = { 0.5f, 1.0f, 1.0f, 1.0f };   // 3 samples at full scale
    const float rA[] = { 0.3f, 0.4f, 0.3f, 0.2f };
    buf.pushBlock (lA, rA, 4);
    // Block 2: L quiet, R peaks at -0.5
    const float lB[] = { 0.1f, 0.1f, 0.1f, 0.1f };
    const float rB[] = { -0.5f, 0.0f, 0.0f, 0.0f };
    buf.pushBlock (lB, rB, 4);

    buf.markEvent ("ClipConfirmed");
    auto snap = buf.consumeSnapshot();
    REQUIRE (snap.valid);

    // peakL should be 1.0 (the rail values)
    CHECK (snap.peakL == 1.0f);
    // peakR should be 0.5 (abs of -0.5)
    CHECK (snap.peakR == 0.5f);
    // railPeakL should be 1.0; railCountL should be 3
    CHECK (snap.railPeakL == 1.0f);
    CHECK (snap.railCountL == 3);
    // R never crossed kRailCeiling
    CHECK (snap.railCountR == 0);
    CHECK (snap.railPeakR == 0.f);
}

TEST_CASE("DiagEventBuffer: consumeSnapshot format fields match prepare()") {
    eb::DiagEventBuffer buf;
    eb::DiagDeviceFormat fmt { 96000.0, 32, 2 };
    buf.prepare (fmt, 4, 8);
    buf.markEvent ("NonFinite");
    auto snap = buf.consumeSnapshot();
    REQUIRE (snap.valid);
    CHECK (snap.format.sampleRate  == 96000.0);
    CHECK (snap.format.bitDepth    == 32);
    CHECK (snap.format.numChannels == 2);
}
```

- [ ] **Step 4b: Run tests to verify new tests fail**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: 2 new tests FAIL; previous 7 PASS.

- [ ] **Step 4c: Extend `consumeSnapshot` with the stats pass**

In `src/audio/DiagEventBuffer.cpp`, inside `consumeSnapshot()`, after the ring-copy loop and before the `snap.valid = true` line, add:
```cpp
    // Non-RT statistics pass over the linearised samples.
    for (int i = 0; i < cap; ++i) {
        const float al = std::abs(snap.samplesL[i]);
        const float ar = std::abs(snap.samplesR[i]);
        if (al > snap.peakL) snap.peakL = al;
        if (ar > snap.peakR) snap.peakR = ar;
        if (al >= HealthMonitor::kRailCeiling) { ++snap.railCountL; if (al > snap.railPeakL) snap.railPeakL = al; }
        if (ar >= HealthMonitor::kRailCeiling) { ++snap.railCountR; if (ar > snap.railPeakR) snap.railPeakR = ar; }
    }
```

Also add `#include "audio/HealthMonitor.h"` at the top of `DiagEventBuffer.cpp` if not already present (it is included via the header chain but make it explicit).

- [ ] **Step 4d: Run tests to verify all pass**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "DiagEventBuffer" --output-on-failure
```
Expected: all 9 tests PASS.

- [ ] **Step 4e: Commit**

```bash
git add src/audio/DiagEventBuffer.cpp tests/test_diageventbuffer.cpp
git commit -m "feat(diag): consumeSnapshot per-channel peak + rail statistics

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Wire `DiagEventBuffer` into `AudioEngine`

**Files:**
- `src/audio/AudioEngine.h` — add `DiagEventBuffer diagBuf_` member + `consumeDiagSnapshot()` declaration
- `src/audio/AudioEngine.cpp` — `CaptureCallback` calls `pushBlock` + `markEvent` on events; `AudioEngine::consumeDiagSnapshot()` delegates to `diagBuf_`
- `tests/test_audioengine.cpp` — add seam-level tests confirming markEvent fires on clip/nonfinite

**Interfaces:**
- `DiagEventBuffer eb::AudioEngine::diagBuf_` — private member; `prepare()` called inside `AudioEngine::start()` immediately after `hm.prepare(...)` (before the stream opens), with `grantedRate_`, `grantedBitDepth`, `2` channels, and the block size derived from the device.
- `DiagEventSnapshot eb::AudioEngine::consumeDiagSnapshot() noexcept` — public; forwards to `diagBuf_.consumeSnapshot()`. Called by the GUI timer (non-RT).
- In `CaptureCallback::audioDeviceIOCallbackWithContext`:
  1. `e.diagBuf_.pushBlock(l, r, numSamples)` — called FIRST, before `hm.analyzeInputBlock`, so the pre-event audio is already in the ring.
  2. After `hm.analyzeInputBlock(l, r, numSamples)`: if `hm.clipConfirmed()` was newly raised **this block** (i.e. `ClipConfirmed` was not set before this call but is set after) — `e.diagBuf_.markEvent("ClipConfirmed")`.
  3. After `hm.scanAndFlagNonFinite(mono.data(), numSamples)` returns `true` — `e.diagBuf_.markEvent("NonFinite")`.

  **Detecting "newly raised this block":** add a `bool wasClipConfirmedBefore` local that snapshots `hm.clipConfirmed()` BEFORE the `analyzeInputBlock` call. After the call, if `!wasClipConfirmedBefore && hm.clipConfirmed()` → markEvent. This fires markEvent exactly once per event burst (the first block), matching the idempotent markEvent contract.

- [ ] **Step 5a: Write the failing seam tests**

Append to `tests/test_audioengine.cpp`:
```cpp
TEST_CASE("AudioEngine seam: DiagEventBuffer is armed after a confirmed clip block") {
    eb::AudioEngine eng;
    eng.prepareForTest (48000.0, 128);

    // Build a 128-sample block with a 3-sample positive-rail run on L (confirmed clip).
    std::vector<float> inL (128, 0.1f);
    std::vector<float> inR (128, 0.1f);
    inL[64] = 1.0f; inL[65] = 1.0f; inL[66] = 1.0f;   // 3 consecutive rail samples
    std::vector<float> out (128);
    eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), 128);

    // The DiagEventBuffer should now have a pending event.
    auto snap = eng.consumeDiagSnapshot();
    CHECK (snap.valid);
    CHECK (std::string(snap.reason) == "ClipConfirmed");
    CHECK (snap.railCountL >= 3);
}

TEST_CASE("AudioEngine seam: DiagEventBuffer is armed after a NonFinite block") {
    eb::AudioEngine eng;
    eng.prepareForTest (48000.0, 128);

    std::vector<float> inL (128, 0.1f);
    std::vector<float> inR (128, 0.1f);
    inL[10] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> out (128);
    eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), 128);

    auto snap = eng.consumeDiagSnapshot();
    CHECK (snap.valid);
    CHECK (std::string(snap.reason) == "NonFinite");
}

TEST_CASE("AudioEngine seam: DiagEventBuffer has no pending event on a clean block") {
    eb::AudioEngine eng;
    eng.prepareForTest (48000.0, 128);

    std::vector<float> inL (128, 0.1f);
    std::vector<float> inR (128, 0.1f);
    std::vector<float> out (128);
    eng.processCaptureBlockForTest (inL.data(), inR.data(), out.data(), 128);

    auto snap = eng.consumeDiagSnapshot();
    CHECK_FALSE (snap.valid);
}
```

- [ ] **Step 5b: Run tests to verify they fail**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build -R "AudioEngine" --output-on-failure
```
Expected: 3 new tests FAIL (symbol not found); previous AudioEngine tests PASS.

- [ ] **Step 5c: Add `diagBuf_` member to `AudioEngine`**

In `src/audio/AudioEngine.h`, add `#include "audio/DiagEventBuffer.h"` after the existing includes, then add to the `private:` section (after `HealthMonitor hm;`):
```cpp
    DiagEventBuffer diagBuf_;
```
And add to the public telemetry section (after `cleanCapture()`):
```cpp
    DiagEventSnapshot consumeDiagSnapshot() noexcept;
```

- [ ] **Step 5d: Implement `AudioEngine::consumeDiagSnapshot`**

In `src/audio/AudioEngine.cpp`, add the body (anywhere in the non-callback section, e.g. near `healthFlags()`):
```cpp
DiagEventSnapshot AudioEngine::consumeDiagSnapshot() noexcept {
    return diagBuf_.consumeSnapshot();
}
```

- [ ] **Step 5e: Add `diagBuf_.prepare()` call inside `AudioEngine::start()`**

In `src/audio/AudioEngine.cpp`, find the `start()` method and, immediately after `hm.prepare(...)` is called (before the stream opens), add:
```cpp
    {
        eb::DiagDeviceFormat fmt;
        fmt.sampleRate   = activeRate;
        fmt.bitDepth     = outputBits;   // closest proxy; capture bit depth is the granted depth
        fmt.numChannels  = 2;
        diagBuf_.prepare (fmt, blockSize);
    }
```

- [ ] **Step 5f: Wire `pushBlock` + `markEvent` into `CaptureCallback`**

In `src/audio/AudioEngine.cpp`, inside `CaptureCallback::audioDeviceIOCallbackWithContext`, make these changes in order:

1. Before `e.hm.analyzeInputBlock(l, r, numSamples)`, add:
```cpp
        e.diagBuf_.pushBlock (l, r, numSamples);              // rolling pre-event record
        const bool wasClipBefore = e.hm.clipConfirmed();      // snapshot before analysis
```

2. After `e.hm.analyzeInputBlock(l, r, numSamples)`, add:
```cpp
        if (!wasClipBefore && e.hm.clipConfirmed())
            e.diagBuf_.markEvent ("ClipConfirmed");            // first block of the confirmed clip
```

3. Change the existing `scanAndFlagNonFinite` block from:
```cpp
        if (e.hm.scanAndFlagNonFinite (mono.data(), numSamples))
            juce::FloatVectorOperations::clear (mono.data(), numSamples);
```
to:
```cpp
        if (e.hm.scanAndFlagNonFinite (mono.data(), numSamples)) {
            e.diagBuf_.markEvent ("NonFinite");
            juce::FloatVectorOperations::clear (mono.data(), numSamples);
        }
```

- [ ] **Step 5g: Extend the test seam `processCaptureBlockForTest` to drive `diagBuf_`**

In `src/audio/AudioEngine.cpp`, find `processCaptureBlockForTest` (lines ~351-354). The seam currently calls `graph.process` directly. Extend it to mirror the real callback:
```cpp
void AudioEngine::processCaptureBlockForTest (const float* inL, const float* inR,
                                               float* outMono, int numSamples) {
    diagBuf_.pushBlock (inL, inR, numSamples);
    const bool wasClipBefore = hm.clipConfirmed();
    hm.analyzeInputBlock (inL, inR, numSamples);
    if (!wasClipBefore && hm.clipConfirmed())
        diagBuf_.markEvent ("ClipConfirmed");
    graph.process (inL, inR, outMono, numSamples);
    if (hm.scanAndFlagNonFinite (outMono, numSamples)) {
        diagBuf_.markEvent ("NonFinite");
        juce::FloatVectorOperations::clear (outMono, numSamples);
    }
}
```

- [ ] **Step 5h: Run full test suite**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build --output-on-failure
```
Expected: all 101 tests PASS (98 existing + 3 new AudioEngine + 9 DiagEventBuffer — total = 110; the count depends on how many DiagEventBuffer tests were accumulated from Tasks 1-4). Adjust count to actual.

- [ ] **Step 5i: Commit**

```bash
git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp tests/test_audioengine.cpp
git commit -m "feat(diag): wire DiagEventBuffer into AudioEngine capture callback

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Full-suite green gate

**Files:** no new files

- [ ] **Step 6a: Run the full test suite and confirm all tests pass**

```bash
./tools/dev.cmd cmake --build build --target eb_tests
./tools/dev.cmd ctest --test-dir build --output-on-failure
```
Expected: all tests PASS; count ≥ 98 (the baseline before this plan).

- [ ] **Step 6b: Verify no audio-thread RT violations were introduced**

Manually inspect `DiagEventBuffer::pushBlock` and `DiagEventBuffer::markEvent` for any of: `new`/`delete`/`malloc`/`free`, `std::vector` resize, `std::mutex`, `std::cout`/`DBG`, `jassert`-that-throws, syscalls, I/O. Confirm: plain loops + `std::atomic` stores only. (RT-safety is structural here — the ring is a fixed-size array and both methods do only arithmetic and atomic stores.)

- [ ] **Step 6c: Final commit**

```bash
git add -p   # stage only if there are any last fixups
git commit -m "chore(diag): full-suite green gate for diag-event-capture-buffer

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```
(Skip this commit if there are no changes to stage; the previous task's commit is the final one.)

---

## Self-review — mapping to audit §11 and §6 (optional diagnostics entry)

| Requirement from audit | Addressed by |
|---|---|
| **Pre-allocated** circular buffer of recent raw EARS audio | `DiagEventBuffer` uses two fixed-size `float[kMaxRingSamples]` arrays; no heap allocation after `prepare()` |
| **Snapshot when a clip or corruption fires** | `markEvent("ClipConfirmed")` / `markEvent("NonFinite")` called from `CaptureCallback` on the first block where the flag is raised; idempotent — subsequent blocks in the same burst do not overwrite |
| **Retains audio before/during/after** the event | `pushBlock` is unconditional and called BEFORE `analyzeInputBlock`; the ring always holds the last `capacity_` (4096 default) samples; `eventOffset` in the snapshot locates the "during" position |
| **Peaks, rail positions, timestamps, device format, channel identity** | `DiagEventSnapshot` carries `peakL/R`, `railPeakL/R`, `railCountL/R`, `eventTimeMs`, `DiagDeviceFormat{sampleRate, bitDepth, numChannels}`, and `reason` (channel identity is per-channel L/R) |
| **Audio thread ONLY writes** to ring + sets atomic marker | `pushBlock` + `markEvent` touch only `ringL_`/`ringR_` (pre-allocated) + `std::atomic` stores; no lock, no syscall, no alloc |
| **Non-RT consumer serializes** — never blocking disk I/O on audio thread | `consumeSnapshot()` is called from `AudioEngine::consumeDiagSnapshot()` which is polled by the GUI timer (message thread, 30 Hz); no file I/O anywhere in the chain |
| **Out of scope for earlier phases; belongs in a later phase** (audit §6) | This plan is explicitly the optional-diagnostics phase (Phase 5 in the audit's §8 table) |
