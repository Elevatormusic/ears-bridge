# Design tokens — distilled from the macOS 26 UI kit

Extracted from the Apple **macOS 26 UI kit** design-tokens export (`apple-macos-27-ui-kit.tokens.json`,
provided by the project owner; sourced from the Apple/Sketch UI kit). These are the values to encode
in the JUCE `Theme` so the app matches the system. Apple's fonts (SF Pro/SF Mono) are licensed for
Apple-platform UI only — ship the OS system font on each platform, not a bundled Apple font.

## Type ramp (SF Pro)

| Role | Size | Weight (regular · emphasized) |
|---|---|---|
| Large Title | 26 px | 400 · 700 |
| Title 1 | 22 px | 400 · 700 |
| Title 2 | 17 px | 400 · 700 |
| Title 3 | 15 px | 400 · 600 |
| Headline | 13 px | 700 · 900 |
| Body | 13 px | 400 · 600 |
| Callout | 12 px | 400 · 600 |
| Subheadline | 11 px | 400 · 600 |
| Footnote | 10 px | 400 · 600 |
| Caption 1 / 2 | 10 px | 400 · 600 |

Emphasis weight is **600** (semibold), not 700. Tracking is 0 at these sizes.

## Labels (text) — opacity over the background

| Level | Light | Dark |
|---|---|---|
| Primary | black @ 0.85 | white @ 1.0 |
| Secondary | black @ 0.50 | white @ 0.55 |
| Tertiary | black @ 0.25 | white @ 0.25 |
| Quaternary | black @ 0.10 | white @ 0.10 |

(Use Primary/Secondary for real text — Tertiary @0.25 is below the 4.5:1 contrast floor for body text.)

## System colors

| Color | Light | Dark |
|---|---|---|
| Blue (accent) | `#0088FF` | `#0091FF` |
| Green (success) | `#34C759` | `#30D158` |
| Orange (caution) | `#FF8D28` | `#FF9230` |
| Yellow | `#FFCC00` | `#FFD600` |
| Red (error / clip) | `#FF383C` | `#FF4245` |
| Teal | `#00C3D0` | `#00D2E0` |
| Indigo | `#6155F5` | `#6D7CFF` |

The current system **blue is `#0091FF`** (dark), not the older `#0A84FF`. Apple has no separate "amber"
— caution uses **Orange**.

## Surfaces & fills

| Token | Light | Dark |
|---|---|---|
| Window background | `#FFFFFF` | `#1E1E1E` |
| System gray | `#8E8E93` | `#98989D` |
| Control fill (primary → quinary) | black @ 0.10 / 0.08 / 0.05 / 0.03 / 0.02 | white @ 0.10 / 0.08 / 0.05 / 0.03 / 0.02 |
| Separator | `#3C3C43` @ 0.29 | white @ ~0.12 |

Elevation is a translucent **white fill step** over the base (e.g. a card ≈ base + white @0.05), not a
heavier border — which is why the mockup uses fill-step cards with minimal outlines.

## Applied in `mockup-b.html`

Dark: bg `#1E1E1E`, cards `#2A2A2A` (fill step), controls white @0.10, separators white @0.12,
accent `#0091FF`, success `#30D158`, caution `#FF9230`, clip `#FF4245`, text white @1.0/0.55/0.25,
SF Pro ramp (15 px card titles, 13 px body, 11 px labels, 10 px captions). A matching light palette
(window `#FFFFFF`, accent `#0088FF`, etc.) follows the same table.
