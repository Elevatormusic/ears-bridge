# IR-SNR headphone-correct metric — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Redefine the reference-monitor IR-quality metric so a clean *headphone* sweep reads positive IR-SNR (grades `GradedClean`) instead of the current negative reading that makes every clean measurement `GradedSuspect`.

**Architecture:** One pure header-only function (`evaluateIr` in `src/gui/IrQualityStatus.h`): replace the ±2-sample peak window + summed-energy tail with a **few-ms signal gate** (mean power) vs a **clean-region noise floor** (mean power), per-sample normalized, reusing the existing peak-finder + circular harmonic guard. Then pass `sampleRate` through the one caller (`RefMonitor::gradeMeasurement`).

**Tech Stack:** C++20, JUCE 8, Catch2 v3. Build/test: `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build`.

## Global Constraints
- **Pure DSP, header-only** in `IrQualityStatus.h`; GUIDANCE-only (never invalidates a capture).
- **Per-sample mean power** (Huszty & Sakamoto Eq. 2): each window's energy ÷ its OWN sample count.
- **Noise = the clean region excluding the wrapped harmonic spots**, NOT pre-arrival (which holds the wrapped ESS distortion + linear-phase pre-ring).
- Thresholds stay **PROVISIONAL / `kIrThresholdsRatified = false`** (INFO-only) — this build fixes the metric, not the on-device ratification.
- Commit attribution: `Elevatormusic` / `22101396+Elevatormusic@users.noreply.github.com`. **No Claude co-author trailer.**

---

### Task 1: Redefine `evaluateIr` (the pure metric) + its tests

**Files:**
- Modify: `src/gui/IrQualityStatus.h` (the constants + the `evaluateIr` body)
- Test: `tests/test_irquality.cpp` (append 2 new cases; the 3 existing cases pass unchanged)

**Interfaces — Produces:** `evaluateIr(const float* ir, int n, bool matched, float minIrSnrDb = kMinIrSnrDb, float maxThdPct = kMaxThdPct, const std::vector<int>& harmonicOffsetsSamples = {}, double sampleRate = 48000.0)` → `IrQuality` (unchanged struct); constants `kMinIrSnrDb = 6.0f`, `kSignalGateMs = 8.0`, `kPreMarginMs = 0.5`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_irquality.cpp` (it already includes `<random>`, `<cmath>`; `kHarm` is defined at the top):

```cpp
// A SPREAD headphone-like IR: energy decaying over ~5 ms (~240 samples @48k) from the peak, plus a
// quiet noise floor. The OLD +/-2-sample metric read this NEGATIVE (the spread response counted as
// "noise"); the redefinition reads it solidly positive. THIS is the headphone bug fix.
static std::vector<float> spreadHeadphoneIr (int n, int peakIdx = 64) {
    std::vector<float> ir ((size_t) n, 0.0f);
    for (int k = 0; k < 240 && peakIdx + k < n; ++k)              // ~5 ms decaying, oscillating (colored)
        ir[(size_t) (peakIdx + k)] = std::exp (-(float) k / 60.0f) * ((k & 1) ? -1.0f : 1.0f);
    std::mt19937 rng (3u);                                        // a quiet noise floor everywhere
    std::uniform_real_distribution<float> d (-1.0e-3f, 1.0e-3f);
    for (int i = 0; i < n; ++i) ir[(size_t) i] += d (rng);
    return ir;
}

TEST_CASE("evaluateIr: a SPREAD headphone IR grades GOOD (the headphone bug fix)") {
    const int n = 1 << 13;
    auto ir = spreadHeadphoneIr (n);
    auto q = evaluateIr (ir.data(), n, /*matched*/ true, kMinIrSnrDb, kMaxThdPct, kHarm, /*rate*/ 48000.0);
    CHECK (q.matched);
    CHECK (q.irSnrDb > kMinIrSnrDb);    // clean spread IR -> clearly positive (the OLD metric read NEGATIVE)
    CHECK_FALSE (q.lowQuality);
}

TEST_CASE("evaluateIr: per-sample normalization - lengthening the noise window doesn't move IR-SNR") {
    // Same signal + same noise DENSITY in two IRs of different length: IR-SNR must match (mean power,
    // not summed energy). The OLD summed-tail metric drifted ~3 dB per length doubling.
    auto make = [] (int n) {
        std::vector<float> ir ((size_t) n, 0.0f);
        ir[(size_t) 64] = 1.0f; ir[(size_t) 65] = 0.5f; ir[(size_t) 66] = 0.25f;   // a small spread signal
        std::mt19937 rng (7u);
        std::uniform_real_distribution<float> d (-1.0e-3f, 1.0e-3f);
        for (int i = 0; i < n; ++i) ir[(size_t) i] += d (rng);
        return ir;
    };
    auto a = make (1 << 12), b = make (1 << 13);    // 4096 vs 8192 - twice the noise window, same density
    auto qa = evaluateIr (a.data(), (int) a.size(), true, kMinIrSnrDb, kMaxThdPct, {}, 48000.0);
    auto qb = evaluateIr (b.data(), (int) b.size(), true, kMinIrSnrDb, kMaxThdPct, {}, 48000.0);
    CHECK (qb.irSnrDb == Catch::Approx (qa.irSnrDb).margin (1.5f));   // length-invariant
}
```

- [ ] **Step 2: Run to verify they fail** — `./tools/dev.cmd cmake --build build --target eb_tests` then `./tools/dev.cmd ctest --test-dir build`. Expected: the SPREAD case FAILS (old metric reads negative → `irSnrDb > kMinIrSnrDb` false + `lowQuality` true); the normalization case FAILS (old summed-tail drifts ~3 dB > the 1.5 margin).

- [ ] **Step 3: Redefine the metric** — in `src/gui/IrQualityStatus.h`:

  (a) Replace the constant `static constexpr float kMinIrSnrDb = 20.0f;` and add the gate constants:
```cpp
// PROVISIONAL — on-device ratification needed. Was 20 (the OLD +/-2-sample metric); the mean-power
// IR-SNR of a clean SPREAD headphone IR reads solidly positive, so 6 dB is a conservative clean floor.
static constexpr float kMinIrSnrDb = 6.0f;
// Signal-gate span (post-peak decay) + pre-margin (onset / short pre-ring), in ms. PROVISIONAL.
static constexpr double kSignalGateMs = 8.0;
static constexpr double kPreMarginMs  = 0.5;
```

  (b) Add `double sampleRate = 48000.0` as the LAST parameter of `evaluateIr` (after `harmonicOffsetsSamples`).

  (c) Replace the body from the "A short window around the main peak…" comment through the `q.thdPercent = …` line with:
```cpp
    // Signal = the IR's real span around the peak. A headphone IR is SPREAD over a few ms (not a single
    // tap), so the signal is a ms-based gate: a small pre-margin (onset / short pre-ring) + a post-peak
    // decay span. ms-based so it scales with rate; circular; clamped to n.
    const int signalSpan = juce::jlimit (1, n, (int) std::lround (kSignalGateMs * sampleRate / 1000.0));
    const int preMargin  = juce::jlimit (0, n, (int) std::lround (kPreMarginMs  * sampleRate / 1000.0));
    auto inSignalGate = [&] (int i) {
        int d = i - peakIdx;                          // shortest circular signed distance
        if (d >  n / 2) d -= n;
        if (d < -n / 2) d += n;
        return d >= -preMargin && d <= signalSpan;
    };

    // The harmonic spots wrap to (peakIdx - off + n) % n in the circular IR. Exclude them from BOTH the
    // signal AND the noise (distortion is neither) so they can't masquerade as noise and depress the SNR.
    constexpr int kHarmHalfWidth = 2;
    auto inAnyHarmonic = [&] (int i) {
        for (int off : harmonicOffsetsSamples) {
            const int c = ((peakIdx - off) % n + n) % n;   // circular wrap of the negative-time spot
            int d = std::abs (i - c);
            d = std::min (d, n - d);                       // circular distance
            if (d <= kHarmHalfWidth) return true;
        }
        return false;
    };

    // --- per-sample MEAN-power energies (signal / noise each divided by its OWN sample count) ---
    double signalEnergy = 0.0, noiseEnergy = 0.0, harmEnergy = 0.0;
    long   signalCount  = 0,   noiseCount  = 0;
    for (int i = 0; i < n; ++i) {
        const double e = (double) ir[i] * ir[i];
        if (inSignalGate (i))       { signalEnergy += e; ++signalCount; }
        else if (inAnyHarmonic (i))   harmEnergy   += e;   // distortion: not signal, not noise
        else                        { noiseEnergy  += e; ++noiseCount; }
    }

    const double tiny = 1.0e-20;
    const double signalMean = signalEnergy / (double) juce::jmax (1L, signalCount);
    const double noiseMean  = (noiseCount > 0) ? noiseEnergy / (double) noiseCount : tiny;
    q.irSnrDb    = 10.0f * (float) std::log10 ((signalMean + tiny) / (noiseMean + tiny));
    q.thdPercent = 100.0f * (float) std::sqrt (harmEnergy / (signalEnergy + tiny));
```
(Leave the `q.lowQuality = matched && (q.irSnrDb < minIrSnrDb || q.thdPercent > maxThdPct);` line and everything else unchanged. Update the doc-comment block lines 18-21 to describe the gate-vs-clean-region mean-power definition.)

- [ ] **Step 4: Run to verify all pass** — build + ctest. Expected: the 2 new cases pass; the 3 existing `test_irquality.cpp` cases still pass unchanged (clean sharp ≈59 dB > 30 ✓; noisy ≈0 dB < `kMinIrSnrDb` ✓; distorted THD ≈52 % > 5 ✓); full suite green except possibly `test_refmonitor.cpp`'s gradeMeasurement-clean case (Task 2). If that one fails, it's expected — Task 2 fixes it.

- [ ] **Step 5: Commit** — `git add src/gui/IrQualityStatus.h tests/test_irquality.cpp` then commit `fix(refmon): redefine IR-SNR for spread headphone IRs (gate + mean-power)`.

---

### Task 2: Pass `sampleRate` through the caller + update the gradeMeasurement test

**Files:**
- Modify: `src/audio/RefMonitor.h:152-153` (the `evaluateIr` call in `gradeMeasurement`)
- Test: `tests/test_refmonitor.cpp:169-185` (the gradeMeasurement clean→GradedClean case)

**Interfaces — Consumes:** `evaluateIr(..., harm, sampleRate)` from Task 1. `gradeMeasurement` already has `double sampleRate` (RefMonitor.h:132).

- [ ] **Step 1: Wire the rate** — in `src/audio/RefMonitor.h`, change the `evaluateIr` call (lines 152-153) to pass the rate it already has:
```cpp
    g.quality = evaluateIr (ir.data(), (int) ir.size(), /*matched*/ true,
                            minIrSnrDb, maxThdPct, harm, sampleRate);
```

- [ ] **Step 2: Run the gradeMeasurement test** — build + `./tools/dev.cmd ctest --test-dir build`. Read `tests/test_refmonitor.cpp:169-185` ("gradeMeasurement: a matching ESS + a clean IR -> GradedClean"). Expected: it still asserts `g.state == GradedClean` — the redefined metric reads a clean deconvolved IR *more* positively than before, so it clears the test's threshold. If it now FAILS (the synthetic IR's new value or the test's hard-coded `minIrSnrDb` arg no longer agree), proceed to Step 3; if it PASSES, skip to Step 4.

- [ ] **Step 3 (only if Step 2 failed): Re-fit the test** — in `tests/test_refmonitor.cpp`, the case passes a `minIrSnrDb` argument to `gradeMeasurement` chosen for the old ~13 dB value. Print `g.quality.irSnrDb` (add a temporary `WARN(g.quality.irSnrDb);`), read the new value, set the test's `minIrSnrDb` arg a few dB below it so a clean IR clears it, and update the stale `// A real deconvolved ESS clears ~13 dB IR-SNR …` comment (line 170) to the new measured value. Remove the temporary `WARN`. Re-run → `GradedClean`.

- [ ] **Step 4: Full suite green** — `./tools/dev.cmd ctest --test-dir build`. Expected: `100% tests passed`.

- [ ] **Step 5: Commit** — `git add src/audio/RefMonitor.h tests/test_refmonitor.cpp` then commit `fix(refmon): pass sampleRate to evaluateIr; refit the gradeMeasurement-clean test`.

---

## Notes
- After both tasks: full suite green, then a fresh-Opus `code-verifier` over the diff (the redefined metric + the circular-gate math are the things to scrutinize), then `finishing-a-development-branch`.
- **On-device re-validation owed** (not in this build): re-run a real measurement → confirm a clean sweep now grades `GradedClean`; then ratify `kSignalGateMs` / `kPreMarginMs` / `kMinIrSnrDb` and flip `kIrThresholdsRatified`.
- Linear-phase cal FIR pre-ring is a noted follow-up (the asymmetric gate may count some as noise; INFO-only meanwhile).
