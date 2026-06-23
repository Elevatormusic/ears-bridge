# Hardware-Dirac detect-and-degrade — Design Spec

**Status:** Brainstorm + design approved (2026-06-22). Research complete + primary-source verified
(`docs/research/2026-06-22-ddrc-24-hardware-dirac-compatibility.md`). Branches off `main`.

**Goal:** Make EARS Bridge **correct** when Dirac runs on a hardware miniDSP processor (DDRC-24 / SHD /
Flex) instead of Dirac Live software on the PC. The per-ear measurement already works (it is entirely
mic-side); the only casualty is the reference-monitor **grade** — the box generates its sweep internally,
so the WASAPI loopback captures no digital reference. This feature **detects** hardware Dirac
*behaviorally* (no device names) and **degrades** the grade to a calm, honest "grading off" state instead
of a never-green grade.

**Tech stack:** C++20, JUCE 8, Catch2 v3. Pure cores + one Windows-guarded platform read + GUI.

## Why — the behavioral signature (no device names)

The defining trait of hardware Dirac is a *physical fact*, not an identity:

> The sweep is generated **inside the box**, so it reaches the EARS mic acoustically (through the
> headphones) but **never appears as a PC render stream**. Software Dirac is the opposite — its sweep
> *is* a PC render stream (exactly what our loopback captures).

So we detect the **behavior**, not the USB name/VID (a VID list wouldn't even work — the EARS jig is also
a miniDSP USB device). This is model-agnostic and future-proof.

## Detection (auto, *suggests*) + confirmation (toggle, *commits*)

- **Auto-detect — suggests, never silently suppresses.** During a measurement the pure
  `sweepWasInternal(...)` (exact signature in Components) is true only when **all** of:
  - `micHeardSweep` — the EARS mic clearly registered a sweep this run (a completed sweep with real
    per-ear energy; the engine already tracks `maxSweepPeakL/R` + the sweep-complete edge).
  - Dirac's output endpoint was **readable AND silent** — `OutputActivity` read a real value and it showed
    no PC render. An *unreadable* output (−1) does NOT qualify (we can't confirm silence, so we don't guess).
  - `validMode` — Dirac is in a valid Windows-Audio (shared) mode
    (`diracDeviceTypeIsWindowsAudio(readDiracDeviceType())`) — rules out exclusive/ASIO silence, a
    different case we already surface.
  On fire (and the toggle isn't already set) → surface the **honest suggestion** message. It never flips
  grading off by itself (a false positive must not silently kill a software-Dirac user's grading).
- **Toggle — commits.** `Settings::diracHardwareProcessor` (Advanced). The user confirms the suggestion,
  sets it proactively, or overrides a misdetection. When ON → the calm `GradingOffHardware` state, the
  Learn-reference flow suppressed, no loopback grade attempted.

## Components

1. **`Settings::diracHardwareProcessor`** (bool, default `false`) — persisted; mirrors `advancedOverride`
   (`getBoolValue`/`setValue`, total — no validation needed).
2. **`platform/OutputActivity.{h,cpp}`** (NEW; Windows-guarded, mirrors `EndpointFormat` / `EndpointUid`;
   stub elsewhere). `float outputRenderPeakForName(const juce::String& deviceName)` — activate
   `IAudioMeterInformation` on the render `IMMDevice` whose FriendlyName equals `deviceName`,
   `GetPeakValue` → 0..1; returns **−1.0f** when unknown (non-Windows / unmatched / COM failure). Runs
   OFF the audio thread (COM + enumeration), like `InputVolume`. Registered next to `EndpointFormat.cpp`.
3. **Pure decision core `src/audio/HardwareDiracDetect.h`:**
   - `bool outputRendered(float renderPeak, float floor = 0.0015f) noexcept` → `renderPeak > floor`
     (a real PC render). The −1 unknown sentinel → `false`.
   - `bool sweepWasInternal(bool micHeardSweep, bool outputRenderedReadable, bool outputDidRender,
     bool validMode) noexcept` → `micHeardSweep && outputRenderedReadable && !outputDidRender &&
     validMode`. **Key:** an UNREADABLE output (−1) must NOT auto-suggest (we can't confirm the output was
     *silent* — it might just be unreadable), so the decision requires the output to be *readable AND
     silent*, not merely "not rendered".
   - `const char* hardwareDiracSuggestion() noexcept` / `const char* hardwareDiracGradingOff() noexcept` —
     the honest copy (suggestion + the calm-state line).
4. **`RefMonState += GradingOffHardware`** — a calm state. Classified `refMonBlocksGreen == true` (no
   reference grade → never a "verified clean" green) but rendered **neutrally** (not a warning/red); it
   does **not** gate Start, and the measurement's own quality (SNR / clip / level) is shown independently
   and can still read clean. `nextRefMonState` is unchanged — this state is published directly by the
   toggle path, never inferred from a grade.
5. **Engine wiring (`AudioEngine`):** a message-thread output-activity poll (each tick during a
   measurement, when the toggle is OFF) reads Dirac's output render peak (target =
   `readDiracOutputDeviceName()`) and tracks `maxOutputRenderPeak` + whether it was *readable*; the engine
   already has `micHeardSweep` (a completed sweep with `maxSweepPeak` above a real-signal floor). At the
   sweep-complete edge → `autoDetectedHardwareDirac = sweepWasInternal(...)`. Getters:
   `autoDetectedHardwareDirac()`, plus the toggle plumbs `setDiracHardwareProcessor(bool)` →
   suppress the grade + publish `GradingOffHardware`.
6. **GUI (`MainComponent`), apple-hig copy pass:** the Advanced toggle; the auto-detect **suggestion**
   (shown when `autoDetectedHardwareDirac() && !diracHardwareProcessor`); the calm `GradingOffHardware`
   state in the grade status area; the **Learn-reference flow suppressed** when the toggle is on.

## Data flow

1. Measurement runs (bridge running). Toggle OFF → the GUI poll reads Dirac's output render peak each tick
   (`outputRenderPeakForName(readDiracOutputDeviceName())`), tracking `maxOutputRenderPeak` + a `readable`
   flag, and reads `validMode` once (`diracDeviceTypeIsWindowsAudio(readDiracDeviceType())`).
2. At the sweep-complete edge, the engine has `micHeardSweep` (a completed sweep, `maxSweepPeak` > floor).
3. `autoDetectedHardwareDirac = sweepWasInternal(micHeardSweep, readable, outputRendered(maxPeak),
   validMode)`.
4. GUI: `autoDetectedHardwareDirac && !diracHardwareProcessor` → the suggestion message (+ confirm action
   that sets the toggle). `diracHardwareProcessor` → the calm `GradingOffHardware` state + Learn suppressed
   + no grade attempt.

## Error handling / fallback

- `OutputActivity` unreadable (−1: non-Windows / unmatched / COM failure) → the auto-detect does **not**
  fire (cannot confirm silence). The toggle still degrades correctly regardless of platform.
- macOS: `OutputActivity` stubs to −1 → no auto-detect; the toggle path works (the calm state has no
  platform dependency).
- False auto-detect → the user simply doesn't confirm (or unsets the toggle). Missed auto-detect → the
  user sets the toggle. The toggle is always the deterministic ground truth.

## Testing (pure, no hardware)

- `Settings::diracHardwareProcessor`: default `false`; round-trips through a reload.
- `outputRendered`: peak > floor → true; 0 → false; −1 (unknown) → false.
- `sweepWasInternal`: the truth table — only `mic && readable && !rendered && validMode` is true;
  unreadable output (`readable=false`) → false even if `!rendered`; not-valid-mode → false; no mic sweep
  → false.
- `RefMonState`: `GradingOffHardware` is in `refMonBlocksGreen` (never reads green) AND is distinct from
  the warning states; `nextRefMonState` is unaffected.
- The copy: the suggestion names a "hardware processor" + points to the Advanced toggle; the calm line
  says grading is off but the per-ear calibration is active.
- GUI (render-verified): the toggle + persistence, the suggestion, the calm state, the suppressed Learn.
- `OutputActivity` WASAPI read: **build-verified** (hardware/COM, manual on Windows, like `EndpointFormat`).

## Open / on-device ratification (needs a real DDRC-24)

- That a hardware Dirac unit truly streams **nothing** to its PC output during calibration (the manual
  says the sweep is "generated inside the processor" → the render peak should stay silent — confirm).
- The exact `micHeardSweep` engine signal (a completed sweep with `maxSweepPeak` above what floor) and the
  output-peak silence floor (`0.0015` provisional).
- That AutoPerEar still follows the swept ear through the box (independent of this feature, but in the
  same on-device session).

## Out of scope (roadmap, later)

- Alt-reference grading (a stored/canonical ESS reference or rig-characterization, calibration
  sub-project D) — restoring an actual grade without the loopback.
- The full hardware-Dirac setup guide / a dedicated setup mode.
