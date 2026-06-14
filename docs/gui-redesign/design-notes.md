# EARS Bridge — GUI redesign (design notes)

A proposed redesign of the desktop window, applying `docs/GUIDesignGuide.md` (the Apple-HIG-derived,
framework-neutral guideline). This is a **design mockup** ([`mockup.html`](mockup.html)) for review —
not yet implemented in the JUCE code. Open `mockup.html` in a browser to see it full size.

It keeps the existing functionality (input, two per-ear calibrations, combine mode, output sink,
sample rate, bit depth, level meters, clean-capture status, Start/Stop, and the Advanced disclosure)
and restructures the presentation.

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

The bottom bar shows the input meters as a **pre-routing monitor** so levels can be checked before
pressing Start. (Today the engine only meters while running.) This is a small usability addition, not
required by the current code.

## Status

Mockup only. If the direction is approved, the next step is to implement it in the JUCE GUI
(`src/gui/`), mapping these decisions onto the existing components (`DevicePicker`, `CalSlotComponent`,
`CurveThumbnail`, `LevelMeter`, `Theme`, `MainComponent`).
