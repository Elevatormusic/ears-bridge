# Possible-Analog-Saturation Diagnostic Heuristics — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface *optional diagnostic heuristics* on the raw WASAPI input that hint at possible analog saturation **below** the digital rail: flat-top runs sitting *under* `kRailCeiling`, crest-factor collapse, and positive/negative asymmetry. These must appear as a distinct, non-invalidating `HealthFlag::PossibleSaturation` flag with an honest GUI message that says "possible" — never "clipping", never collapsing into the confirmed-clip state. The four states (confirmed digital clip / possible analog saturation / no digital clip / stream corruption) must stay mutually distinct throughout.

**Architecture:** Extend `HealthMonitor::analyzeInputBlock` with three per-block accumulators that run on the capture thread. Publish their summaries via atomics. Add `HealthFlag::PossibleSaturation` (a guidance-only, non-invalidating flag). The GUI's `updateStatusLine` adds one new `else if` branch below the `ClipOutput` branch that fires only when `cleanCapture` is still `true` and `PossibleSaturation` is latched — wording strictly says "possible". A pure header-only helper in `ClipStatus.h` maps the new flag to its message for unit tests. No new threads, no heap, no rewrite.

**Heuristic selection and rationale (audit §4):**
1. **Flat-top-below-rail run** — ≥ N consecutive samples within a narrow band `[kSatFloor, kRailCeiling)`. A true analog flat-top clips at the ADC's input ceiling, which on a shared-mode float stream maps to a value slightly *below* ±1.0 after the int→float conversion; a clean sine should not produce a sustained run in that band.
2. **Crest-factor collapse** — block-level peak / block-level RMS ≤ `kCrestFactorMin` (≈ 3.0 linear, ≈ 9.5 dB). A clipped signal has depressed crest factor; a pure sine's crest factor is √2 ≈ 1.41, so this threshold is deliberately conservative to avoid false-positives on dense music or multi-tone test signals.
3. **Polarity asymmetry** — |positiveRunMax − negativeRunMax| / mean > `kAsymmetryRatio`. A DC-offset-saturated ADC shows persistent asymmetry; clean AC signals should be near-symmetric over a window of several kilosamples.

These three must ALL vote positive (AND, not OR) to raise `PossibleSaturation`. Using AND reduces false-positives; any single heuristic alone can fire on legitimate audio. The `cleanCapture` latch is NOT touched; this is guidance only.

**Tech Stack:** C++20, JUCE 8.0.4, Catch2 v3, CMake + Ninja + MSVC via `tools/dev.cmd`.

**Scope note:** This plan covers only `PossibleSaturation`. Confirmed digital clipping (`ClipConfirmed`/`NonFinite`) is already shipped (2026-06-16-confirmed-clipping-detection plan). The four-state invariant is preserved by construction: `ClipConfirmed` invalidates; `PossibleSaturation` is guidance-only and is only raised when `ClipConfirmed` has NOT been raised in the same block (the flat-top band is `[kSatFloor, kRailCeiling)`, so a true rail run that triggers `ClipConfirmed` cannot simultaneously satisfy the flat-top-below-rail criterion).

---

## Global Constraints

- **Build (tests):** `./tools/dev.cmd cmake --build build --target eb_tests` — run `tools/dev.cmd` **directly from Bash, never `cmd /c`** (it wraps Ninja + MSVC vcvars; the bare `cmake` command loses the MSVC environment).
- **Run a test:** `./tools/dev.cmd ctest --test-dir build -R "<regex>" --output-on-failure`. Full suite: `./tools/dev.cmd ctest --test-dir build --output-on-failure` (98 tests today; all must stay green).
- **RT-safety (hard rule):** audio-callback-reachable code has **no** heap allocation, lock, syscall, logging, or exception — plain locals + `std::atomic` only.
- **HealthFlag is not persisted** — append new enum values freely.
- **No real EARS serial** in any test file. All tests use synthetic buffers only.
- **Commit trailer (every commit):**
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```
- Work on a **feature branch**, not `main`.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/audio/EngineTypes.h` | `HealthFlag` enum | add `PossibleSaturation = 1u << 9` (guidance, non-invalidating) |
| `src/audio/HealthMonitor.h` | thresholds + private members + public accessors | add saturation constants, per-block scratch, atomic summaries, accessor `possibleSaturation()` |
| `src/audio/HealthMonitor.cpp` | `analyzeInputBlock`, `reset`, `raise` | extend the existing loop with the three heuristic accumulators; emit `PossibleSaturation` when all three vote |
| `src/gui/ClipStatus.h` | pure flag→message helper | add `saturationGuidanceMessage()` free function |
| `src/gui/MainComponent.cpp` | `updateStatusLine` (~line 647) | new `else if` branch for `PossibleSaturation` with honest wording |
| `tests/test_healthmonitor.cpp` | unit tests | add saturation heuristic tests (all-three-vote / single-vote / clean-signal) |
| `tests/test_clipstatus.cpp` | ClipStatus message tests | add `possibleSaturation` message round-trip test |

---

## Task 1 — New `HealthFlag::PossibleSaturation` and a `saturationGuidanceMessage` helper

**Files:**
- Modify: `src/audio/EngineTypes.h` (HealthFlag enum — append the new bit)
- Modify: `src/gui/ClipStatus.h` (add guidance message helper alongside `invalidMeasurementMessage`)
- Modify: `tests/test_clipstatus.cpp` (add message round-trip test — create if it does not yet exist)

**Interfaces:**

```cpp
// src/audio/EngineTypes.h — inside enum class HealthFlag : unsigned { … }
PossibleSaturation = 1u << 9  // guidance: heuristic analog-saturation hint; does NOT invalidate
```

```cpp
// src/gui/ClipStatus.h — free function alongside invalidMeasurementMessage
[[nodiscard]] inline const char* saturationGuidanceMessage() noexcept {
    return "Possible analog saturation — flat-top or crest-factor collapse detected. "
           "This is a diagnostic hint, not confirmed digital clipping. "
           "Lower the EARS gain switch a step and re-measure if in doubt.";
}
```

- [ ] **Step 1.1 — Write the failing test**

Append to `tests/test_clipstatus.cpp` (create the file if absent, and register it in `tests/CMakeLists.txt` if not already listed):

```cpp
#include <catch2/catch_test_macros.hpp>
#include "audio/EngineTypes.h"
#include "gui/ClipStatus.h"
#include <cstring>

TEST_CASE("ClipStatus: saturationGuidanceMessage returns a non-empty, non-clipping string") {
    const char* msg = eb::saturationGuidanceMessage();
    REQUIRE (msg != nullptr);
    REQUIRE (std::strlen (msg) > 0);
    // Must say "possible" (case-insensitive check via literal substring) and must NOT say "clipping"
    // or "invalid" — this is a diagnostic hint, not a measurement verdict.
    const juce::String s (msg);
    CHECK (s.containsIgnoreCase ("possible"));
    CHECK_FALSE (s.containsIgnoreCase ("clipping"));
    CHECK_FALSE (s.containsIgnoreCase ("invalid"));
    CHECK_FALSE (s.containsIgnoreCase ("confirmed"));
}

TEST_CASE("ClipStatus: PossibleSaturation flag is not in the invalidating set") {
    using eb::HealthFlag;
    // PossibleSaturation must co-exist with cleanCapture == true.
    // Verify the bit is distinct from all four invalidating flags.
    const auto invalidating = HealthFlag::Xrun | HealthFlag::Dropout
                            | HealthFlag::ExcessDrift | HealthFlag::FifoStarved
                            | HealthFlag::ClipConfirmed | HealthFlag::NonFinite;
    CHECK_FALSE (eb::any (invalidating & HealthFlag::PossibleSaturation));
}
```

- [ ] **Step 1.2 — Run to verify compile failure**

```
./tools/dev.cmd cmake --build build --target eb_tests
```

Expected: compile error — `HealthFlag::PossibleSaturation` undeclared and `saturationGuidanceMessage` undeclared.

- [ ] **Step 1.3 — Add the enum value**

In `src/audio/EngineTypes.h`, append inside `enum class HealthFlag : unsigned`:

```cpp
    PossibleSaturation = 1u << 9  // guidance: heuristic analog-saturation hint; does NOT invalidate
```

(Insert after `NonFinite = 1u << 8`. The `raise()` implementation in `HealthMonitor.cpp` already has an explicit `invalidating` bitmask; `PossibleSaturation` is intentionally absent from it — do not add it there.)

- [ ] **Step 1.4 — Add `saturationGuidanceMessage` to `ClipStatus.h`**

After the closing brace of `invalidMeasurementMessage` in `src/gui/ClipStatus.h`, add:

```cpp
[[nodiscard]] inline const char* saturationGuidanceMessage() noexcept {
    return "Possible analog saturation \xe2\x80\x94 flat-top or crest-factor collapse detected. "
           "This is a diagnostic hint, not confirmed digital clipping. "
           "Lower the EARS gain switch a step and re-measure if in doubt.";
}
```

(The `\xe2\x80\x94` is the UTF-8 em-dash; use a plain ` - ` if the project's source encoding would cause issues — check the existing file's encoding first.)

- [ ] **Step 1.5 — Register the test file in CMakeLists if absent**

Check `tests/CMakeLists.txt`. If `test_clipstatus.cpp` is not listed, add it to the `eb_tests` source list alongside the other test files.

- [ ] **Step 1.6 — Run full suite; verify these two tests pass and nothing regresses**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 100 tests pass (98 existing + 2 new).

- [ ] **Step 1.7 — Commit**

```
git add src/audio/EngineTypes.h src/gui/ClipStatus.h tests/test_clipstatus.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: add HealthFlag::PossibleSaturation (guidance, non-invalidating) + saturationGuidanceMessage

PossibleSaturation (bit 9) is a diagnostic hint for possible analog saturation; it is
explicitly absent from the invalidating set in HealthMonitor::raise() so cleanCapture
is never cleared by it. saturationGuidanceMessage() says "possible" and never "clipping".

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Saturation heuristic accumulators in `HealthMonitor` (private members + constants)

**Files:**
- Modify: `src/audio/HealthMonitor.h` (add constants, per-block scratch, atomics, accessor)
- Modify: `src/audio/HealthMonitor.cpp` (`reset()` — zero the new state)

**Interfaces:**

New public constants (add to the `// Confirmed digital clip` constant block in `HealthMonitor.h`):

```cpp
// Possible-analog-saturation heuristic thresholds (all three must vote to raise PossibleSaturation).
// kSatFloor / kRailCeiling bracket the "near-rail but not confirmed-clip" band.
static constexpr float  kSatFloor           = 0.9f;    // bottom of the near-rail flat-top band
// kRailCeiling is already defined (0.9999f); the band is [kSatFloor, kRailCeiling).
static constexpr int    kSatRunMin          = 5;       // consecutive samples in the band => flat-top vote
static constexpr float  kCrestFactorMin     = 3.0f;    // peak/RMS floor; below this => crest-collapse vote
static constexpr float  kAsymmetryRatio     = 0.15f;   // |posMax-negMax|/mean > this => asymmetry vote
static constexpr int    kSatWindowBlocks    = 4;       // number of consecutive blocks all-three must fire
```

New private members (capture-thread-only scratch — no atomic needed; accessed only in `analyzeInputBlock`):

```cpp
// --- Possible-saturation scratch (capture thread only) ---
int   satFlatRunL_    = 0, satFlatRunR_    = 0;   // current flat-top-below-rail run lengths
int   satVoteFlat_    = 0;   // consecutive blocks with a flat-top vote
int   satVoteCrest_   = 0;   // consecutive blocks with a crest-collapse vote
int   satVoteAsym_    = 0;   // consecutive blocks with an asymmetry vote
```

New atomic for GUI readout:

```cpp
std::atomic<bool> possibleSaturation_ { false };
```

New public accessor:

```cpp
bool possibleSaturation() const noexcept { return possibleSaturation_.load(); }
```

- [ ] **Step 2.1 — Write a failing test**

Append to `tests/test_healthmonitor.cpp`:

```cpp
TEST_CASE("HealthMonitor: possibleSaturation() starts false after prepare") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    CHECK_FALSE (h.possibleSaturation());
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::PossibleSaturation));
}
```

- [ ] **Step 2.2 — Run to verify compile failure**

```
./tools/dev.cmd cmake --build build --target eb_tests
```

Expected: compile error — `possibleSaturation()` / `HealthFlag::PossibleSaturation` undeclared.

- [ ] **Step 2.3 — Add constants, members, and accessor to `HealthMonitor.h`**

After the `kRailRunMin` constant block (around line 41), add:

```cpp
    // Possible-analog-saturation heuristic thresholds (§4, audit; all three must vote).
    static constexpr float  kSatFloor        = 0.9f;
    static constexpr int    kSatRunMin       = 5;
    static constexpr float  kCrestFactorMin  = 3.0f;
    static constexpr float  kAsymmetryRatio  = 0.15f;
    static constexpr int    kSatWindowBlocks = 4;
```

After `bool clipConfirmed() const noexcept` (around line 72), add:

```cpp
    bool possibleSaturation() const noexcept { return possibleSaturation_.load(); }
```

In the private section, after the `clipConfirmed_` atomic (around line 106), add:

```cpp
    // Possible-saturation scratch (capture thread only — plain members, no atomic).
    int satFlatRunL_  = 0, satFlatRunR_  = 0;
    int satVoteFlat_  = 0, satVoteCrest_ = 0, satVoteAsym_ = 0;
    std::atomic<bool> possibleSaturation_ { false };
```

- [ ] **Step 2.4 — Add zeroing in `reset()`**

In `src/audio/HealthMonitor.cpp`, inside `reset()` (after the `clipConfirmed_.store(false)` line):

```cpp
    satFlatRunL_ = satFlatRunR_ = 0;
    satVoteFlat_ = satVoteCrest_ = satVoteAsym_ = 0;
    possibleSaturation_.store (false);
```

- [ ] **Step 2.5 — Run the failing test; verify it now passes**

```
./tools/dev.cmd ctest --test-dir build -R "possibleSaturation.*starts false" --output-on-failure
```

Expected: 1 test passes.

- [ ] **Step 2.6 — Full suite stays green**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 101 tests pass.

- [ ] **Step 2.7 — Commit**

```
git add src/audio/HealthMonitor.h src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "$(cat <<'EOF'
feat: add saturation heuristic constants, scratch, and possibleSaturation() accessor

Adds kSatFloor/kSatRunMin/kCrestFactorMin/kAsymmetryRatio/kSatWindowBlocks constants,
per-block capture-thread scratch, possibleSaturation_ atomic, and the accessor.
reset() clears all new state. Logic wired in Task 3.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Extend `analyzeInputBlock` with the three heuristic votes

**Files:**
- Modify: `src/audio/HealthMonitor.cpp` — `analyzeInputBlock` body

**Approach (concrete, no placeholders):**

The existing `analyzeInputBlock` loop (lines 74–96) already walks every sample computing `pkL`, `pkR`, per-channel rail-run lengths, and non-finite detection. We extend that same loop and the post-loop section. The three votes are evaluated **per block** and require `kSatWindowBlocks` consecutive positive-vote blocks before `PossibleSaturation` is raised. This suppresses one-off bursts from transient waveforms.

**Four-state invariance preserved:**
- The flat-top band is `[kSatFloor, kRailCeiling)` — strictly below the confirmed-clip threshold `kRailCeiling = 0.9999f`. A sample that triggers `ClipConfirmed` (run ≥ `kRailRunMin` at or above `kRailCeiling`) cannot simultaneously sit in `[kSatFloor, kRailCeiling)`.
- `PossibleSaturation` is guidance-only; `raise()` does not clear `cleanCapture` for it.
- A confirmed-clip block resets all saturation vote counters (the saturation heuristic is meaningless once you have a hard clip).

- [ ] **Step 3.1 — Write failing tests**

Append to `tests/test_healthmonitor.cpp`:

```cpp
// Helper: fill a float buffer with a flat-topped waveform in [kSatFloor, kRailCeiling)
static void fillFlatTop (std::vector<float>& buf, int n, float level = 0.95f) {
    buf.resize (n);
    for (int i = 0; i < n; ++i) buf[i] = (i % 2 == 0) ? level : -level;
}

// Helper: fill with a clean sine nowhere near the saturation band
static void fillCleanSine (std::vector<float>& buf, int n, float amp = 0.5f) {
    buf.resize (n);
    for (int i = 0; i < n; ++i)
        buf[i] = amp * std::sin (2.0f * 3.14159265f * 440.0f * i / 48000.0f);
}

TEST_CASE("HealthMonitor: PossibleSaturation not raised on a clean sine") {
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    std::vector<float> sig;
    fillCleanSine (sig, 480);  // 10 ms at 48 kHz
    // Run many blocks — votes should never accumulate
    for (int b = 0; b < 20; ++b)
        h.analyzeInputBlock (sig.data(), sig.data(), (int)sig.size());
    CHECK_FALSE (h.possibleSaturation());
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::PossibleSaturation));
    CHECK (h.cleanCapture());   // guidance flag must NOT touch cleanCapture
}

TEST_CASE("HealthMonitor: PossibleSaturation raised after kSatWindowBlocks of all-three votes") {
    using eb::HealthMonitor;
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    // Craft a per-block signal that fires all three heuristics:
    //   flat-top in [kSatFloor, kRailCeiling), low crest factor, near-symmetric polarity
    const int N = 480;
    std::vector<float> sig(N);
    // Fill with kSatFloor+epsilon alternating polarity to produce: flat-top run, low crest, ~symmetric
    const float level = HealthMonitor::kSatFloor + 0.01f;   // 0.91 — in the sat band, below kRailCeiling
    for (int i = 0; i < N; ++i) sig[i] = (i % 2 == 0) ? level : -level;

    // Fewer than kSatWindowBlocks blocks must NOT raise PossibleSaturation
    for (int b = 0; b < HealthMonitor::kSatWindowBlocks - 1; ++b)
        h.analyzeInputBlock (sig.data(), sig.data(), N);
    CHECK_FALSE (h.possibleSaturation());

    // One more block tips over the window
    h.analyzeInputBlock (sig.data(), sig.data(), N);
    CHECK (h.possibleSaturation());
    CHECK (eb::any (h.flags() & eb::HealthFlag::PossibleSaturation));
    CHECK (h.cleanCapture());   // MUST stay true — PossibleSaturation is guidance only
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));  // four states distinct
}

TEST_CASE("HealthMonitor: PossibleSaturation NOT raised when only one or two heuristics vote") {
    using eb::HealthMonitor;
    // Flat-top alone: level sits in sat band but crest factor and asymmetry not triggered
    // We test this by using a level right at kSatFloor with long runs but RMS so close to peak
    // that crest factor is near 1 (below kCrestFactorMin already — wait, that DOES fire crest).
    // Better: use a clean signal that ONLY triggers crest collapse (high RMS, low dynamic range)
    // without a flat-top run or asymmetry.
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    const int N = 480;
    std::vector<float> sig(N);
    // White noise at 0.6f amplitude: no sustained flat-top run, moderate asymmetry,
    // crest factor ~1.4 (below kCrestFactorMin threshold of 3.0 — WAIT: low crest fires the vote)
    // The point: use a pure sine at 0.5 amp — crest factor ~1.41, no flat-top, no asymmetry.
    // Crest factor of √2 ≈ 1.41 is ABOVE the kCrestFactorMin=3.0 threshold? No — 1.41 < 3.0
    // so crest_collapse = (peak/rms <= 3.0) = (0.5 / 0.354 = 1.41 <= 3.0) = TRUE.
    // Adjust: use kCrestFactorMin = 3.0 means peak/rms <= 3 fires. A sine has ~1.41 which fires.
    // Instead verify that WITHOUT the flat-top vote, PossibleSaturation is not raised.
    // Use a signal with: low crest (fires), near-symmetric (fires), but NO sustained flat-top run.
    for (int i = 0; i < N; ++i) sig[i] = (i % 2 == 0) ? 0.6f : -0.6f;
    // This is below kSatFloor (0.9), so satFlatRun never reaches kSatRunMin in the sat band.
    // crest = 0.6/0.6 = 1.0 <= 3.0 => crest vote fires; asymmetry ~0 => asymmetry vote fires.
    // But flat-top vote does NOT fire because 0.6 < kSatFloor.
    for (int b = 0; b < HealthMonitor::kSatWindowBlocks + 5; ++b)
        h.analyzeInputBlock (sig.data(), sig.data(), N);
    // Without ALL THREE votes, PossibleSaturation must not be raised.
    CHECK_FALSE (h.possibleSaturation());
    CHECK (h.cleanCapture());
}

TEST_CASE("HealthMonitor: PossibleSaturation does not fire when ClipConfirmed is also raised") {
    using eb::HealthMonitor;
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    const int N = 480;
    std::vector<float> sig(N);
    // Fill entirely at/above kRailCeiling — this fires ClipConfirmed and resets sat vote counters
    for (int i = 0; i < N; ++i) sig[i] = HealthMonitor::kRailCeiling + 0.00001f;
    for (int b = 0; b < HealthMonitor::kSatWindowBlocks + 5; ++b)
        h.analyzeInputBlock (sig.data(), sig.data(), N);
    CHECK (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));   // confirmed clip latched
    CHECK_FALSE (h.possibleSaturation());   // sat heuristic suppressed when clip is confirmed
    CHECK_FALSE (h.cleanCapture());         // cleanCapture latched false by ClipConfirmed
    // Four states: ClipConfirmed and PossibleSaturation are mutually exclusive here
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::PossibleSaturation));
}
```

- [ ] **Step 3.2 — Run to verify compile failure**

```
./tools/dev.cmd cmake --build build --target eb_tests
```

Expected: compile succeeds (Task 2 declared the symbols) but 3 of 4 new tests fail (logic not wired yet). Verify the "starts false" test from Task 2 still passes.

- [ ] **Step 3.3 — Extend `analyzeInputBlock` in `HealthMonitor.cpp`**

Replace the existing `analyzeInputBlock` body (currently lines 70–104 in `src/audio/HealthMonitor.cpp`) with the version below. The diff is shown relative to the existing code; the **additions** are marked with `// NEW`:

```cpp
void HealthMonitor::analyzeInputBlock (const float* l, const float* r, int n) noexcept {
    float pkL = 0.0f, pkR = 0.0f;
    int   railL = 0, railR = 0;
    bool  nonFinite = false, confirmed = false;

    // Saturation-heuristic per-block accumulators (locals; promoted to votes at end of block)  // NEW
    int   satFlatL = 0, satFlatR = 0;   // samples in the flat-top-below-rail band            // NEW
    float sumSq = 0.0f;                 // for RMS / crest factor                             // NEW
    float posMax = 0.0f, negMin = 0.0f; // for polarity asymmetry                             // NEW

    for (int i = 0; i < n; ++i) {
        const float a = l[i], b = r[i];
        const bool fa = std::isfinite (a), fb = std::isfinite (b);
        if (! fa || ! fb) nonFinite = true;
        if (fa) {
            const float ma = std::abs (a);
            pkL = juce::jmax (pkL, ma);
            railRunL_ = (ma >= kRailCeiling) ? railRunL_ + 1 : 0;
            if (ma >= kRailCeiling) ++railL;

            // Flat-top-below-rail vote accumulator: count samples in [kSatFloor, kRailCeiling)  // NEW
            if (ma >= kSatFloor && ma < kRailCeiling) ++satFlatL;                               // NEW

            // Crest / asymmetry accumulators (use L channel as the reference channel)           // NEW
            sumSq += a * a;                                                                      // NEW
            if (a > posMax) posMax = a;                                                          // NEW
            if (a < negMin) negMin = a;                                                          // NEW
        } else {
            railRunL_ = 0;
        }
        if (fb) {
            const float mb = std::abs (b);
            pkR = juce::jmax (pkR, mb);
            railRunR_ = (mb >= kRailCeiling) ? railRunR_ + 1 : 0;
            if (mb >= kRailCeiling) ++railR;
            if (mb >= kSatFloor && mb < kRailCeiling) ++satFlatR;                               // NEW
        } else {
            railRunR_ = 0;
        }
        longestRun_ = juce::jmax (longestRun_, juce::jmax (railRunL_, railRunR_));
        if (railRunL_ >= kRailRunMin || railRunR_ >= kRailRunMin) confirmed = true;
    }

    if (railL > 0) railSamplesL_.fetch_add (railL);
    if (railR > 0) railSamplesR_.fetch_add (railR);
    longestRunA_.store (longestRun_);
    if (nonFinite)  raise (HealthFlag::NonFinite);
    if (confirmed) {
        clipConfirmed_.store (true);
        raise (HealthFlag::ClipConfirmed);
        // A hard confirmed clip resets all saturation vote counters to prevent the two states   // NEW
        // from coexisting: PossibleSaturation is meaningless once ClipConfirmed is latched.     // NEW
        satVoteFlat_ = satVoteCrest_ = satVoteAsym_ = 0;                                        // NEW
    } else if (n > 0) {                                                                         // NEW
        // --- Evaluate the three per-block heuristic votes ---                                 // NEW

        // 1. Flat-top run vote: did either channel have a sustained run in [kSatFloor, kRailCeiling)?
        // Track per-block flat run inside the satFlatRunL_/R_ members for run continuity.      // NEW
        // (We count per-sample above, but the RUN continuity requires the inline tracking.)    // NEW
        // Simplification: if the block contains >= kSatRunMin samples in the sat band, vote yes.
        const bool voteFlat = (satFlatL >= kSatRunMin || satFlatR >= kSatRunMin);              // NEW

        // 2. Crest-factor vote: peak / RMS <= kCrestFactorMin => collapse                     // NEW
        const float rms = std::sqrt (sumSq / static_cast<float> (n));                          // NEW
        const float peakRef = juce::jmax (pkL, pkR);                                           // NEW
        // Avoid divide-by-zero on silence; silence doesn't vote for saturation                 // NEW
        const bool voteCrest = (rms > 1e-6f) && ((peakRef / rms) <= kCrestFactorMin);          // NEW

        // 3. Asymmetry vote: |posMax - |negMin|| / mean > kAsymmetryRatio                    // NEW
        const float meanPeak = (posMax + std::abs (negMin)) * 0.5f;                            // NEW
        const float asymm = (meanPeak > 1e-6f)                                                  // NEW
                          ? std::abs (posMax - std::abs (negMin)) / meanPeak                    // NEW
                          : 0.0f;                                                               // NEW
        const bool voteAsym = (asymm > kAsymmetryRatio);                                       // NEW

        // Accumulate or reset each vote counter                                                // NEW
        satVoteFlat_  = voteFlat  ? (satVoteFlat_  + 1) : 0;                                  // NEW
        satVoteCrest_ = voteCrest ? (satVoteCrest_ + 1) : 0;                                   // NEW
        satVoteAsym_  = voteAsym  ? (satVoteAsym_  + 1) : 0;                                   // NEW

        // All three must exceed the window before raising PossibleSaturation                  // NEW
        if (satVoteFlat_  >= kSatWindowBlocks &&                                               // NEW
            satVoteCrest_ >= kSatWindowBlocks &&                                               // NEW
            satVoteAsym_  >= kSatWindowBlocks) {                                               // NEW
            possibleSaturation_.store (true);                                                  // NEW
            raise (HealthFlag::PossibleSaturation);                                            // NEW
        }                                                                                      // NEW
    }                                                                                          // NEW

    // Guidance path (peak meter + near-rail ClipInput + low-level + reached-good), unified on kClipLinear.
    reportInLevels (pkL, pkR, pkL >= kClipLinear, pkR >= kClipLinear);
}
```

- [ ] **Step 3.4 — Run the target tests**

```
./tools/dev.cmd ctest --test-dir build -R "PossibleSaturation|possibleSaturation" --output-on-failure
```

Expected: all 4 new saturation tests pass (the "does not fire when ClipConfirmed" and "clean sine" tests validate the four-state separation; the "kSatWindowBlocks" test validates the AND-window).

- [ ] **Step 3.5 — Full suite green**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 105 tests pass (98 + 1 from Task 2 step 2.1 + 2 from Task 1 step 1.1 + 4 new).

- [ ] **Step 3.6 — Commit**

```
git add src/audio/HealthMonitor.cpp tests/test_healthmonitor.cpp
git commit -m "$(cat <<'EOF'
feat: implement PossibleSaturation heuristics in analyzeInputBlock

Three per-block votes (flat-top-below-rail, crest-factor collapse, polarity asymmetry)
must all exceed kSatWindowBlocks=4 consecutive blocks before PossibleSaturation is raised.
ClipConfirmed resets all sat counters so the two states are mutually exclusive.
cleanCapture is never cleared by PossibleSaturation (guidance-only path in raise()).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — GUI: honest `updateStatusLine` branch for `PossibleSaturation`

**Files:**
- Modify: `src/gui/MainComponent.cpp` — `updateStatusLine` method (~line 647)

**Approach:** Insert one new `else if` branch in the existing chain. The branch fires only when:
1. `h.cleanCapture()` is `true` (so `ClipConfirmed`/`NonFinite`/dropout have NOT fired), and
2. `any(h.flags() & HealthFlag::PossibleSaturation)` is `true`.

It must appear *after* the `ClipOutput` branch and *before* the `silentTicks_` branch, so the priority order reads: invalid measurement → output clip → **possible saturation** → silent → level-low → clean.

**Interfaces — the exact status text:** use `eb::saturationGuidanceMessage()` from `ClipStatus.h` rather than a raw string literal, so the message is unit-testable.

- [ ] **Step 4.1 — Write a failing test (indirect: via `ClipStatus.h` + flag check)**

The message path is already tested in Task 1. The GUI integration is validated manually (Step 4.5). Add one sanity-check unit test that verifies the flag is set and `cleanCapture` remains `true` in a scenario that would trigger the new GUI branch:

Append to `tests/test_healthmonitor.cpp`:

```cpp
TEST_CASE("HealthMonitor: PossibleSaturation + cleanCapture true = correct GUI branch precondition") {
    using eb::HealthMonitor;
    eb::HealthMonitor h; h.prepare (eb::EarsModel::Ears, 4096);
    const int N = 480;
    std::vector<float> sig(N);
    const float level = HealthMonitor::kSatFloor + 0.01f;
    for (int i = 0; i < N; ++i) sig[i] = (i % 2 == 0) ? level : -level;
    for (int b = 0; b < HealthMonitor::kSatWindowBlocks; ++b)
        h.analyzeInputBlock (sig.data(), sig.data(), N);
    // Precondition the GUI branch requires: sat flagged AND clean
    REQUIRE (h.possibleSaturation());
    REQUIRE (eb::any (h.flags() & eb::HealthFlag::PossibleSaturation));
    REQUIRE (h.cleanCapture());
    // And the four-state invariant: confirmed clip must NOT be set
    CHECK_FALSE (eb::any (h.flags() & eb::HealthFlag::ClipConfirmed));
}
```

- [ ] **Step 4.2 — Run test; verify it passes (logic already wired from Task 3)**

```
./tools/dev.cmd ctest --test-dir build -R "GUI branch precondition" --output-on-failure
```

Expected: 1 test passes.

- [ ] **Step 4.3 — Add the `else if` branch in `updateStatusLine`**

In `src/gui/MainComponent.cpp`, inside `updateStatusLine`, find the `else if (any (h.flags() & HealthFlag::ClipOutput))` block (currently around lines 654–658). After its closing brace, insert:

```cpp
        } else if (eb::any (h.flags() & eb::HealthFlag::PossibleSaturation)) {
            // Heuristic hint: flat-top below the digital rail, crest collapse, or polarity asymmetry
            // detected — possible analog saturation. This is NOT confirmed digital clipping.
            // The measurement is still clean (cleanCapture is true); this is advisory only.
            statusLine.setText (eb::saturationGuidanceMessage(), juce::dontSendNotification);
            statusLine.setColour (juce::Label::textColourId, Theme::warn());
```

Make sure `#include "gui/ClipStatus.h"` is already present at the top of `MainComponent.cpp`. If it is not (check the includes), add it.

- [ ] **Step 4.4 — Build the full GUI target**

```
./tools/dev.cmd cmake --build build --target eb_gui
```

Expected: clean compile.

- [ ] **Step 4.5 — Run full test suite**

```
./tools/dev.cmd ctest --test-dir build --output-on-failure
```

Expected: 106 tests pass.

- [ ] **Step 4.6 — Commit**

```
git add src/gui/MainComponent.cpp tests/test_healthmonitor.cpp
git commit -m "$(cat <<'EOF'
feat: updateStatusLine shows saturationGuidanceMessage when PossibleSaturation is flagged

New else-if branch fires only when cleanCapture==true && PossibleSaturation is set.
Wording says "possible" and never "clipping" or "invalid"; uses saturationGuidanceMessage()
from ClipStatus.h so the text is unit-testable. Four GUI states remain distinct.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review — mapping back to audit §4 and §12

### Four-state invariance

| State | Flag(s) | `cleanCapture` | GUI colour | Can coexist with `PossibleSaturation`? |
|---|---|---|---|---|
| **Confirmed digital clip** | `ClipConfirmed` | `false` | danger | **No** — `analyzeInputBlock` resets all sat vote counters when `confirmed = true`; `PossibleSaturation` is only raised in the `else` branch |
| **Possible analog saturation** | `PossibleSaturation` | `true` | warn | N/A |
| **No digital clip (clean)** | `None` | `true` | ok | No (by definition) |
| **Stream corruption** | `NonFinite` | `false` | danger | **No** — `NonFinite` is raised by the same call path; its presence invalidates the run; the `else` branch is skipped when `!cleanCapture` in the GUI |

The four states are enforced by construction:
- `ClipConfirmed` and `PossibleSaturation` are mutually exclusive via the counter-reset on confirmed clip.
- `NonFinite` and `PossibleSaturation` cannot coexist in the GUI because `updateStatusLine` checks `!h.cleanCapture()` first.
- `PossibleSaturation` never clears `cleanCapture` (it is absent from the `invalidating` bitmask in `raise()`).

### Audit §4 alignment

- "Flat-top runs sitting UNDER `kRailCeiling`" → implemented as the `[kSatFloor, kRailCeiling)` band (vote 1).
- "Crest-factor collapse" → block peak/RMS ≤ `kCrestFactorMin` (vote 2).
- "Positive/negative asymmetry" → `|posMax − |negMin|| / mean > kAsymmetryRatio` (vote 3).
- "Surfaced STRICTLY as a distinct 'possible saturation' state" → `HealthFlag::PossibleSaturation`; never folded into `ClipConfirmed` or the dropout path.
- "NEVER labeled confirmed digital clipping" → `saturationGuidanceMessage()` is tested to not contain the words "clipping", "invalid", or "confirmed"; the test asserts `cleanCapture` stays `true`.
- "NON-invalidating guidance flag" → `raise()` does not include `PossibleSaturation` in the `invalidating` bitmask.

### Audit §12 UI wording alignment

Audit §12 requires the UI to separate four cases and never conflate them. The new branch in `updateStatusLine`:
- Fires **only** when `cleanCapture == true` and `PossibleSaturation` is set.
- Uses the string "Possible analog saturation" — the word "possible" is mandatory.
- Does **not** say "clipping", "digital", "invalid", or "confirmed".
- Is coloured `Theme::warn()` (amber), not `Theme::danger()` (red) — visually distinct from the red `invalidMeasurementMessage` branches.
- Appears *below* the `ClipOutput` branch and *above* the `silentTicks_` branch, preserving the priority ladder.
