# Clip Statistics & Headroom Readout Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox syntax.

**Goal:** Extend `HealthMonitor::analyzeInputBlock` to track the full clip-statistics set required by audit findings R4 and R8: positive vs negative rail-hit split, first/last clip-sample index within a run, clipped-sample percentage, and numeric per-channel session peak. Expose all of this through new RT-safe accessors on `HealthMonitor`. Surface a numeric per-ear peak dBFS + remaining-headroom readout in the GUI levels card (right pane, below the existing meters) as a `juce::Label` updated each 30 Hz timer tick.

**Architecture:** All new counters follow the existing lock-free pattern in `HealthMonitor`: capture-thread-only scratch integers for in-block accumulation, published to `std::atomic` at the end of `analyzeInputBlock`. The GUI reads the atomics via new accessors from the timer thread (30 Hz). No new threads, no new libraries.

**Design decision:** The first/last clip-sample index is published as an absolute sample counter (total samples seen since `reset()`) rather than as a per-block offset. This makes the position meaningful to any future session-scoped consumer without requiring the caller to pass an external clock. The alternative (per-block offset only) loses positional information across blocks. A `std::atomic<long long>` holds the index; the GUI converts it to a wall-clock time using the captured sample rate for display if desired, but for now it is exposed as a raw sample count.

**Tech Stack:** C++17, JUCE 8, Catch2 (existing test harness), MSVC via `./tools/dev.cmd`.

---

## Global Constraints

- Build (tests): ./tools/dev.cmd cmake --build build --target eb_tests  — run tools/dev.cmd DIRECTLY from Bash, never bare cmake (it loses the MSVC env) and never cmd /c; it wraps Ninja + MSVC.
- Run a test: ./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure . Full suite: ./tools/dev.cmd ctest --test-dir build --output-on-failure (must stay green; 98 tests today).
- RT-safety: code reachable from an audio callback must have NO heap allocation, lock, syscall, logging, or exception — plain locals + std::atomic only.
- HealthFlag (src/audio/EngineTypes.h) is NOT persisted — append enum values freely. CombineMode IS persisted by index — never change its existing values.
- No real EARS serial in any file (tests use synthetic buffers).
- Commit trailer on every commit: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
- Work on a feature branch, not main.

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `src/audio/EngineTypes.h` | Modify | Add `ClipStats` struct (all per-session clip statistics in one snapshot-able value type) |
| `src/audio/HealthMonitor.h` | Modify | Add `ClipStats clipStats() const noexcept` accessor; add private atomic members for sign split, first/last index, total samples seen, session peak L/R |
| `src/audio/HealthMonitor.cpp` | Modify | Extend `analyzeInputBlock` to track sign split, first/last absolute index, running total-sample count, session peak L/R; publish to atomics; extend `reset()` to clear new state |
| `src/gui/MainComponent.h` | Modify | Add `juce::Label headroomLabel` member |
| `src/gui/MainComponent.cpp` | Modify | Construct `headroomLabel`, lay it out below the meters in `resized()`, update it each 30 Hz tick in `timerCallback()` from `engine.clipStats()` and `engine.levels()` |
| `src/audio/AudioEngine.h` | Modify | Add `ClipStats clipStats() const noexcept` forwarding to `hm` |
| `src/audio/AudioEngine.cpp` | Modify | Implement the forwarding accessor |
| `tests/test_healthmonitor.cpp` | Modify | Add test cases for the new statistics (sign split, first/last index, percentage, session peak, reset behaviour) |

---

## Task 1: Add `ClipStats` struct to `EngineTypes.h` and extend `HealthMonitor` with new atomic members

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/audio/EngineTypes.h`
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/audio/HealthMonitor.h`

**Interfaces:**

Consumes:
- Existing `HealthFlag`, `Health`, `Levels` types in `src/audio/EngineTypes.h` (lines 1-55)
- Existing private members of `HealthMonitor`: `railSamplesL_`, `railSamplesR_`, `longestRunA_`, `clipConfirmed_`, `railRunL_`, `railRunR_`, `longestRun_` (HealthMonitor.h lines 104-106)

Produces:
- `eb::ClipStats` struct in `EngineTypes.h` — a plain value type, copyable, no atomics, safe to read on the GUI thread from a snapshot
- `HealthMonitor::clipStats() const noexcept -> ClipStats` — reads atomics, returns a `ClipStats` snapshot (GUI-thread-safe)
- New private atomics in `HealthMonitor`: `posRailL_`, `posRailR_`, `negRailL_`, `negRailR_` (`std::atomic<int>`), `firstClipIdx_`, `lastClipIdx_` (`std::atomic<long long>`), `totalSamplesA_` (`std::atomic<long long>`), `sessionPeakL_`, `sessionPeakR_` (`std::atomic<int>`, stored as `peak * 1000` to match the existing fixed-point convention for `inLm`/`inRm`)
- Capture-thread-only scratch: `long long totalSamples_` (mirrors `totalSamplesA_` for use inside `analyzeInputBlock` without a load per sample)

- - -

- [ ] Write the failing test (see Task 2 — `clipStats()` is tested there; this task has no standalone test because the struct definition is pure data). Verify the build fails before the implementation by attempting to call `clipStats()` on `HealthMonitor` (the method doesn't exist yet).

```bash
# From Bash (not PowerShell) — runs inside the MSVC env via tools/dev.cmd
./tools/dev.cmd cmake --build build --target eb_tests 2>&1 | tail -20
```

- [ ] Add `ClipStats` to `src/audio/EngineTypes.h` immediately after the `Health` struct (line 42):

```cpp
// --- Per-session clip statistics (R4 / R8) ---
// Snapshot produced by HealthMonitor::clipStats(); safe to read on any thread.
struct ClipStats {
    // Total rail-hit counts split by sign and channel.
    int posRailL  = 0;   // samples >= +kRailCeiling on the left channel
    int negRailL  = 0;   // samples <= -kRailCeiling on the left channel
    int posRailR  = 0;
    int negRailR  = 0;

    // Absolute sample index (since the last reset) of the first and last confirmed-clip sample seen.
    // -1 = no clip has occurred yet this session.
    long long firstClipIdx = -1;
    long long lastClipIdx  = -1;

    // Total samples analyzed since the last reset (both channels contribute equally; this is the
    // per-channel denominator for the clipped-sample percentage).
    long long totalSamples = 0;

    // Per-channel session peak (linear, 0..1+).
    float sessionPeakL = 0.0f;
    float sessionPeakR = 0.0f;

    // Derived helpers (computed on demand; not stored in atomics).
    // Clipped-sample percentage: rail hits on either channel as a fraction of total samples.
    float clippedPct() const noexcept {
        if (totalSamples <= 0) return 0.0f;
        const int total = posRailL + negRailL + posRailR + negRailR;
        return 100.0f * static_cast<float>(total) / static_cast<float>(totalSamples);
    }
    // Per-channel peak in dBFS (-inf represented as -144 dBFS for display).
    float peakLDb() const noexcept {
        return (sessionPeakL > 1.0e-7f) ? 20.0f * std::log10(sessionPeakL) : -144.0f;
    }
    float peakRDb() const noexcept {
        return (sessionPeakR > 1.0e-7f) ? 20.0f * std::log10(sessionPeakR) : -144.0f;
    }
    // Remaining headroom: distance from the session peak to 0 dBFS (positive = headroom remaining).
    float headroomLDb() const noexcept { return -peakLDb(); }
    float headroomRDb() const noexcept { return -peakRDb(); }
};
```

Note: `std::log10` is used in the derived helpers, so add `#include <cmath>` to the translation units that call `clippedPct`/`peakLDb`/etc. if not already present. `EngineTypes.h` itself only needs a forward `#include <cmath>` guard — check that `juce_core/juce_core.h` already pulls it in (it does via `<cmath>` in `juce_MathsFunctions.h`).

- [ ] Add private atomics and scratch to `HealthMonitor` (in `src/audio/HealthMonitor.h`), immediately after the existing confirmed-clip block (lines 104-106):

```cpp
    // --- Plan 5 additions: full clip statistics (R4 / R8) ---
    // Capture-thread-only scratch (written only inside analyzeInputBlock).
    long long totalSamples_  = 0;   // running total samples seen since reset (per-channel denominator)
    int posRunL_   = 0, negRunL_   = 0;   // per-sample rail-sign accumulators for the current block
    int posRunR_   = 0, negRunR_   = 0;

    // Published to GUI thread via atomics at the end of each analyzeInputBlock call.
    std::atomic<int>       posRailL_ { 0 }, negRailL_ { 0 };
    std::atomic<int>       posRailR_ { 0 }, negRailR_ { 0 };
    std::atomic<long long> totalSamplesA_ { 0 };
    std::atomic<long long> firstClipIdx_  { -1 };
    std::atomic<long long> lastClipIdx_   { -1 };
    std::atomic<int>       sessionPeakLm_ { 0 };   // peak * 1000, matches inLm/inRm convention
    std::atomic<int>       sessionPeakRm_ { 0 };
```

- [ ] Add the `clipStats()` accessor declaration to the public section of `HealthMonitor` (after `clipLongestRun()` at line 74):

```cpp
    ClipStats clipStats() const noexcept;
```

- [ ] Run the build to confirm it compiles (the method body is added in Task 3):

```bash
./tools/dev.cmd cmake --build build --target eb_tests 2>&1 | tail -20
```

- [ ] Commit:

```
git checkout -b feature/clip-statistics
git add src/audio/EngineTypes.h src/audio/HealthMonitor.h
git commit -m "$(cat <<'EOF'
feat(R4/R8): add ClipStats struct and HealthMonitor atomic declarations

Adds eb::ClipStats (positive/negative rail split, first/last clip index,
total-sample count, per-channel session peak, derived dBFS helpers) to
EngineTypes.h and declares the matching private atomics + clipStats()
accessor in HealthMonitor — no behaviour change yet.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Write failing tests for the new clip statistics

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/tests/test_healthmonitor.cpp`

**Interfaces:**

Consumes:
- `eb::HealthMonitor::clipStats() const noexcept -> ClipStats` (declared in Task 1, not yet implemented)
- `eb::ClipStats` struct fields: `posRailL`, `negRailL`, `posRailR`, `negRailR`, `firstClipIdx`, `lastClipIdx`, `totalSamples`, `sessionPeakL`, `sessionPeakR`, and helpers `clippedPct()`, `peakLDb()`, `headroomLDb()`
- Existing test helpers: `Catch::Matchers::WithinAbs`, the block-builder lambda pattern already used at test_healthmonitor.cpp:233

Produces: Failing Catch2 test cases appended to `tests/test_healthmonitor.cpp`

- - -

- [ ] Append the following test cases to `tests/test_healthmonitor.cpp`:

```cpp
// ============================================================
// Plan 5: Clip statistics (R4 / R8)
// ============================================================

TEST_CASE("ClipStats: positive and negative rail hits counted separately per channel") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);

    // L: two positive hits, zero negative. R: one negative hit.
    std::vector<float> l { 0.5f,  1.0f,  1.0f, -0.5f };   // pos rail at [1],[2]
    std::vector<float> r { 0.5f, -1.0f, -0.5f,  0.5f };   // neg rail at [1]
    h.analyzeInputBlock(l.data(), r.data(), (int)l.size());

    const auto s = h.clipStats();
    CHECK(s.posRailL == 2);
    CHECK(s.negRailL == 0);
    CHECK(s.posRailR == 0);
    CHECK(s.negRailR == 1);
}

TEST_CASE("ClipStats: firstClipIdx and lastClipIdx track the absolute sample position") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);

    // Block 1: 4 samples, no rail hit.
    std::vector<float> clean(4, 0.5f);
    std::vector<float> zero(4, 0.0f);
    h.analyzeInputBlock(clean.data(), zero.data(), 4);

    // Block 2: 3 samples; rail hit at position [1] within the block = absolute index 5.
    std::vector<float> b2l { 0.5f, 1.0f, 0.5f };
    std::vector<float> b2r(3, 0.0f);
    h.analyzeInputBlock(b2l.data(), b2r.data(), 3);

    // Block 3: 3 samples; rail hit at position [2] within the block = absolute index 9.
    std::vector<float> b3l { 0.5f, 0.5f, 1.0f };
    std::vector<float> b3r(3, 0.0f);
    h.analyzeInputBlock(b3l.data(), b3r.data(), 3);

    const auto s = h.clipStats();
    CHECK(s.firstClipIdx == 5);   // block 1 consumed 4 samples; hit at [1] in block 2 -> idx 4+1=5
    CHECK(s.lastClipIdx  == 9);   // hit at [2] in block 3 -> idx 4+3+2=9
}

TEST_CASE("ClipStats: firstClipIdx is -1 before any rail hit, stays at first hit thereafter") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);
    CHECK(h.clipStats().firstClipIdx == -1);

    std::vector<float> l { 0.5f, 1.0f, 0.5f };
    std::vector<float> r(3, 0.0f);
    h.analyzeInputBlock(l.data(), r.data(), 3);
    const long long first = h.clipStats().firstClipIdx;
    CHECK(first == 1);   // hit at position [1]

    // A subsequent block with another hit must NOT overwrite firstClipIdx.
    std::vector<float> l2 { 1.0f };
    std::vector<float> r2(1, 0.0f);
    h.analyzeInputBlock(l2.data(), r2.data(), 1);
    CHECK(h.clipStats().firstClipIdx == first);   // unchanged
    CHECK(h.clipStats().lastClipIdx  == 3);        // absolute index 3
}

TEST_CASE("ClipStats: clippedPct is zero before any hit and correct after") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);
    CHECK(h.clipStats().clippedPct() == 0.0f);

    // Feed 10 samples on L, 4 of which are at/above the rail.
    std::vector<float> l { 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f };
    std::vector<float> r(10, 0.0f);
    h.analyzeInputBlock(l.data(), r.data(), 10);

    const auto s = h.clipStats();
    CHECK(s.totalSamples == 10);
    // 4 positive-rail hits on L, 0 on R; percentage = 4/10 * 100 = 40.
    CHECK_THAT(s.clippedPct(), Catch::Matchers::WithinAbs(40.0f, 0.1f));
}

TEST_CASE("ClipStats: session peak accumulates across blocks and both channels") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);

    std::vector<float> l1(4, 0.3f);
    std::vector<float> r1(4, 0.5f);
    h.analyzeInputBlock(l1.data(), r1.data(), 4);

    std::vector<float> l2 { 0.8f, 0.2f };
    std::vector<float> r2 { 0.1f, 0.6f };
    h.analyzeInputBlock(l2.data(), r2.data(), 2);

    const auto s = h.clipStats();
    // Session peak on L should be 0.8 (highest single sample across both blocks).
    CHECK_THAT(s.sessionPeakL, Catch::Matchers::WithinAbs(0.8f, 2e-3f));
    // Session peak on R should be 0.6.
    CHECK_THAT(s.sessionPeakR, Catch::Matchers::WithinAbs(0.6f, 2e-3f));
}

TEST_CASE("ClipStats: peakLDb and headroomLDb are consistent with the linear peak") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);
    // Peak at -6 dBFS on L.
    const float peak6dB = 0.5012f;   // 20*log10(0.5012) ≈ -6.0 dBFS
    std::vector<float> l(8, peak6dB);
    std::vector<float> r(8, 0.1f);
    h.analyzeInputBlock(l.data(), r.data(), 8);

    const auto s = h.clipStats();
    CHECK_THAT(s.peakLDb(),     Catch::Matchers::WithinAbs(-6.0f, 0.1f));
    CHECK_THAT(s.headroomLDb(), Catch::Matchers::WithinAbs( 6.0f, 0.1f));
}

TEST_CASE("ClipStats: reset clears all statistics") {
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);

    std::vector<float> l { 1.0f, 1.0f, 1.0f };
    std::vector<float> r(3, 0.0f);
    h.analyzeInputBlock(l.data(), r.data(), 3);

    // Confirm stats are non-zero before reset.
    const auto before = h.clipStats();
    CHECK(before.posRailL > 0);
    CHECK(before.firstClipIdx != -1);

    h.reset();
    const auto after = h.clipStats();
    CHECK(after.posRailL    == 0);
    CHECK(after.negRailL    == 0);
    CHECK(after.posRailR    == 0);
    CHECK(after.negRailR    == 0);
    CHECK(after.firstClipIdx == -1);
    CHECK(after.lastClipIdx  == -1);
    CHECK(after.totalSamples == 0);
    CHECK(after.sessionPeakL == 0.0f);
    CHECK(after.sessionPeakR == 0.0f);
    CHECK(after.clippedPct() == 0.0f);
}

TEST_CASE("ClipStats: totalSamples counts both channels as one per pair") {
    // totalSamples is the per-channel count (not summed across channels), so a block of N frames
    // contributes N to totalSamples regardless of how many channels are active.
    eb::HealthMonitor h; h.prepare(eb::EarsModel::Ears, 4096);
    std::vector<float> l(7, 0.1f);
    std::vector<float> r(7, 0.2f);
    h.analyzeInputBlock(l.data(), r.data(), 7);
    CHECK(h.clipStats().totalSamples == 7);

    h.analyzeInputBlock(l.data(), r.data(), 7);
    CHECK(h.clipStats().totalSamples == 14);
}
```

- [ ] Run the failing tests to confirm they do not link / all fail:

```bash
./tools/dev.cmd cmake --build build --target eb_tests 2>&1 | tail -30
./tools/dev.cmd ctest --test-dir build -R "ClipStats" --output-on-failure 2>&1 | tail -30
```

- [ ] Commit the failing tests:

```
git add tests/test_healthmonitor.cpp
git commit -m "$(cat <<'EOF'
test(R4/R8): add failing Catch2 tests for ClipStats statistics

Covers positive/negative rail split, firstClipIdx/lastClipIdx absolute
positions, clippedPct(), sessionPeak, peakDb/headroomDb helpers, and
reset behaviour.  All fail until Task 3 implements the logic.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Implement `analyzeInputBlock` extensions and `clipStats()` in `HealthMonitor.cpp`

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/audio/HealthMonitor.cpp`

**Interfaces:**

Consumes:
- `HealthMonitor` private members added in Task 1: `totalSamples_`, `posRunL_`, `negRunL_`, `posRunR_`, `negRunR_`, `posRailL_`, `negRailL_`, `posRailR_`, `negRailR_`, `totalSamplesA_`, `firstClipIdx_`, `lastClipIdx_`, `sessionPeakLm_`, `sessionPeakRm_`
- Existing `analyzeInputBlock` body (`HealthMonitor.cpp` lines 70-104): the loop already tracks `pkL`, `pkR`, `railRunL_`, `railRunR_`, `longestRun_`, `confirmed`
- Existing `reset()` body (lines 31-44): must be extended with the new members

Produces:
- Extended `analyzeInputBlock` loop (RT-safe; plain locals + atomic stores; NO heap/lock/log/exception)
- `clipStats()` implementation
- Extended `reset()` clearing all new members

- - -

- [ ] Replace the `reset()` body to clear the new members (insert after `clipConfirmed_.store(false)` at line 42):

The new lines to add inside `reset()`, immediately before `driftRun.store(0)`:

```cpp
    // Plan 5: clip statistics reset
    totalSamples_  = 0;
    posRunL_  = negRunL_  = posRunR_  = negRunR_  = 0;
    posRailL_.store(0); negRailL_.store(0);
    posRailR_.store(0); negRailR_.store(0);
    totalSamplesA_.store(0);
    firstClipIdx_.store(-1);
    lastClipIdx_.store(-1);
    sessionPeakLm_.store(0);
    sessionPeakRm_.store(0);
```

- [ ] Rewrite `analyzeInputBlock` with the sign-split, first/last index, and session-peak extensions. The full replacement for `HealthMonitor.cpp` lines 70-104:

```cpp
void HealthMonitor::analyzeInputBlock (const float* l, const float* r, int n) noexcept {
    float pkL = 0.0f, pkR = 0.0f;
    int   railL = 0, railR = 0;
    int   posL = 0, negL = 0, posR = 0, negR = 0;
    bool  nonFinite = false, confirmed = false;
    long long firstIdx = -1, lastIdx = -1;   // -1 = no rail hit this block

    for (int i = 0; i < n; ++i) {
        const long long absIdx = totalSamples_ + static_cast<long long>(i);
        const float a = l[i], b = r[i];
        const bool fa = std::isfinite(a), fb = std::isfinite(b);
        if (!fa || !fb) nonFinite = true;

        if (fa) {
            const float ma = std::abs(a);
            if (ma > pkL) pkL = ma;
            if (ma >= kRailCeiling) {
                ++railL;
                if (a >= kRailCeiling)  ++posL;
                else                    ++negL;   // a <= -kRailCeiling
                if (firstIdx < 0) firstIdx = absIdx;
                lastIdx = absIdx;
            }
            railRunL_ = (ma >= kRailCeiling) ? railRunL_ + 1 : 0;
        } else {
            railRunL_ = 0;
        }

        if (fb) {
            const float mb = std::abs(b);
            if (mb > pkR) pkR = mb;
            if (mb >= kRailCeiling) {
                ++railR;
                if (b >= kRailCeiling)  ++posR;
                else                    ++negR;
                if (firstIdx < 0) firstIdx = absIdx;
                lastIdx = absIdx;
            }
            railRunR_ = (mb >= kRailCeiling) ? railRunR_ + 1 : 0;
        } else {
            railRunR_ = 0;
        }

        longestRun_ = juce::jmax(longestRun_, juce::jmax(railRunL_, railRunR_));
        if (railRunL_ >= kRailRunMin || railRunR_ >= kRailRunMin) confirmed = true;
    }

    // Publish sign-split rail counts.
    if (posL > 0) posRailL_.fetch_add(posL);
    if (negL > 0) negRailL_.fetch_add(negL);
    if (posR > 0) posRailR_.fetch_add(posR);
    if (negR > 0) negRailR_.fetch_add(negR);

    // Publish first/last clip index (first only written on the first block that has a hit).
    if (firstIdx >= 0) {
        // CAS to write firstClipIdx_ only if it is still -1 (not yet set this session).
        long long expected = -1LL;
        firstClipIdx_.compare_exchange_strong(expected, firstIdx);
        // lastClipIdx_ always advances (we hold the absolute index from this block).
        lastClipIdx_.store(lastIdx);
    }

    // Advance the total-sample counter (per-channel frame count, not summed across channels).
    totalSamples_ += static_cast<long long>(n);
    totalSamplesA_.store(totalSamples_);

    // Session peak (fixed-point * 1000, same convention as inLm / inRm).
    {
        const int newL = static_cast<int>(std::lround(juce::jlimit(0.0f, 8.0f, pkL) * 1000.0f));
        const int newR = static_cast<int>(std::lround(juce::jlimit(0.0f, 8.0f, pkR) * 1000.0f));
        // Relaxed CAS loop: only update if the new value is larger.
        int curL = sessionPeakLm_.load();
        while (newL > curL && !sessionPeakLm_.compare_exchange_weak(curL, newL)) {}
        int curR = sessionPeakRm_.load();
        while (newR > curR && !sessionPeakRm_.compare_exchange_weak(curR, newR)) {}
    }

    // Keep the existing publish paths.
    if (railL > 0) railSamplesL_.fetch_add(railL);
    if (railR > 0) railSamplesR_.fetch_add(railR);
    longestRunA_.store(longestRun_);
    if (nonFinite)  raise(HealthFlag::NonFinite);
    if (confirmed) { clipConfirmed_.store(true); raise(HealthFlag::ClipConfirmed); }

    // Guidance path (unchanged).
    reportInLevels(pkL, pkR, pkL >= kClipLinear, pkR >= kClipLinear);
}
```

RT-safety note: the CAS loops for session peak are bounded — they terminate as soon as `curL >= newL` or the exchange succeeds; no heap, no lock, no syscall. The `compare_exchange_weak` spinning is purely on registers.

- [ ] Add the `clipStats()` implementation after `HealthMonitor::levels()` (after line 160 in the current file):

```cpp
ClipStats HealthMonitor::clipStats() const noexcept {
    ClipStats s;
    s.posRailL     = posRailL_.load();
    s.negRailL     = negRailL_.load();
    s.posRailR     = posRailR_.load();
    s.negRailR     = negRailR_.load();
    s.firstClipIdx = firstClipIdx_.load();
    s.lastClipIdx  = lastClipIdx_.load();
    s.totalSamples = totalSamplesA_.load();
    s.sessionPeakL = sessionPeakLm_.load() / 1000.0f;
    s.sessionPeakR = sessionPeakRm_.load() / 1000.0f;
    return s;
}
```

- [ ] Run the tests and confirm all new cases pass, and the full suite stays green:

```bash
./tools/dev.cmd cmake --build build --target eb_tests 2>&1 | tail -20
./tools/dev.cmd ctest --test-dir build -R "ClipStats" --output-on-failure 2>&1
./tools/dev.cmd ctest --test-dir build --output-on-failure 2>&1 | tail -10
```

- [ ] Commit:

```
git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp
git commit -m "$(cat <<'EOF'
feat(R4/R8): extend analyzeInputBlock with full clip statistics

Tracks positive/negative rail-hit split per channel, absolute first/last
clip-sample index, per-session peak (fixed-point atomics), total-sample
count; exposes clipStats() snapshot. All new Catch2 tests pass; 98-test
suite stays green.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Forward `clipStats()` through `AudioEngine` and update `MainComponent`

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/audio/AudioEngine.h`
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/audio/AudioEngine.cpp`
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/gui/MainComponent.h`
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/src/gui/MainComponent.cpp`

**Interfaces:**

Consumes:
- `AudioEngine::hm` (the `HealthMonitor` member — confirmed in `AudioEngine.cpp` pattern: `hm.cleanCapture()`, `hm.flags()` etc.)
- Existing `AudioEngine` public API surface in `AudioEngine.h` lines 57-84 (the "Plan 4 additions" block)
- `MainComponent` members: `LevelMeter meterL { "L" }, meterR { "R" }` (MainComponent.h line 88); `levelsBounds` rectangle (MainComponent.cpp line 947); `timerCallback()` (lines 728-800)
- `resized()` levels card layout (MainComponent.cpp lines 947-965): `levelsBounds` is a 104 px tall `juce::Rectangle<int>`; the three meters each get `lv.getHeight() / 3` rows after an 8 px top gap = `(104 - 24 - 8) / 3 ≈ 24` px each, leaving no spare rows inside the card

Produces:
- `AudioEngine::clipStats() const noexcept -> ClipStats` forwarding accessor
- `MainComponent::headroomLabel` — a `juce::Label` that shows a one-line numeric peak/headroom summary, placed below the levels card (not inside it, to avoid disturbing the existing 104 px card height)
- `timerCallback()` update: each tick, read `engine.clipStats()` and `engine.levels()`, format the label text

- - -

- [ ] Add `clipStats()` to `AudioEngine.h` in the Plan-4-additions block (after `reachedGoodLevel()` at line 71):

```cpp
    ClipStats clipStats() const noexcept;
```

- [ ] Implement the forwarding in `AudioEngine.cpp` (add after the existing `reachedGoodLevel` body):

```cpp
ClipStats AudioEngine::clipStats() const noexcept { return hm.clipStats(); }
```

- [ ] Add `headroomLabel` to `MainComponent.h` in the Meters section (after line 91, the `inputClipHint` declaration):

```cpp
    juce::Label headroomLabel;   // numeric per-ear peak dBFS + headroom summary (R8)
```

- [ ] Construct and configure `headroomLabel` in `MainComponent::MainComponent()` (add after the `inputClipHint` initialisation block; pattern matches `levelsHint` configuration at MainComponent.cpp lines 69-75):

```cpp
    headroomLabel.setColour(juce::Label::textColourId, Theme::textDim());
    headroomLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    headroomLabel.setJustificationType(juce::Justification::centredLeft);
    headroomLabel.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(headroomLabel);
```

- [ ] Add layout for `headroomLabel` in `resized()`. The levels card ends at `levelsBounds.getBottom()`. Place the label in the 18 px strip immediately below it (before the `inputClipHint` layout block at line 962):

```cpp
        pp.removeFromTop(4);
        headroomLabel.setBounds(pp.removeFromTop(18));
```

  This goes directly after `meterOut.setBounds(...)` and before the `if (inputClipHint.isVisible())` block. The full patched section (replacing lines 956-965) reads:

```cpp
        lv.removeFromTop(8);
        const int mh = lv.getHeight() / 3;
        meterL.setBounds   (lv.removeFromTop(mh));
        meterR.setBounds   (lv.removeFromTop(mh));
        meterOut.setBounds (lv.removeFromTop(mh));

        pp.removeFromTop(4);
        headroomLabel.setBounds(pp.removeFromTop(18));

        if (inputClipHint.isVisible()) {
            pp.removeFromTop(8);
            inputClipHint.setBounds(pp.removeFromTop(34));
        }
```

- [ ] Update `timerCallback()` to populate `headroomLabel` each 30 Hz tick. Add the following block immediately after the `meterOut.setLevel(...)` call (MainComponent.cpp line 742), before the `silentTicks_` update:

```cpp
    // Numeric peak / headroom readout (R8): per-ear session peak in dBFS and remaining headroom.
    // Only shown while running (stats are session-scoped and cleared on Stop).
    if (engine.status() == EngineStatus::Running) {
        const auto cs = engine.clipStats();
        if (cs.totalSamples > 0) {
            // Format: "L peak −X dB  (+Y dB headroom)  |  R peak −X dB  (+Y dB headroom)"
            const int pLi = juce::roundToInt(-cs.peakLDb());   // positive integer = distance below 0 dBFS
            const int pRi = juce::roundToInt(-cs.peakRDb());
            const int hLi = juce::roundToInt(cs.headroomLDb());
            const int hRi = juce::roundToInt(cs.headroomRDb());
            juce::String txt = "L peak \xe2\x88\x92" + juce::String(pLi) + " dB"   // U+2212 minus sign
                             + "  (+" + juce::String(hLi) + " dB hdroom)"
                             + "   R peak \xe2\x88\x92" + juce::String(pRi) + " dB"
                             + "  (+" + juce::String(hRi) + " dB hdroom)";
            headroomLabel.setText(txt, juce::dontSendNotification);
            headroomLabel.setColour(juce::Label::textColourId, Theme::textDim());
        } else {
            headroomLabel.setText({}, juce::dontSendNotification);
        }
    } else {
        headroomLabel.setText({}, juce::dontSendNotification);
    }
```

  Note: `\xe2\x88\x92` is the UTF-8 encoding of the Unicode minus sign (U+2212), matching the typographic style in the existing readout label ("−X dB"). If the compiler or string literal encoding causes issues, replace with a plain ASCII hyphen-minus `-`.

- [ ] Build the full target (not just tests — this confirms the GUI compilation unit):

```bash
./tools/dev.cmd cmake --build build 2>&1 | tail -20
```

- [ ] Run the full test suite to confirm nothing regressed:

```bash
./tools/dev.cmd ctest --test-dir build --output-on-failure 2>&1 | tail -10
```

- [ ] Commit:

```
git add src/audio/AudioEngine.h src/audio/AudioEngine.cpp src/gui/MainComponent.h src/gui/MainComponent.cpp
git commit -m "$(cat <<'EOF'
feat(R8): surface numeric peak/headroom readout in the GUI levels card

Adds headroomLabel below the three level meters, updated each 30 Hz
timer tick from engine.clipStats().  Shows L/R session peak in dBFS and
remaining headroom so the user has an objective numeric reference
alongside the visual meters. AudioEngine forwards clipStats() from hm.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Add `ClipStats` accessors to `test_clipstatus.cpp` and verify the audit closure

**Files:**
- `C:/Users/Shaya/OneDrive/Documents/EARS_program/tests/test_clipstatus.cpp`

**Interfaces:**

Consumes:
- `eb::invalidMeasurementMessage` (ClipStatus.h line 8)
- `eb::ClipStats` helpers: `clippedPct()`, `peakLDb()`, `headroomLDb()`

Produces: Two additional test cases that (a) confirm `ClipStats` derived helpers are numerically correct at boundaries and (b) confirm that `invalidMeasurementMessage` wording is unchanged (regression guard for the first-slice messages that must survive this plan).

- - -

- [ ] Append to `tests/test_clipstatus.cpp`:

```cpp
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio/EngineTypes.h"   // ClipStats
#include <cmath>

TEST_CASE("ClipStats: boundary cases for derived helpers") {
    // Zero-samples state: all helpers return safe defaults.
    eb::ClipStats empty;
    CHECK(empty.clippedPct() == 0.0f);
    CHECK(empty.peakLDb()    == -144.0f);
    CHECK(empty.peakRDb()    == -144.0f);
    CHECK(empty.headroomLDb() == 144.0f);

    // 0 dBFS peak: headroom = 0.
    eb::ClipStats full;
    full.sessionPeakL = 1.0f;
    full.sessionPeakR = 1.0f;
    full.totalSamples = 10;
    CHECK_THAT(full.peakLDb(),     Catch::Matchers::WithinAbs(  0.0f, 0.05f));
    CHECK_THAT(full.headroomLDb(), Catch::Matchers::WithinAbs(  0.0f, 0.05f));

    // −3 dBFS peak: headroom = 3.
    eb::ClipStats mid;
    mid.sessionPeakL = std::pow(10.0f, -3.0f / 20.0f);
    mid.totalSamples = 100;
    mid.posRailL     = 5;
    CHECK_THAT(mid.peakLDb(),     Catch::Matchers::WithinAbs(-3.0f, 0.05f));
    CHECK_THAT(mid.headroomLDb(), Catch::Matchers::WithinAbs( 3.0f, 0.05f));
    CHECK_THAT(mid.clippedPct(),  Catch::Matchers::WithinAbs( 5.0f, 0.05f));
}

TEST_CASE("invalidMeasurementMessage wording is unchanged (regression guard)") {
    using eb::HealthFlag; using eb::invalidMeasurementMessage;
    // These strings are part of the first-slice D1/D3/D4 fix; must not change accidentally.
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::ClipConfirmed))
          == "Input reached digital full scale - this measurement is invalid. Lower the level and repeat.");
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::NonFinite))
          == "Measurement invalidated by a corrupted audio sample.");
    CHECK(std::string(invalidMeasurementMessage(HealthFlag::Dropout))
          == "Dropouts detected - this measurement is invalid.");
}
```

- [ ] Run the tests to confirm they pass and the full suite is green:

```bash
./tools/dev.cmd cmake --build build --target eb_tests 2>&1 | tail -10
./tools/dev.cmd ctest --test-dir build --output-on-failure 2>&1 | tail -10
```

The expected final count is 98 + new test cases (the 7 ClipStats cases from Task 2 + 2 from this task = 107 tests minimum, depending on Catch2 section counting).

- [ ] Commit:

```
git add tests/test_clipstatus.cpp
git commit -m "$(cat <<'EOF'
test(R4/R8): boundary-case tests for ClipStats helpers + regression guard

Verifies clippedPct/peakLDb/headroomLDb at 0, -3, 0 dBFS boundaries;
confirms invalidMeasurementMessage wording is unchanged from the first
slice so accidental edits are caught.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

### Audit-finding coverage

| Finding | Requirement | Covered by |
|---|---|---|
| R4 | Positive vs negative rail-hit split | Task 1 (`ClipStats.posRailL/negRailL/posRailR/negRailR`), Task 3 (sign detection in loop), Task 2 (test) |
| R4 | First/last clip-sample position (absolute index) | Task 1 (`firstClipIdx_`, `lastClipIdx_` atomics), Task 3 (CAS-based write, lastIdx always advances), Task 2 (position tests) |
| R4 | Clipped-sample percentage | Task 1 (`ClipStats::clippedPct()` derived helper from `totalSamples` denominator), Task 3 (per-block accumulation), Task 2 (percentage test) |
| R8 | Numeric per-channel peak dBFS | Task 1 (`ClipStats::peakLDb()/peakRDb()`), Task 3 (session-peak CAS loop), Task 4 (GUI label) |
| R8 | Remaining headroom numeric readout in GUI | Task 4 (`headroomLabel` + `timerCallback` update, shows `headroomLDb()` per ear) |

### Placeholder scan

No "TBD", "TODO", "add error handling", "handle edge cases", or "similar to" phrases appear in the code blocks. Every code snippet uses exact struct-field names, exact file paths, and exact line references where cited.

### RT-safety audit

All code in `analyzeInputBlock` additions uses:
- Plain local integers (`posL`, `negL`, `firstIdx`, `lastIdx`) accumulated in the loop — no heap.
- `juce::jmax`, `std::abs`, `std::isfinite` — branchless intrinsics, no allocation.
- Atomic `fetch_add` and the bounded CAS loops for session peak at the end of the function (outside the sample loop), not inside it. The CAS loop terminates immediately if `newL <= curL` (the common case after the first few blocks).
- No `std::log10` calls on the audio thread — `peakLDb()` and helpers are called only in `timerCallback()` on the GUI thread.

### Type consistency

- Session peak stored as `int` (peak * 1000) — matches the existing `inLm`/`inRm`/`outM` fixed-point convention in `HealthMonitor` (HealthMonitor.h lines 89-90, HealthMonitor.cpp lines 107-108).
- `firstClipIdx_` / `lastClipIdx_` are `std::atomic<long long>` — consistent with `droppedA` (`std::atomic<long long>`, HealthMonitor.h line 93) for large frame counts.
- `ClipStats` public fields use plain `int` and `float` — copyable by value, no atomics, safe for GUI-thread snapshots.
- `clippedPct()` returns `float` (0–100 range) for easy comparison with `0.0f`; `peakLDb()` / `headroomLDb()` return `float` in dBFS.
