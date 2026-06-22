# Noise-Floor Detection Primitive — Design Spec

**Status:** Brainstorm complete 2026-06-22 (hybrid-auto capture chosen by the user). Foundation —
built first; consumed by AutoPerEar (#26) and the per-room/SPL calibration feature (#27).
**Research-validated (`validate-ears-ideas`, high confidence):** the architecture (baseline +
gap-refinement + robust-low) matches the acoustic-measurement literature; the statistic and baseline
duration are refined below to the AES-2id / Prawda-et-al. precedents. Per-band (octave) SNR — which
the literature wants because the *deconvolved* floor rises with frequency — is a downstream **grade**
concern (calibration #27 / the reference-monitor), NOT this primitive: #1 provides the broadband
per-channel **ambient** floor for the gap threshold + a coarse SNR.

**Goal:** Provide a **measured per-channel ambient/amp-hiss noise floor** so that SNR, the AutoPerEar
gap threshold, and calibration all read one accurate source — replacing today's fixed `armFloor`
assumption in `HealthMonitor`.

**Architecture:** A new pure, real-time-safe `NoiseFloorTracker`. `HealthMonitor` (which already
computes per-channel block level) feeds it each block and reads back the floor for its SNR verdict.
The floor is captured **amp-on, no sweep** — the only condition where the amp hiss that actually
limits SNR is exposed.

**Tech stack:** C++20, JUCE 8, audio-thread (lock-free, no alloc), Catch2 tests. Cross-platform
(pure; no OS deps).

## Global constraints
- `observeBlock(...)` runs on the audio callback: allocation/lock-free, plain loops + atomics.
- Per-channel (L and R independent — the two EARS mics differ; cf. the cal-file "sensitive side").
- Per-session: re-baselined on each run start (amp volume / room can change between sessions).
- Floor is in **dBFS** (linear internally). SPL conversion is NOT here — it's layered on in the
  calibration feature via the cal-file sense factor.

## Units (pure cores are the testable heart)

1. **Pure helpers (`NoiseFloorMath` — header, fully unit-tested):**
   - `bool isQuietBlock(float level, float ceiling)` — true iff `level` is finite and below a **loose
     absolute ceiling** (`kQuietCeilingLin` ≈ −24 dBFS = `kGoodLevelLinear`): a real sweep block sits
     above it, the gap / pre-sweep silence well below. An absolute ceiling, not a recent-peak margin,
     because the FIRST (pre-sweep) window has no prior loud peak to reference. The block "level" is the
     per-ear **PEAK** the capture callback already computes (`blockPeakPerEar`) — same measure as the
     peak-based SNR numerator `maxSweepPeak`.
   - `float robustLowFloor(const float* rms, int n)` — a **running median** (or ~10th–25th percentile)
     of the quiet-window block-RMS values; rejects transient bumps (cough/creak) that a mean would
     retain and that min-hold would over-chase. Median is the literature-endorsed robust choice for
     non-stationary noise (Prawda, Schlecht & Välimäki "rule of two", JASA 2022). Returns linear level.
   - `float blendFloor(float current, float candidate, float alpha)` — slow robust-low update:
     move toward `candidate` only partially, and bias toward the *lower* of the two (the amp hiss is
     stable; resist transient upward spikes). `alpha` small (slow).

2. **`NoiseFloorTracker` (RT-safe wrapper, atomics for cross-thread reads):**
   - `void prepare(double sampleRate, int maxBlock)` / `void reset()` — clear state, invalidate floor.
   - `void observeBlock(float levelL, float levelR, double blockSeconds) noexcept` — per channel: if
     `isQuietBlock` (below the loose ceiling), accumulate the block into the quiet-window buffer and the
     sustained-quiet timer; a non-quiet block resets the run. (Earlier "recent-peak envelope" idea
     dropped — see `isQuietBlock`.) When the quiet run is long enough, fold it into the floor
     into the current quiet-window buffer; when a quiet window is long enough, fold it into the floor
     (`robustLowFloor` → `blendFloor`) and mark `valid`. The FIRST such window is the pre-sweep-silence
     **baseline** (require **≥500 ms**, per AES-2id's pre-sweep-silence recommendation); later windows
     (inter-sweep gaps, ≥~300 ms) **refine** it. Captured through the same ASIO path the sweep uses.
   - `float floorLinear(int ch) const noexcept` / `float floorDb(int ch) const noexcept` — **per-channel
     (BACKEND):** the gate threshold and per-ear SNR are per-channel and use these directly.
   - `float floorDbAveraged() const noexcept` — the single **user-facing** number: the two channels
     averaged in the **linear/power domain** then converted to dB (averaging dB values directly is
     level-wrong). The GUI shows this as the headline readout; it should surface the per-channel split
     only when `|floorDb(L) − floorDb(R)|` exceeds a few dB, so a genuinely noisier/faulty mic on one
     side isn't hidden behind the average. (The display + divergence-surfacing live in the GUI / #27;
     this primitive just provides the level-correct averaged value.)
   - `bool valid() const noexcept` — false until a baseline exists (consumers must gate on this).

## Consumers (wired in this feature)
- **`HealthMonitor` SNR:** replace the fixed `armFloor` denominator with `floorLinear(ear)` once
  `valid()`; before that, fall back to the old constant so a too-early read never divides by zero.
- (Later) AutoPerEar gap threshold = `floorLinear + marginDb`; calibration floor/SPL readout.

## Data flow / timing
Audio callback → `HealthMonitor::analyzeInputBlock` (already computes per-channel level) also calls
`NoiseFloorTracker::observeBlock`. The GUI / SNR verdict read `floorDb`/`valid` via atomics. No new
thread, no new device.

## Error handling / edge cases
- No quiet window yet → `valid()==false`; consumers use their existing fallback.
- A loud transient inside a quiet window → rejected by `robustLowFloor` (low percentile).
- A genuinely rising floor (amp turned up) → tracked, but slowly (`blendFloor`), so it converges
  without jitter.
- NaN/Inf block level → skipped (not folded into the floor).

## Testing (pure, no hardware)
- `isQuietBlock`: boundary at exactly `marginDb`; loud block ⇒ false.
- `robustLowFloor`: a window of mostly-floor values with one loud spike returns ≈ the floor, not the
  mean; empty/short window handled.
- `blendFloor`: converges toward a stable candidate; a single high candidate barely moves it.
- `NoiseFloorTracker`: synthetic per-channel timeline `silence → sweep → gap → sweep` → baseline set
  from pre-sweep silence; floor refined from the gap; L/R independent; `valid()` gating; output does
  not jump on one noisy block; a hand-computed SNR (sweep peak ÷ measured floor) matches.
- `HealthMonitor`: SNR verdict uses the measured floor once valid; falls back before.

## Open / on-device ratification
- `marginDb` (~20 dB-below-peak), the quiet-window sustain (~300 ms), the percentile (~20th), and
  `blendFloor` alpha are synthetic-tuned defaults — the research + the rig ratify them.
