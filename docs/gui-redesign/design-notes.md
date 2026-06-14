# EARS Bridge — GUI redesign (design notes)

A proposed redesign of the desktop window, applying `docs/GUIDesignGuide.md` (the Apple-HIG-derived,
framework-neutral guideline). These are **design mockups** for review — not yet implemented in the
JUCE code. Open the HTML files in a browser to see them full size.

All three keep the same functionality (input, two per-ear calibrations, combine mode, output sink,
sample rate, bit depth, level meters, clean-capture status, Start/Stop, and the Advanced disclosure)
and the same design system (tokens, type ramp, components); they differ in composition.

## Three directions

- **A — Panel** ([`mockup-a.html`](mockup-a.html)): dark, a single sectioned column (Input →
  Calibration → Routing → Format → Advanced) with the meters and Start in a bottom transport bar.
- **B — Studio** ([`mockup-b.html`](mockup-b.html)): dark, a two-pane layout — a compact config rail
  on the left, the calibration plots and meters given the most space on the right, Start in the
  content header.
- **C — Clean (light)** ([`mockup-c.html`](mockup-c.html)): the light appearance, with the meters,
  clean status, and Start anchored in a top bar above an airier single-column body.

## Same design on Windows and macOS

The shared design system — layout, components, color tokens, and the type roles — renders identically
on both platforms (the JUCE app draws its own widgets). What differs is the OS-owned chrome, which you
let each platform handle: window-frame controls (traffic lights top-left on macOS vs. min/max/close
top-right on Windows — the mockups show the macOS dots), the menu-bar placement, the native file
dialogs, and the actual system font the stack resolves to (San Francisco on macOS, Segoe UI on
Windows). Theme (light/dark) follows the OS appearance setting; mockup C shows the light variant of
the same design.

## How it maps to the guide

- **Deference / content first (§1).** The two per-ear calibration plots are the largest, most
  prominent elements — the curve is the content; labels, dropdowns, and borders are quieter than the
  plotted line.
- **Layout & 8-point grid (§3).** All spacing is on an 8/4-pt grid. Related controls are grouped
  under quiet section labels (Input, Calibration, Routing, Format), with internal padding ≤ the gaps
  between groups. The layout is responsive: the cal cards and the rate/bit-depth pair collapse from
  two columns to one as the window narrows.
- **Typography (§4).** A single system-font stack (San Francisco on macOS, Segoe UI on Windows) with
  a mono stack for data (file names, dB values, serial). Type is a small role-based ramp; section
  labels are small, tracked, and muted rather than loud.
- **Color (§5).** One accent (blue `#0A84FF`) shared by the data line and the Start button. Semantic
  status tokens: green for clean, amber for caution, red for error — each defined once.
- **Accessibility (§2).** Status never relies on color alone — "Clean" shows a check icon **and** the
  word; the inline rate caution shows a warning triangle **and** text. Meters encode level by bar
  length (position) **and** a numeric dB readout, with a red zone near clip. Hit targets are
  comfortable; a visually-hidden summary describes the screen for screen readers.
- **Components & states (§9).** One filled primary action (Start); dropdowns show the current value
  with helper text directly beneath; the empty Right-ear card is a real empty state ("Drop a
  calibration file… or Load…") rather than a blank panel; the "Recommended" combine mode is tagged.
- **Progressive disclosure (§9.8).** The common path (input → calibration → combine → output → format
  → Start) is visible; Complex-phase FIR, FIR length, and Output trim stay behind **Advanced**.
- **Inline validation (§9.9).** Cautions and errors appear next to the control they concern (the rate
  caution under Format, the output preflight under Output), not in modal dialogs.
- **Depth without glass (§7).** A flat dark theme with a clear two-step elevation (window base vs.
  raised cards) and crisp 1px separators — portable across macOS and Windows.

## One intentional change

The monitor shows the input meters as a **pre-routing monitor** so levels can be checked before
pressing Start. (Today the engine only meters while running.) This is a small usability addition, not
required by the current code.

## Option B (Studio) — selected direction, refinements applied

`mockup-b.html` was revised after review:

1. **One transport zone (top-right).** Start/Stop and the run state are a single control — the button
   reads Start when stopped and Stop when running, and *is* the state indicator. The redundant
   "Stopped" pill is gone, and Start no longer sits under the Calibration header (it governs the whole
   bridge, not calibration). Capture status ("Clean") lives in the same zone when running.
2. **Start is the only filled control.** It is the single filled accent button; every other button
   (Replace…, etc.) is secondary/outlined.
3. **"Recommended", not "Rec".** Spelled out with a check, so it can't read as a record-arm in a
   live-audio tool.
4. **Honest meter zones.** Green through the normal range; amber only in the last ~3 dB before clip;
   red at/over 0 dBFS. A healthy −6 dB output reads green, not amber.
5. **Start is gated when calibration is incomplete.** With the right ear unloaded, Start is disabled
   with a one-line reason ("Load a right-ear calibration to start"), the right-ear card is tagged
   "Required to start", and the empty card guides the next action.

Polish: one consistent load affordance (a "Replace…" button on loaded cards, a click-or-drop zone on
empty ones); plot axes labelled (dB and Hz); cards separated by an elevation fill-step with no
outline so the blue curve stays the most colourful thing on screen; the Dirac helper expanded to
"In Dirac Live, choose this device's capture side as the recording input."

## Authentic macOS tokens

`mockup-b.html` is now coloured and sized from the real **macOS 26 UI kit** tokens (see
[`apple-tokens.md`](apple-tokens.md)): base window `#1E1E1E`, the current system blue `#0091FF`
(not the older `#0A84FF`), green `#30D158`, orange `#FF9230` for caution, red `#FF4245` for clip,
the exact label opacities (white @1.0 / 0.55 / 0.25), white-fill control/elevation steps, and the
SF Pro type ramp (15 px card titles, 13 px body, 11 px labels). These are the values to encode in the
JUCE `Theme`.

## Status

Mockup only. If the direction is approved, the next step is to implement it in the JUCE GUI
(`src/gui/`), mapping these decisions onto the existing components (`DevicePicker`, `CalSlotComponent`,
`CurveThumbnail`, `LevelMeter`, `Theme`, `MainComponent`).
