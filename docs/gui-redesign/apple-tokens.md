# Design tokens — macOS 27 "Golden Gate" (pinned copy)

Provenance: transcribed 2026-07-05 from the apple-hig plugin v1.10.0 exact-spec references
(`references/design-tokens-macos.md`, `references/control-tokens-macos.md`, both
last_verified 2026-07-01, sourced from Apple Design Resources, June 2026 / macOS 27).
SUPERSEDES the 2026-06-14 "macOS 26 UI kit" distillation that previously lived here.
Apple states these values are a design aid and may change per release — re-verify each
macOS release. Apple's fonts (SF Pro/SF Mono) are licensed for Apple-platform UI only;
ship each platform's system font, never a bundled Apple font.

## Type ramp (SF Pro, default leading; per-style emphasized weights are NON-uniform)

| Style | Size | Default wt | Emphasized wt | Leading |
|---|---|---|---|---|
| Large Title | 26 | 400 | 700 | 32 |
| Title 1 | 22 | 400 | 700 | 26 |
| Title 2 | 17 | 400 | 700 | 22 |
| Title 3 | 15 | 400 | 600 | 20 |
| Headline | 13 | 700 | 900 | 16 |
| Body | 13 | 400 | 600 | 16 |
| Callout | 12 | 400 | 600 | 15 |
| Subheadline | 11 | 400 | 600 | 14 |
| Footnote / Caption 1 / Caption 2 | 10 | 400 | 600 | 13 |

Tracking 0 at every size. Body floor 13; smallest legible 10. Headline is the only style
bolder than Regular by default. (The old copy of this file claimed "emphasis is 600, not
700" for the whole ramp — WRONG for LargeTitle/Title1/Title2 (700) and Headline (900).)

## Labels (alpha over background)

| Tier | Light (#000 a) | Dark (#FFF a) |
|---|---|---|
| 1 Primary | 0.85 | 1.0 |
| 2 Secondary | 0.5 | 0.55 |
| 3 Tertiary | 0.25 | 0.25 |
| 4 Quaternary | 0.1 | 0.1 |
| 5 Quinary | 0.05 | 0.05 |
| 6 Seximal | 0.03 | 0.03 |

Primary/Secondary only for real text (Tertiary is under the 4.5:1 floor). Vibrant label
ramps (plus-lighter/plus-darker solid greys for material surfaces) live in the reference
file — not needed until EB adopts materials (P4+).

## Fills

Primary -> Quinary: black/white @ 0.1 / 0.08 / 0.05 / 0.03 / 0.02 (same both appearances).
Control-internal Default ladder: #000 @ 0.85/0.5/0.25/0.1/0.05/0.03; Selected: #FFF @
1.0/0.55/0.25/0.1/0.05/0.025.

## System colors (macOS 27 refresh)

| Color | Light | Dark |
|---|---|---|
| Red | #FF383C | #FF4245 |
| Orange (caution) | #FF8D28 | #FF9230 |
| Yellow | #FFCC00 | #FFD600 |
| Green (success) | #34C759 | #30D158 |
| Mint | #00C8B3 | #00DAC3 |
| Teal | #00C3D0 | #00D2E0 |
| Cyan | #00C0E8 | #3CD3FE |
| Blue (accent) | #0088FF | #0091FF |
| Indigo | #6155F5 | #6D7CFF |
| Purple | #CB30E0 | #DB34F2 |
| Pink | #FF2D55 | #FF375F |
| Brown | #AC7F5E | #B78A66 |

Grays: #8E8E93 (light) / #98989D (dark). No separate "amber" — caution is Orange.

## Surfaces

Window #FFFFFF -> #1E1E1E (with-sidebar dark -> #000000 behind the sidebar). Sidebar active
#FAFAFA a0.8 / #0C0C0C a0.85. Separator #3C3C43 a0.29 (light; dark via semantic color).
Materials (Ultra Thin -> Ultra Thick): #ECECEC a0.38/0.5/0.63/0.76/0.88 light; #292929-#2C2C2C
a0.4/0.49/0.61/0.71/0.82 dark. Elevation on opaque surfaces = a translucent fill step, not a
heavier border (why EB cards are fill-step, outline-free).

## Controls (from control-tokens-macos.md)

- Sizes: DEFAULT 28x28pt, technical MINIMUM 20x20pt. mini/small/medium = rounded-rect;
  large/x-large = capsule (Liquid Glass) — EB: 28px combos/buttons, 34px capsule CTA.
- Bordered button: light #000 a0.08 idle / a0.16 clicked / a0.04 disabled; dark on #FFF —
  idle a0.07 (NOT mirrored), clicked a0.16 / disabled a0.04 (mirrored).
- Prominent (default) button: System Blue base (+ vibrant quaternary overlay in light);
  clicked adds #000/#FFF a0.1; destructive = same recipes on Red.
- Input fields: fill #FFFFFF / #1E1E1E, 1px border #000 a0.05 / #FFF a0.04; focus = 3.5px +
  1px System Blue strokes.
- Focus ring (global): two stacked #0088FF strokes, outer a0.25 + inner a0.15.
- Tracks: filled = accent, unfilled = Fills.1 Primary; disabled filled = Vibrant Quinary.
- Full per-state layered recipes (incl. Over-Glass variants): see the plugin reference
  `control-tokens-macos.md` — authoritative; not duplicated here.

## Spacing (HIG conventions)

8pt grid with 4pt half-steps (4/8/12/16/20/24). Standard inter-element gap ~8; section gap
~16-20 (macOS density may compress); margins 16/20. 28 is a control size, not a grid step.

## EB adoption ledger (what the app actually encodes, P2 Task 10)

Stage title = Title1 (22 Bold, 26 row). Eyebrows = Subheadline (11 Bold + tracking, 14 row).
Lead/body = Body 13 (2-line reserve 34). Hints = Callout 12 (line unit 16, 2-line reserve 32).
Combos/buttons 28; CTA capsule 34; disclosure row 24 (>= 24px WCAG target floor). Card pad
16x12; card gap 8; intra-group 4; inter-group 8. StageHeader 96 (12 top pad). Fold budget at
900x720: stage viewport = 546px — see the T10 plan's height ledger. Colour tokens: NOT yet
retokenized to this file (Theme.cpp frozen until P4 M2/M3/M4).
