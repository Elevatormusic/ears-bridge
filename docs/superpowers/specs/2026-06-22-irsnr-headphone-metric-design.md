# IR-SNR quality metric — headphone-correct redefinition — Design Spec

**Status:** Brainstorm + design approved (2026-06-22, Approach A). **Mandatory research complete** (2 agent
passes over Huszty & Sakamoto's SNR definition, Farina/Novak ESS, the INR quality metric, REW practice,
headphone-IR decay data — see "Research basis"). Branches off `main`.

**Goal:** Fix the reference-monitor IR-quality grade so it is **correct for headphone impulse responses**.
Today every clean headphone sweep grades `GradedSuspect` (never `GradedClean`) because the IR-SNR metric
reads **negative** on a clean spread IR. Redefine the metric so a clean measurement reads solidly positive,
while a genuinely noisy/contaminated one still reads low.

**Tech stack:** C++20, JUCE 8, Catch2 v3. Pure header-only DSP (`src/gui/IrQualityStatus.h::evaluateIr`),
used by `RefMonitor::gradeMeasurement`. Buildable + TDD-able **without hardware** (synthetic IRs).

## The bug (recap)

`irSnrDb = 10·log10(peakEnergy / tailEnergy)` with **peak = ±2 samples** (`kPeakHalfWidth=2`) around the
max-|sample| index and **tail = everything else** (minus the harmonic spots). That assumes a SHARP impulse.
A headphone IR (headphone + coupler + driver + the cal FIR) is **spread over ~2–5+ ms / hundreds of
samples**, so ±2 samples (~0.04 ms) captures <1 % of the real IR energy and the genuine response is counted
as "noise" → negative SNR on a pristine sweep. Proven on-device: 7 real grades, all clean (sweepSNR 22–25 dB,
coherence 0.99), all IR-SNR −2.6 to −6.2 dB. The synthetic-delta unit test passes (~13 dB) because a
regularized delta IS sharp — the integration gap only a real measurement exposes.

## Research basis (what it changed about the naive fix)

1. **Noise is NOT the pre-arrival region.** For an ESS-deconvolved (circular) IR, Farina's harmonic-
   distortion products sit at NEGATIVE time and **wrap to just before the linear IR**, and a linear-phase
   cal FIR adds symmetric **pre-ringing** there. So the clean noise floor is the **quiet region away from
   the peak, excluding the wrapped harmonic spots** (the code already locates them) — not pre-arrival.
2. **Per-sample mean power, not summed energy** (Huszty & Sakamoto, *Acoust. Sci. & Tech.* 2012, Eq. 2):
   `SNR = 10·log10[ (Σsignal²/N) / (Σnoise²/M) ]` — each window divided by its OWN sample count so the
   (different) window lengths don't bias the ratio.
3. **Signal is the gated IR span, not the peak.** Headphone IRs decay to −40 dB in <2 ms, −50 dB in <3 ms
   (open-back best case; longer closed/with coupler resonance) — a few ms, far shorter than REW's room
   defaults (125/500 ms) but far longer than ±2 samples.

## The redefined metric (`evaluateIr`)

The pure function keeps its peak-finding + harmonic-offset machinery; only the energy split + ratio change.

1. **Peak** `peakIdx` = argmax|ir| (unchanged — the direct arrival ≈ the peak).
2. **Signal gate** = a window around the peak holding the IR's real response:
   `[peakIdx − preMargin, peakIdx + signalSpan]` (circular). `signalSpan ≈ kSignalGateMs · rate` (post-peak
   decay), `preMargin ≈ kPreMarginMs · rate` (a small margin for the onset + a short pre-ring). Defaults:
   `kSignalGateMs ≈ 8`, `kPreMarginMs ≈ 0.5` — **PROVISIONAL, on-device ratification**. Asymmetric (the
   default min-phase IR is causal); the long-FIR linear-phase pre-ring is a noted ratification item.
3. **3-way per-sample classification** (precedence: gate → harmonic → noise):
   - `inSignalGate(i)` → `signalEnergy += ir[i]²`, `signalCount++`.
   - else `inAnyHarmonic(i)` (the existing circular harmonic guard) → `harmEnergy += ir[i]²` (distortion;
     not signal, not noise).
   - else → `noiseEnergy += ir[i]²`, `noiseCount++` (the clean deconvolution noise floor).
4. **`irSnrDb = 10·log10( (signalEnergy/signalCount) / (noiseEnergy/noiseCount) )`** (mean-power ratio,
   tiny-floored). `thdPercent = 100·sqrt(harmEnergy / signalEnergy)` (harmonics vs the full gated response,
   not just the peak sample).
5. **Robustness:** clamp the gate to `n`; if `noiseCount == 0` (a gate covering the whole IR) → fall back to
   the tiny floor (no NaN). The match-gate (`matched`) precedence is unchanged — never grade a non-match.

## Signature change (backward-compatible)

Add `double sampleRate = 48000.0` as the LAST parameter of `evaluateIr` (after `harmonicOffsetsSamples`),
so the function owns the ms→samples conversion and existing positional callers keep working. The real
caller (`RefMonitor::gradeMeasurement`) passes the measurement rate. The internal `kSignalGateMs` /
`kPreMarginMs` are file-scope constants (PROVISIONAL).

## Thresholds (re-ratify, keep INFO-only posture)

- `kMinIrSnrDb = 20` is from the OLD broken metric. The new mean-power IR-SNR on a clean headphone sweep
  will read a different (solidly positive) value — references cite ~45–60 dB *peak-to-noise* for a clean
  swept measurement; the mean-power number calibrates on-device. **Set a conservative provisional
  `kMinIrSnrDb` ≈ 6 dB** (a clean signal's per-sample power is at least a few × the noise; clearly positive,
  well clear of the old −4 dB regime) and keep **`kIrThresholdsRatified = false`** — the GUI
  still shows the numbers as INFO, no false warn — until the on-device campaign confirms clean-vs-bad
  separation, then flip it.
- `kMaxThdPct = 5` unchanged (THD already read low/correct).

## Testing (pure, no hardware)

Synthetic IRs in `test_irquality.cpp` / `test_refmonitor.cpp` (a few hundred samples each, rate 48000):
- **Sharp delta** (a single spike + a quiet tail) → still reads HIGH IR-SNR (regression: the old good case).
- **Spread headphone-like IR** (energy decaying over ~5 ms / ~240 samples + a quiet noise floor) → now reads
  **HIGH** (the bug fixed; the old metric read negative on this).
- **Noisy IR** (a weak/spread response buried in a high noise floor) → reads **LOW**.
- **Harmonic-contaminated IR** (energy injected at the harmonic offsets) → THD catches it; IR-SNR is NOT
  falsely tanked by the harmonics (they're excluded from noise).
- **Per-sample normalization** check: lengthening the noise window alone must NOT change `irSnrDb`
  (mean-power invariance) — the property that was broken before.
- The `evaluateIr` defaults + the `RefMonitor::gradeMeasurement` wiring pass the rate through.

## Open / on-device ratification

- `kSignalGateMs` (~8) + `kPreMarginMs` (~0.5) + `kMinIrSnrDb` — calibrate against real clean-vs-bad sweeps,
  then flip `kIrThresholdsRatified`.
- **Linear-phase cal FIR** (the Dirac-phase fix path) adds symmetric pre-ringing of ~`firLen/2` samples; the
  asymmetric gate may count some as noise → a phase-aware/wider pre-margin is a follow-up (the metric is
  INFO-only meanwhile). [[ears-dirac-phase-precision]]
- Confirm on-device that a clean sweep now grades **`GradedClean`** (the whole point).

## Out of scope

- The Schroeder/EDC decay metric (Approach B) and any change to the match-gate or THD definition beyond the
  denominator.
- Flipping `kIrThresholdsRatified` (that's the on-device campaign, not this build).
