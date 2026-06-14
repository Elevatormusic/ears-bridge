# EARS Bridge — Bench-Validation Runbook (design spec §13 gates)

Each gate is a MANUAL procedure with an explicit PASS criterion. Run all gates on **Windows**
(VB-CABLE) and the macOS-relevant ones on **macOS** (BlackHole 2ch). Anchor Dirac findings to the
Dirac Live version under test (record it). Hardware: an EARS **or** EARS Pro, a host with Dirac Live,
and the platform virtual cable. Record results in the table at the bottom.

> Device-capability reminder (spec §3): original EARS = 48 kHz / 24-bit only, 0–36 dB DIP;
> EARS Pro = 44.1–192 kHz, 16/24/32-bit, 0–45 dB DIP. The app exposes only the detected model's
> native rates/bit-depths; the virtual-sink **output** bit depth is user-selectable 16/24/32 (24
> default; 32 only where the sink accepts it, e.g. BlackHole — no measurement benefit beyond 24-bit),
> requested best-effort on the render format (not enforced).

> These gates are the ONLY validation of behavior that the automated suite cannot reach: real Dirac
> negotiation, real inter-clock drift, the cal-polarity round-trip, and (on macOS) the CoreAudio
> aggregate routing path. None of it compiles or runs in CI — treat a skipped gate as untested, not
> as passing.

---

## Gate 1 — Named virtual cable appears in Dirac AND completes a measurement (Win + mac)

**Why:** spec §13.1 — strongly inferred, not documented; existential to the product.

**Procedure:**
1. Install the cable (VB-CABLE on Windows, BlackHole 2ch on macOS). Reboot if the installer asks.
2. Start EARS Bridge; select the EARS input and the **virtual cable** output; press Start.
3. Open Dirac Live; in the recording-device dropdown, select the virtual cable's **capture** side
   (e.g. "CABLE Output (VB-Audio Virtual Cable)" / "BlackHole 2ch").
4. Run a full headphone measurement sweep on one channel.

**PASS:**
- The virtual cable's capture side **appears** in Dirac's recording-device list.
- Dirac records the sweep with a usable level and **completes** the measurement with no
  "recording inaccuracy / missing samples" error.
- EARS Bridge shows `cleanCapture` green and `xruns == 0` throughout.

**FAIL signatures:** cable absent from Dirac's list (driver not installed / wrong endpoint); Dirac
shows missing-sample error (a Bridge xrun — check `healthFlags()` for `Xrun`/`Dropout`).

---

## Gate 2 — Dirac stereo-channel + SPL behavior with a known-level mono tone

**Why:** spec §13.2 — learn whether Dirac reads ch0, sums (+6 dB), or averages a stereo capture.

**Helper:** generate a calibrated mono tone WAV (1 kHz, −20 dBFS, 10 s) and play it to the EARS
acoustically (or inject a known electrical level). A tiny generator can live at
`tools/gen_tone.py` (writes `tone_1k_-20dBFS.wav`); or use REW's signal generator.

**Procedure:**
1. Drive the known −20 dBFS / 1 kHz tone so **both** EARS mic channels see the same level.
2. In EARS Bridge, set combine mode to **Average** `(L+R)/2`; note the app's mono output meter dB.
3. Switch to **Sum** `L+R`; note the new mono output meter dB.
4. In Dirac, observe the recorded level for each combine mode.

**PASS:**
- Average shows the **same** level as a single channel (0 dB relative); Sum shows **+6 dB**
  relative to Average for identical L=R content. (This confirms the combine math and lets you
  predict Dirac's absolute-SPL target offset.)
- The dual-mono delivery (L=R on the virtual cable) means Dirac reading ch0, sum, or average all
  yield a consistent, predictable level — record which Dirac actually does for this version.

---

## Gate 3 — Cal-polarity REW round-trip on a known reference (inverse −dB confirmed)

**Why:** spec §13.3 / §3.6 — the EARS cal carries a large +dB bump that must be **removed**; confirm
the Bridge applies the **inverse** (−dB), matching REW's `corrected = raw − cal`.

**Procedure:**
1. In REW, import a known raw measurement `R` and the EARS cal `C` (e.g. `R_HPN_8604350.txt`);
   REW computes `corrected_REW = R − C`.
2. Feed the **same** raw signal through EARS Bridge with the **same** cal loaded; capture the Bridge
   output `corrected_Bridge` (record the virtual-cable output in REW or Dirac's raw capture).
3. Overlay `corrected_Bridge` and `corrected_REW`.

**PASS:**
- The two curves match within **±1.0 dB** across 40 Hz–18 kHz. In particular, at the ~4 kHz EARS
  resonance (cal ≈ +13…+22 dB) the Bridge output shows a **cut** of that magnitude (not a boost).
- If the Bridge output instead **boosts** at 4 kHz, polarity is wrong — `FirDesignParams.invert`
  must be `true` (it is by default); investigate before shipping any equivalence claim.

---

## Gate 4 — WASAPI-exclusive rate negotiation with the virtual cable

**Why:** spec §13.4 — Dirac opens the recording device WASAPI-exclusive; confirm the chosen rate is
accepted. Test **48 kHz (EARS)** AND a **high rate (96/192 kHz, EARS Pro)**.

**Procedure:**
1. **EARS @ 48 kHz:** set EARS Bridge to 48 kHz; set the virtual cable's internal SR to 48 kHz
   (VB-CABLE control panel / AMS). In Dirac (exclusive), run a measurement.
2. **EARS Pro @ 96 kHz then 192 kHz:** set EARS Bridge to 96 kHz, the cable to a matching/clean SR,
   and repeat; then 192 kHz. (The FIR auto-scales taps: 8192 @ 48 k → 16384 @ 96 k → 32768 @ 192 k.)

**PASS:**
- Dirac's exclusive open **succeeds** at each tested rate without "format not supported" / silent
  failure; the measurement completes.
- If exclusive open is rejected, the app's shared-mode fallback engages and the run still completes
  (note that `DAUDIO_WASAPI_NON_EXCLUSIVE=ON` is the documented Dirac opt-out).
- EARS Bridge reports the **input** opened at its native rate (no resample warning for a native
  selection); a non-native rate shows the resample warning as designed.

---

## Gate 5 — Inter-clock drift magnitude over a real sweep (incl. mismatched rates)

**Why:** spec §13.6 — measure real drift to confirm the ASRC is mandatory and the control loop is
stable (no oscillation). Include a **mismatched-rate** case: 96 kHz capture / 48 kHz render.

**Applies to:** the **Windows two-device path** and the **macOS two-device fallback** (when the
aggregate fails to build). The macOS aggregate path is Gate 7.

**Procedure:**
1. Same-nominal-rate case: EARS @ 48 kHz, virtual cable @ 48 kHz. Run a 5-minute continuous sweep
   loop. Log `health().captureToRenderRatio` and `health().fifoFill` every second.
2. Mismatched case (EARS Pro): capture @ 96 kHz, render @ 48 kHz. Repeat the 5-minute log.

**PASS:**
- The ClockBridge holds `fifoFill` within a bounded band (no monotonic emptying/filling); **no**
  underrun/overrun (`underruns()==overruns()==0` or only a tiny startup transient).
- `captureToRenderRatio` settles near its nominal (1.0 same-rate; ~2.0 for 96 k→48 k) and does
  **not** oscillate (the PI loop on smoothed fill does not "breathe"). `healthFlags()` has **no**
  `ExcessDrift` after the startup window.

---

## Gate 6 — Dirac analysis-window length vs worst-case bridge+cable delay

**Why:** spec §13.7 — a fixed bridge+cable delay (tens of ms) shifts the impulse in Dirac's analysis
window; confirm it still lands inside the window at the worst-case latency.

**Procedure:**
1. Measure the end-to-end Bridge latency: app FIFO depth + ASRC + cable + Dirac buffering. Read the
   app's reported latency (status bar) and add the cable's nominal latency.
2. Run a Dirac measurement at the **largest** buffer size you will ship as default.
3. Inspect Dirac's impulse/IR view: confirm the located impulse sits well inside the analysis window
   (not clipped at the window edge).

**PASS:**
- Dirac locates the impulse (log-sweep cross-correlation) and applies gain+delay compensation; the
  impulse is comfortably inside the window with margin (≥ 2× the measured delay of headroom).
- No "recording inaccuracy" warning at the worst-case latency.

---

## Gate 7 — macOS CoreAudio aggregate-device path (Plan 4 Task 7)

**Why:** macOS prefers a private CoreAudio **aggregate** (EARS clock-master + virtual output,
follower drift-corrected) so both capture and render run off ONE clock. This path is Objective-C++
that does **not** compile or run on Windows, so it has **zero** automated coverage — every claim
below is bench-only. The capture and render IOProcs are still **separate callbacks** (the engine
opens an input-only and an output-only device from the same aggregate), so the lock-free FIFO is the
required conduit between them, running at a trivial **1:1** ratio (no drift to correct). A regression
that fails to size that FIFO produces **silence**, not an error — verify audio actually flows.

**Procedure:**
1. macOS host with an EARS/EARS Pro **and** BlackHole 2ch installed. Start EARS Bridge, select the
   EARS input and BlackHole output, press Start.
2. Confirm the aggregate engaged (not the fallback): the app reports the aggregate path active, and
   the private aggregate is **NOT** visible in Audio MIDI Setup (`kAudioAggregateDeviceIsPrivateKey`).
   If the app shows the two-device fallback instead, record the create() error and validate via Gate 5.
3. Drive a known tone into the EARS; confirm a **non-zero** output level on the app's mono meter AND
   on the BlackHole capture in Dirac/REW. (This is the silent-FIFO check.)
4. Run a 5-minute continuous sweep loop. Watch latency and level.
5. Forced-failure: temporarily select an output the aggregate cannot wrap (or remove BlackHole) and
   Start — confirm the app **falls through** to the two-device + ClockBridge path and still runs.
6. Stop; confirm the aggregate is torn down (gone from any CoreAudio enumeration; no orphaned
   private device after the process exits).

**PASS:**
- Aggregate engages and is private (hidden from Audio MIDI Setup).
- Audio **flows** (non-zero capture in Dirac) — no silence.
- The 5-minute run shows **no** progressive latency growth and **no** dropouts/silence;
  `cleanCapture` stays green.
- Forced-failure cleanly falls back to the two-device path (no crash, no hang).
- Teardown leaves no orphaned aggregate device.

**FAIL signatures:** Dirac capture is silent (FIFO unsized / not engaged — regression of the Task-7
fix that sizes the bridge on the aggregate path); aggregate visible in Audio MIDI Setup (private flag
lost); progressive latency (follower drift-correction off — check EARS is master `false` / output
`true`); orphaned device after exit (destroy() not reached).

---

## Results table (fill in per run)

| Gate | Platform | Device   | Rate    | Dirac ver | Result | Notes |
|------|----------|----------|---------|-----------|--------|-------|
| 1    | Windows  | EARS     | 48 k    |           |        |       |
| 1    | macOS    | EARS Pro | 96 k    |           |        |       |
| 2    | Windows  | EARS     | 48 k    |           |        |       |
| 3    | Windows  | EARS     | 48 k    |           |        |       |
| 4    | Windows  | EARS     | 48 k    |           |        |       |
| 4    | Windows  | EARS Pro | 96/192k |           |        |       |
| 5    | Windows  | EARS Pro | 96→48 k |           |        |       |
| 6    | Windows  | EARS     | 48 k    |           |        |       |
| 7    | macOS    | EARS     | 48 k    |           |        |       |
| 7    | macOS    | EARS Pro | 96 k    |           |        |       |

> Version drift (spec §13.8): all Dirac findings are version-anchored. Record the Dirac Live version
> in each row and re-run on every Dirac update (exclusive-mode default and the missing-sample
> detector have both changed within 3.x).
