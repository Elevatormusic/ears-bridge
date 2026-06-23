# Capsule SPL + SNR Readout (Calibration sub-project A) — Design Spec

**Status:** Brainstorm + **mandatory research complete** (3 agent workflows over the user-provided
miniDSP EARS / EARS Pro manuals + Dirac guides, REW docs, the REW author, and first-principles — all
converge). First of four calibration sub-projects (A → B headphones/IEM mode → C 1 kHz level-set → D
rig characterization). Builds on the noise-floor primitive (#1).

**Goal:** After a measurement, show the noise floor, sweep level, and SNR in **real dB SPL at the EARS
capsule**, with an honest verdict — resolving the gain/SNR confusion that started the calibration work.

**Tech stack:** C++20, JUCE 8, Catch2 v3. Pure cores + RT-safe wrappers; tests registered in
`tests/CMakeLists.txt`; build/test via `./tools/dev.cmd`.

## The confirmed dBFS→SPL formula (research-validated, primary sources)
```
SPL_capsule(dB) = dBFS − SensFactor + 94 − (DIP_gain − 18)
```
- **SensFactor**: per-channel, from the cal-file header line 1 (`Sens Factor =0.9dB, …`). REW's
  definition (verbatim): *"the input dBFS reading the mic will produce when driven by a 94 dB calibrator
  with the input volume set to maximum."* So `dBFS = SensFactor ⇒ SPL = 94` (the anchor).
- **94**: the universal IEC calibrator reference baked into the Sens Factor.
- **DIP_gain**: the EARS analog gain (default 18 dB). The regular-EARS Sens Factor is referenced to
  18 dB; the `−(DIP_gain−18)` term corrects a changed switch. (For the regular EARS the gain is a fixed
  hardware DIP, not a software slider, so there is NO live-volume term — it collapses to a constant.)
- At factory 18 dB this is a **fixed per-channel offset**: `SPL = dBFS + (94 − SensFactor)`.
- **No calibrator, no mV/Pa, no REW-internal constant, no user level-set** — fully self-contained in the
  cal file. Sources: REW Cal Files / inputcal help; REW changelog V5.20.1 (EARS, 18 dB Windows
  assumption); John Mulcahy (AVNirvana). The "−40 dBFS → 84 dB SPL" rule is a web mis-report — discarded.

## Two honesty conditions (the only things that offset SPL)
1. **Windows input volume must be 100%.** A software input-volume below max silently scales the dBFS
   (REW reads it live and requires a non-"Default Device" input for this reason). A **reads the OS input
   volume** for the EARS endpoint and **warns if it isn't maxed**; if it can't read it, it caveats.
2. **DIP must match the assumed gain** (default 18; user-confirmable per sub-decision). A surfaces the
   assumed gain so a changed switch is visible.

## Components (pure cores + a readout)
1. **`CalFile` sense-factor parse** — extend `CalFile::parse` (src/cal/CalFile.cpp) to read
   `Sens Factor =<float>dB` from header line 1 → `std::optional<float> sensFactorDb`. Absent → SPL
   unavailable (fall back to dBFS/SNR-only). Per-channel (each L/R cal file has its own).
2. **`SplMath` (pure, header)** —
   - `float capsuleSplDb(float dBFS, float sensFactorDb, float dipGainDb)` = the formula above.
   - `float snrDb(float sweepDbFS, float floorDbFS)` = `sweepDbFS − floorDbFS` (SPL-independent).
   - `float dbfsFromLinear(float lin)` helper (20·log10, tiny-guarded).
3. **`MeasurementVerdict` (pure, header)** — `verdict(float snrDb, bool clipped, bool floorValid)` →
   `{ Clipping | NoiseLimited | Healthy | Unknown, adviceKey }`:
   - `clipped` (from `HealthMonitor::clipConfirmed()`) → Clipping ("turn the output level down").
   - `!floorValid` → Unknown (no trusted floor yet).
   - `snrDb < kSnrNoiseLimited` (~20 dB, ratify) → NoiseLimited ("the amp/room is your ceiling").
   - else → Healthy. (`kSnrHealthy` ~30 dB for the GUI accent.)
4. **DIP-gain setting** — a small persisted "EARS gain" (default 18 dB; steps 0/6/12/18/24/30/36) the
   user confirms; feeds `SplMath`.
5. **Input-volume read + guard** (read+guard chosen over caveat-first) — `platform/InputVolume.*` reads
   the EARS input endpoint's master-volume scalar (0..1) via WASAPI `IAudioEndpointVolume` on Windows
   (guarded like `EndpointUid`/`EndpointFormat`; a stub returning "unknown" elsewhere). Pure decision
   `bool inputVolumeMaxedForSpl(float scalar, float tol = 0.01f)` → `scalar >= 1 - tol`. Read off the
   audio thread (on prepare + a periodic poll, like `pollChainConfig`).
6. **Readout** — a "Measurement quality" card (noise floor / last sweep / SNR in dB SPL + the verdict +
   advice) near the Input monitor. Pure presentation-model (like `SignalChainView`); the visual build
   gets the **apple-hig** treatment. SPL shown ONLY when `sensFactorDb` present AND the input volume reads
   maxed; not-maxed → SNR-only + "set the EARS input level to 100% in Windows for SPL"; unknown
   (macOS / unreadable) → SPL with a one-line caveat. SNR + verdict always shown (volume-independent).

## Data flow
The engine already has the per-channel floor (#1 `measuredFloorLinear`), the sweep peak
(`maxSweepPeak`), and the loaded cal. At the sweep-complete edge (mirroring the existing SNR snapshot):
compute `capsuleSplDb(floor)`, `capsuleSplDb(sweep)`, `snrDb`, and the verdict → publish via the
int-milli idiom → the card reads them message-thread-side.

## Error handling / fallback
- No `Sens Factor` in the cal file → no SPL; show SNR (dB) + "load a cal file with a sense factor for SPL".
- Input volume not max / DIP unconfirmed → show SPL with the caveat, or downgrade to SNR-only.
- No valid floor yet (`!floorValid`) → Unknown verdict, no SNR claim.

## Testing (pure, no hardware)
- `CalFile`: parse the exact header `"Sens Factor =0.9dB, EARS Serial 860-4350, compensation HEQ V2"`
  → `sensFactorDb == 0.9`; absent → `nullopt`; still reads type/side/serial.
- `SplMath`: `capsuleSplDb(dBFS=SensFactor, …, 18) == 94` (the anchor); the `−(DIP−18)` correction;
  `snrDb`. Hand-checked constants.
- `MeasurementVerdict`: each boundary (clipped, !floorValid, below/above the SNR thresholds).
- `inputVolumeMaxedForSpl`: 1.0 → true; 0.99 → true (tol); 0.9 → false; a negative "unknown" sentinel
  handled by the caller (caveat path).
- The card via the presentation-model pattern (SPL-shown vs SNR-only vs caveat states).

## Open / on-device ratification
- The verdict SNR thresholds **`kSnrNoiseLimited = 20`, `kSnrHealthy = 30`** (user-approved 2026-06-22) —
  ratify on the rig but ship at these.
- The WASAPI input-volume read is **Windows-only**; macOS falls back to the caveat-and-show path. (Read +
  guard chosen over caveat-first.) The pure `inputVolumeMaxedForSpl` decision is fully unit-tested; the
  WASAPI `IAudioEndpointVolume` read itself is hardware/manual (Windows), like the other `platform/` reads.
