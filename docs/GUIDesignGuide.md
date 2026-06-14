# GUIdesignguideline.md — A Program-Agnostic UI/UX Design Guideline

*Grounded in Apple's Human Interface Guidelines (HIG) and Apple Developer Design Resources, translated into framework-neutral rules. First applied to "EARS Bridge," a cross-platform (macOS + Windows) C++/Qt-style pro-audio desktop utility. Reusable across future desktop projects.*

---

## 0. How to Use This Document

This guideline distills Apple's design wisdom into portable rules you can apply in any toolkit — C++, Qt, Dear ImGui, a custom renderer, web, etc. Each area gives: **(a) the Apple principle/rationale**, **(b) a portable, framework-neutral rule**, and **(c) an EARS Bridge illustration** where useful.

Apple's HIG and resources are copyrighted; everything here is reworded in original phrasing. Concrete numeric specifics (contrast ratios, control sizes, type ramps) are stated as facts.

**Where Apple is platform-specific, substitute portably:**
- SF Pro / New York fonts → licensed for Apple-platform UI work only; use system font stacks instead (see §4).
- SF Symbols → Apple-platforms-only license; use an open icon library (see §6).
- Apple semantic color tokens → define your own equivalent token set (see §5).
- Dynamic Type → build a scalable type ramp and honor OS text-scaling (see §4, §2).

---

## 1. Foundational Principles

**Apple principle.** Apple's classic themes are **Clarity** (legible text at every size, precise icons, function-driven design), **Deference** (UI supports content rather than competing with it), and **Depth** (visual layers and motion convey hierarchy). Apple's current framing adds **Hierarchy**, **Harmony** (align with the platform and hardware), and **Consistency** (reuse familiar patterns to reduce cognitive load).

**Portable rules.**
1. **Clarity first.** Every control must be self-evident. A button labeled with its action ("Start Routing") beats a vague one ("Go").
2. **Defer to content.** Chrome recedes; the user's data is the star. In EARS Bridge, the calibration frequency-response plots are the content — the surrounding labels, dropdowns, and borders must be quieter than the plotted curve.
3. **Convey depth/hierarchy** with layering, spacing, and weight — not decoration.
4. **Be consistent** within the app and with each host OS.
5. **Prefer standard components** over custom ones; you inherit correct focus, keyboard, and accessibility behavior for free.

---

## 2. Accessibility

**Apple principle.** Accessibility is a first-class, structural requirement, not a finishing pass. Apple's pillars: sufficient contrast, scalable text (Dynamic Type), never relying on color alone, screen-reader labels (VoiceOver), reduced motion, and adequate hit-target sizes.

**Portable rules & concrete specifics.**
- **Contrast ratios.** Apple follows the W3C/WAI Web Content Accessibility Guidelines (WCAG 2.2):
  - Body/normal text: **minimum 4.5:1** against background — WCAG SC 1.4.3 Contrast (Minimum), Level AA.
  - Large text (≥ ~18pt regular or ~14pt bold): **minimum 3:1** — also SC 1.4.3, Level AA.
  - Non-text UI (control boundaries, the "on" vs "off" state of a custom toggle, meter fills, graph lines): **minimum 3:1** — WCAG SC 1.4.11 Non-text Contrast, Level AA.
  - For custom foreground/background, **aim for 7:1**, especially for small text — WCAG SC 1.4.6 Contrast (Enhanced), Level AAA.
  - Re-check contrast in **both** light and dark themes — passing in one does not guarantee the other.
- **Scalable text.** Don't hardcode pixel font sizes that can't scale. Define a type ramp by *role* (see §4) and let it scale with the OS text-size / display-scaling setting. Support enlargement to **~200%** of default for common tasks.
- **Never rely on color alone.** Color vision deficiency affects about **8% of men and 0.5% of women — roughly 1 in 12 men** (per the National Eye Institute / NIH: "about 8 percent of men with Northern European ancestry have red-green color blindness, compared to 0.5 percent of women"). Always pair color with a second cue — text, icon, shape, or position. In EARS Bridge: the inline output error must read as red **and** carry an error glyph and explicit message; the "clean" status should say "clean" (and/or show a check) rather than relying on green alone; the yellow non-native-rate caution needs caution text, not just amber coloring.
- **Screen-reader labels.** Every interactive element — including icon-only buttons and meters — needs a descriptive accessible name and value. Level meters should expose a value (e.g., "Output level, −6 dB"); the Advanced checkbox should announce its expanded/collapsed state.
- **Focus order & visible focus.** Keyboard focus must move in a logical reading order and always show a visible focus ring. Never remove focus indicators.
- **Hit-target sizes.** Per Apple's HIG: "a button needs a hit region of at least **44×44 pt** — in visionOS, **60×60 pt** — to ensure that people can select it easily, whether they use a fingertip, a pointer, their eyes, or a remote."
  - Touch (iOS/iPadOS): **44×44 pt** minimum.
  - visionOS (eye/hand): **60×60 pt**.
  - Pointer-driven desktop (macOS/Windows): Apple still cites 44×44 pt as the comfortable minimum; the absolute floor is **24×24 CSS px**, set by WCAG 2.2 SC 2.5.8 Target Size (Minimum), Level AA ("The target is at least 24 by 24 CSS pixels"). Use that only as a hard floor for icon-only controls, with comfortable padding and spacing between adjacent targets.
- **Reduced motion.** Honor the OS "reduce motion" setting (macOS: Reduce Motion; Windows: "Show animations"). Replace large/zoom/parallax animations with a dissolve or instant change. Don't strip meaningful transitions entirely — substitute a calmer one.
- **Respect other OS accessibility settings:** increased contrast, reduced transparency, and bold text.

---

## 3. Layout & Spacing

**Apple principle.** A consistent, adaptive layout makes apps approachable. Use visual hierarchy, grouping, alignment, generous whitespace, safe-area margins, and readable content widths so primary content stays in focus.

**Portable rules & concrete specifics.**
- **Adopt an 8-point spacing grid.** Use multiples of 8 (8, 16, 24, 32, 40, 48…) for margins, padding, and gaps; 4pt is the allowed half-step for fine adjustments and dense, data-rich layouts. Apple and Google both align to this system; it scales cleanly across display densities.
- **Internal ≤ external** (Gestalt proximity): padding *inside* a group should be ≤ the space *separating* groups, so groups read as distinct. In EARS Bridge, the two calibration cards each have tight internal padding but a larger gutter between them.
- **Group related controls.** Sample Rate + Output Bit Depth belong in one row/group; Input device, Combine Mode, and Output sink each form their own labeled block.
- **Align to a grid; establish clear margins.** Maintain consistent window/content margins (e.g., 16–24pt) and align labels and fields to shared columns.
- **Readable widths.** Cap the width of text/help lines (~45–75 characters) so explanatory text under the Combine Mode and Output dropdowns stays legible; don't let help text stretch the full window width.
- **Adaptive/responsive.** Layout should reflow gracefully as the window resizes; protect content from being clipped, and keep primary actions visible.
- **Prioritize primary content.** Give the most important elements — the calibration plots and the Start button — the most visual space and prominence.

---

## 4. Typography

**Apple principle.** Typography should deliver legible text, a clear information hierarchy, and brand voice. Apple's system font is **San Francisco (SF Pro)** on macOS/iOS/iPadOS/tvOS (SF Compact on watchOS), with **SF Mono** for code and **New York** as the serif companion. SF uses optical sizing: per Apple's HIG the crossover is at **20 pt** — SF Pro **Text** for sizes below 20 pt and SF Pro **Display** for 20 pt and above. Apple advises using built-in text styles, emphasizing with weight/size/color, limiting the number of typefaces, supporting Dynamic Type, and avoiding ultralight/thin weights for body text.

**Licensing reality (critical for cross-platform).** SF Pro, SF Compact, SF Mono, and New York are licensed by Apple **solely for designing/mocking up UI for software running on Apple operating systems**. The license text states the font is "to be used solely for creating mock-ups of user interfaces to be used in software products running on Apple's iOS, iPadOS, macOS or tvOS operating systems," and explicitly forbids using it for non-Apple OS UI or embedding it "in any software programs or other products." You may **not** embed these fonts in a Windows build or a cross-platform binary. Use them only for macOS-targeted mockups; ship system fonts instead.

**Portable rules.**
- **Use a system-font stack** so each OS renders its native UI face:
  - macOS → San Francisco (via the system-UI font, not a bundled file).
  - Windows → **Segoe UI** (Windows 11 ships Segoe UI Variable).
  - Linux → a default sans (e.g., Cantarell/Noto/DejaVu).
  - Web/CSS analog: `system-ui, -apple-system, "Segoe UI", Roboto, sans-serif`.
  - For monospaced/tabular data (serials, dB values): a mono stack (SF Mono → Cascadia Code/Consolas → monospace). Tabular/monospaced figures keep numeric columns aligned in meters and the calibration metadata line.
- **Define a type ramp by role**, not by raw pixel sizes, so it scales. A sensible desktop ramp (portable, modeled on Apple's macOS text styles, which are tighter than iOS):

  | Role | macOS HIG size (pt) | Weight | Line height (pt) | EARS Bridge use |
  |---|---|---|---|---|
  | Large Title | 26 | Regular | 32 | App title / first-run header |
  | Title 1 | 22 | Regular | 26 | Major section headers |
  | Title 2 | 17 | Regular | 22 | Card titles |
  | Title 3 | 15 | Regular | 20 | Sub-card headers |
  | Headline | 13 | Bold | 16 | Emphasized labels |
  | Body | 13 | Regular | 16 | Default control text |
  | Callout | 12 | Regular | 15 | Secondary control text |
  | Subheadline | 11 | Regular | 14 | Uppercase section labels (INPUT, OUTPUT) |
  | Footnote | 10 | Regular | 13 | Helper/explanatory lines |
  | Caption 1/2 | 10 | Regular/Medium | 13 | Meter tick labels, fine print |

  *(iOS values are larger — Body 17pt, Large Title 34pt — because of touch viewing distance. For a desktop app use the macOS-scale ramp above; scale the whole ramp proportionally with the OS text-size setting. Note: Apple's macOS table is published "based on image resolution of 144ppi for @2x" — the point values above are the @1x values you use in code.)*
- **Minimum legible size:** keep functional text at/above roughly **10–11pt** (desktop) / 11pt (iOS minimum); don't shrink below that to fit.
- **Limit typefaces.** One UI family plus an optional mono for data. Mixing many faces looks fragmented.
- **Emphasize with weight/size/color**, preserving the relative hierarchy when text scales. Prefer Regular/Medium/Semibold/Bold; avoid Ultralight/Thin/Light for UI text (poor legibility at small sizes and low vision).
- EARS Bridge's uppercase section labels (e.g., "LEFT EAR CAL") are a legitimate device for quiet, scannable headers — keep them small, tracked slightly looser, and in a muted/secondary color.

---

## 5. Color

**Apple principle.** Use color judiciously — to communicate, show status/feedback, and provide continuity — never as the only signal. Apple ships **semantic system colors** (e.g., label, secondaryLabel, systemBackground, systemBlue/Red/Green/Yellow) that automatically adapt to light/dark mode and accessibility settings, plus an accent color. Apps should support both light and dark appearance and not offer a redundant in-app appearance toggle that fights the system setting.

**Portable rules.**
- **Define your own semantic token set** (don't hardcode raw hex throughout). Name tokens by *role*, and provide a light and a dark value for each:
  - Text: `text.primary`, `text.secondary`, `text.tertiary`, `text.disabled`
  - Surfaces: `bg.base`, `bg.elevated`, `surface.grouped`, `separator`
  - Accent: `accent` (one brand/primary action color)
  - Semantic status: `status.info`, `status.success`, `status.warning`, `status.error`
- **One accent color, used sparingly** — reserve the most prominent fill for the single primary action per view. In EARS Bridge the blue data-visualization line and the Start button's accent should feel related and intentional, not competing with a dozen other saturated colors.
- **Semantic status colors with redundancy:**
  - `status.error` (red) → the inline output-sink error.
  - `status.warning` (amber/yellow) → the non-native sample-rate caution.
  - `status.success` (green) → "clean" status.
  - Always pair with text/icon (see §2). Don't reuse one color for two different meanings.
- **Support light & dark themes.** Think of dark mode as "dimming the lights," not inverting: backgrounds get darker, foregrounds brighter; elevated surfaces are slightly *lighter* than the base (not the same). EARS Bridge is dark-themed today; still define a coherent light palette for portability and for users who prefer light.
- **Follow the OS appearance** by default (macOS Appearance setting; Windows light/dark mode). Avoid an app-only toggle unless you also respect the system default.
- **Contrast:** verify every token pair meets §2 ratios in both themes. Blue data lines on a dark background must clear 3:1 as non-text content.
- **Reference hex values** for a familiar, accessible base palette (define light/dark variants): blue `#007AFF`/`#0A84FF`, red `#FF3B30`/`#FF453A`, green `#34C759`, orange `#FF9500`, yellow `#FFCC00`. These are starting points — re-verify contrast for your surfaces; you are not licensed to claim them as "Apple" colors, but the numeric values themselves are not protected.

---

## 6. Icons & Images

**Apple principle.** Per Apple Developer, "SF Symbols is a library of over 7,000 symbols designed to integrate seamlessly with San Francisco… Symbols come in nine weights and three scales" (SF Symbols 7, released June 2025, ships over 6,900 symbols). Symbols match adjacent text weight and align to the text baseline, scaling with Dynamic Type. Icons should be simple, recognizable, consistent in weight/detail/perspective, and convey a single concept. Note: SF Symbols may be used **only in apps on Apple platforms**, and symbols depicting Apple products/features may not be modified.

**Portable rules.**
- **Use an open, license-clean icon library** for cross-platform builds — e.g., Lucide/Feather, Material Symbols, Tabler, Phosphor, or Fluent UI System Icons (check each license). Don't ship SF Symbols in your Windows build.
- **Match icon stroke weight to adjacent text weight** and scale icons with the text ramp, so a 13pt label and its icon look like a matched pair.
- **Keep a single visual style** across your icon set: uniform stroke width, corner radius, level of detail, and perspective.
- **Optically center** icons (adjust padding when an icon's visual weight is lopsided) and give icon-only buttons enough padding to meet hit-target minimums (§2).
- **Pair icons with text or accessible labels;** icon meaning can be ambiguous (a checkmark may read as "confirm" or "select").
- Use vector formats (SVG/PDF) so icons stay crisp on high-DPI displays.

---

## 7. Materials, Vibrancy & Depth

**Apple principle.** On Apple platforms, **materials** add translucency + background blur to separate foreground from background; **vibrancy** pulls color from behind a material to keep text/symbols legible on it. The system defines materials of varying thickness (ultra-thin → thick) that adapt to light/dark. Thicker materials blur more and signal stronger foreground focus. Apple's 2025 "Liquid Glass" extends this with depth, refraction, and specular highlights — but it carries real legibility and performance costs and must honor Reduce Transparency / Reduce Motion / Increase Contrast.

**Portable rules.**
- **Convey depth with a simple elevation system** rather than chasing platform-specific glass: base surface → elevated surface (slightly lighter in dark mode) → overlay. Use subtle shadows and/or 1–2 background-tint steps; avoid heavy borders.
- **If you use translucency/blur**, keep it on chrome (sidebars, toolbars, popovers/overlays) — not on dense content — and ensure text over it still meets contrast. Provide a **solid fallback** when the OS requests reduced transparency.
- **Layer economy:** at most one primary translucent surface per view; don't stack translucent panes. Place buttons/text on solid fills nested inside a glass region.
- **Don't pick a material by its apparent color** — pick by purpose; system settings can change appearance.
- For EARS Bridge, a flat dark theme with a clear two-step elevation (window base vs. raised cards/meters) and crisp separators is more robust and portable than glass effects.

---

## 8. Motion

**Apple principle.** Motion should convey status, feedback, and spatial relationships — never decoration for its own sake. It should feel realistic/credible (a view dismissed the way it appeared), avoid gratuitous animation on frequent interactions, and always be optional under Reduce Motion.

**Portable rules.**
- **Animate with purpose:** state changes, transitions that explain hierarchy, and feedback. Keep durations short (~150–300ms) with natural easing.
- **Make motion optional & meaningful-only.** Under OS reduce-motion, swap large/scale/parallax animations for dissolves or instant changes; keep (calmed) transitions that carry meaning.
- **Don't animate high-frequency interactions** that users repeat constantly.
- In EARS Bridge, level meters should update smoothly and continuously (a stalled meter reads as a frozen app); the Advanced disclosure can expand with a brief, calm reveal (or instantly under reduce-motion).

---

## 9. Components & Patterns (Desktop Pro-Audio Focus)

### 9.1 Windows, panels, sidebars & window chrome (macOS vs Windows)
**Principle/rules.** Respect each OS's native window conventions while keeping one product identity.
- **Window controls:** macOS places the close/minimize/zoom "traffic lights" at the **top-left**; the green button is *zoom* (fit-to-content), **not** Windows-style maximize. Windows places minimize/maximize/close at the **top-right**, and maximize fills the screen. Let each OS draw its native title bar and controls; don't reimplement them inconsistently.
- **Menus:** macOS uses a single global menu bar at the top of the screen; Windows puts the menu bar inside the window. Plan for both — the same commands, placed per platform.
- **Settings/Preferences:** macOS convention is a modeless preferences window (changes apply immediately; ⌘, opens it; Esc/⌘. closes; minimize/zoom buttons dimmed). Windows commonly uses an OK/Cancel/Apply dialog. Match each platform's expectation.
- **File dialogs:** always use the native open/save dialogs of each OS.
- Keep a coherent identity via your shared color tokens, type ramp, spacing, and iconography — not by overriding native window furniture.

### 9.2 Menus, toolbars & primary-action placement
- Put frequent commands in a toolbar near the content they affect; put the full command set in menus.
- The **primary action** (EARS Bridge's **Start**) should be the single most visually prominent button, placed predictably (e.g., bottom-right of its action area or anchored in the main view). Only one default/primary button per view.

### 9.3 Buttons
**Principle.** A button = style + content + role. Use the most prominent (filled) style for the single most likely action; don't fill many buttons (it raises cognitive load). Use **style, not size**, to signal the preferred option. Never give a destructive action the primary style, and separate destructive buttons from safe ones.
**Portable rules.**
- Hierarchy: **Primary/default** (filled accent) > **Secondary** (tinted/outlined) > **Tertiary/plain** (text only). **Destructive** = red styling, kept apart from safe controls.
- Verb-first labels in the app's chosen case (see §10): "Start," "Stop," "Choose File…".
- **Ellipsis (…)** in a label means the action opens a dialog/needs more input before completing (e.g., "Load Calibration…").
- Define a full **state set** for every interactive control: **default, hover, pressed/active, focused (visible ring), disabled,** and where relevant **selected/on**. (Hover applies on pointer desktops; ensure focus state covers keyboard users.)
- The default button should activate on **Return/Enter**; Esc should cancel/close.

### 9.4 Selection controls: dropdowns, checkboxes, toggles, segmented, radio
- **Pop-up button / dropdown** (EARS Bridge's Input, Combine Mode, Output, Sample Rate, Bit Depth): use when picking one option from a list; show the current value in the collapsed control; keep helper text directly beneath.
- **Checkbox:** for an independent on/off (clearer than a toggle-button for two states). EARS Bridge's **Advanced** checkbox is correct usage (see §9.8).
- **Toggle/switch:** for an immediately-applied on/off setting.
- **Segmented control:** 2–~5 mutually exclusive, closely related choices shown inline; content above it applies to all segments.
- **Radio group:** a small set of mutually exclusive options when you want all visible at once.
- Use **sentence case** for checkbox/radio labels (see §10).

### 9.5 Sliders, steppers & value controls
- **Slider:** continuous value over a range (e.g., a gain trim) — show min/max context; optionally a current-value readout. Make the thumb a comfortable hit target.
- **Stepper:** small, precise incremental changes; pair with a text field showing the value for direct entry (e.g., exact dB).
- For audio gain, combine a slider (coarse) with a numeric field/stepper (precise) and show units.

### 9.6 Text fields, labels & helper text
- Label every field clearly; use placeholder/hint text to show format ("name@example.com").
- Show **errors inline, next to the field**, phrased as guidance ("Choose a password with at least 8 characters"), not scolding ("Invalid").
- Labels are static, selectable-but-not-editable text; secondary/helper lines use the muted `text.secondary` token.

### 9.7 Progress indicators, level meters & live status
- **Determinate progress** (bar/ring filling) whenever duration is knowable; **indeterminate** (spinner) only for unquantifiable waits. Don't switch shapes mid-task (spinner↔bar). Even out pacing so progress feels honest; keep it moving (a stationary indicator reads as frozen).
- **Level meters** (L/R/OUT): update continuously and smoothly; expose a numeric value to assistive tech; use color zones (e.g., green/amber/red near clip) **with** position so meaning isn't color-only.
- **Status word** ("clean"/"Error"): pair text with semantic color and, ideally, an icon; place consistently.

### 9.8 Disclosure / progressive disclosure (the "Advanced" pattern)
**Principle.** Show essentials first; reveal advanced/rarely-used options on demand. This serves novices (less clutter, fewer mistakes) and experts (less to scan). The macOS print dialog's "Show Details" is the canonical example.
**Portable rules.**
- EARS Bridge's **Advanced** checkbox is exactly right: keep the common path (input → calibration → combine → output → start) visible, and tuck advanced controls behind the toggle.
- Make the disclosure control clearly labeled (strong "information scent" about what's inside) and remember the user's last state where appropriate.
- Use a disclosure triangle/expander for inline sections; a separate pane/sheet for larger advanced sets.

### 9.9 Alerts, inline validation & error/warning messaging
**Principle.** Minimize modality — use a modal alert only when something critical needs attention or a decision. A good alert answers, at a glance from title + buttons: what happened, what are the consequences, what can I do. State the main point in one sentence; add brief detail only if needed; offer clearly-labeled, verb-based actions. Avoid "Oops/Uh-oh."
**Portable rules.**
- Prefer **inline** validation/messaging (next to the offending control) over modal dialogs. EARS Bridge's inline output error and inline rate caution are the right approach.
- Reserve modal alerts for truly blocking conditions (e.g., "Selected output device is no longer available").
- Write errors as **specific, actionable guidance** with cause + remedy; color them semantically (red error, amber caution) plus icon/text.

### 9.10 Empty states & onboarding
- When no device/calibration is loaded, show a helpful empty state: what's missing and the one action to fix it ("Select an input device to begin"), not a blank panel.
- Keep onboarding light and contextual; reveal guidance where the relevant control is.

---

## 10. Writing & UX Text

**Apple principle.** Words are part of the UX. Lead with the most important information; use concise, plain language and active voice; make controls verb-first; keep terminology consistent; and pick a capitalization style and stick to it.

**Portable rules & conventions.**
- **Lead with what matters;** cut filler. "Output device unavailable — choose another sink" beats a paragraph.
- **Active voice, verb-first controls:** "Start," "Choose File…," "Learn more about calibration" (never "Click here").
- **Capitalization — pick and be consistent.** Apple convention on desktop: **title case for menu commands, buttons, titles**; **sentence case for checkboxes, radio labels, and explanatory/secondary text**. Example: button "Load Calibration…" (title) vs. checkbox "Show advanced options" (sentence).
- **Consistent terminology & patterns:** choose one term per concept (don't alternate "sink"/"output"/"device" arbitrarily); pick first- or second-person and stick to it; standardize flow words (Continue vs. Next; end with Done).
- **Error/empty text:** specific, blame-free, with a remedy. "Sample rate 48 kHz isn't the device's native rate (44.1 kHz); audio will be resampled."
- **Punctuation/typography:** real ellipsis (…), curly quotes, proper minus/×/÷; one space after periods; terminating punctuation on full sentences in help/secondary text, not on short labels.

---

## 11. Apple Developer Design Resources & Portable Equivalents

**What Apple provides** (developer.apple.com/design/resources/):
- **UI Kits & templates** for Figma and Sketch (iOS/iPadOS, macOS, watchOS, tvOS, visionOS), plus design/production templates (Sketch/Photoshop/Figma) and **app-icon templates**.
- **SF Symbols app** (download; macOS-only) with the full symbol library, rendering modes, and a custom-symbol annotation workflow.
- **Fonts:** SF Pro, SF Compact, SF Mono, New York, and localized SF scripts (Arabic, Armenian, Georgian, Hebrew, etc.).
- **Icon Composer** for layered "Liquid Glass" app icons; **Parallax tools** for tvOS/visionOS; **product bezels** for marketing; **technology badges/logos** (Sign in with Apple, Apple Pay, etc.).

**Conventions these encode:** a fixed type ramp tied to text styles + Dynamic Type; semantic color sets with light/dark variants; an 8pt-based spacing/layout system; symbol-to-text weight matching; standardized component states and metrics.

**Portable equivalents for a non-Apple toolchain:**
- Build your **own component library** (in Figma/Penpot/code) encoding the §3 spacing grid, §4 type ramp, §5 color tokens, and §9 component states.
- Use **system fonts** (§4) and an **open icon set** (§6) in shipped binaries.
- Define **design tokens** (JSON/CSS/code constants) for color, spacing, type, and radius so macOS, Windows, and any future target stay in sync.
- For macOS-only mockups you may use SF Pro/SF Symbols under Apple's license; never embed them in the Windows build.

---

## 12. Cross-Platform Desktop Best Practices (macOS + Windows)

- **Respect native conventions, keep one identity.** Native window controls, menu placement, file dialogs, and default control styling per OS; shared tokens (color/type/spacing/icons) for identity.
- **Default fonts:** SF (system-UI) on macOS, Segoe UI on Windows — via a system stack, not bundled Apple fonts.
- **DPI / high-resolution scaling.** Support per-monitor DPI and fractional scaling (common on Windows); use vector icons and scalable layouts; test at 100/125/150/200%. Don't assume 1× pixels.
- **Keyboard navigation & shortcuts.** Full keyboard operability with logical tab order; honor platform shortcut idioms (⌘ on macOS, Ctrl on Windows; ⌘, vs. Ctrl+, for preferences). Default button on Enter, cancel on Esc.
- **Visible focus rings** everywhere; never suppress them.
- **Light/dark theme** following the OS; verify contrast in both.
- **Consistent spacing system** (8pt grid) shared across platforms.
- **Title bar / chrome behavior** differs (traffic lights vs. right-side controls; zoom vs. maximize) — adopt each platform's behavior rather than forcing one.

---

## 13. Review Checklists

### Accessibility checklist
- [ ] Body text ≥ 4.5:1 contrast; large text ≥ 3:1; non-text UI ≥ 3:1 — verified in light **and** dark.
- [ ] Text scales with OS setting (no clipped/broken layouts at ~200%).
- [ ] No meaning conveyed by color alone (text/icon/shape backup) — errors, cautions, "clean" status, meter zones.
- [ ] Every interactive element + meter has an accessible name and value.
- [ ] Logical focus order; visible focus ring on all controls.
- [ ] Hit targets meet minimums (≥24×24 px hard floor / 44×44 pt comfortable desktop & touch) with spacing.
- [ ] Reduce-motion honored; transparency/contrast/bold-text settings respected.

### Layout checklist
- [ ] All spacing on the 8pt grid (4pt half-steps only where needed).
- [ ] Internal ≤ external spacing; related controls grouped (rate+bit depth, the two cal cards).
- [ ] Consistent window/content margins; elements aligned to a grid.
- [ ] Help/explanatory text width-capped for readability.
- [ ] Layout reflows on resize; primary content (plots, Start) gets prominence.
- [ ] One primary action per view; predictable placement.

### Writing checklist
- [ ] Leads with the most important info; concise, plain, active voice.
- [ ] Controls are verb-first; no "Click here."
- [ ] Capitalization consistent (title case: menus/buttons/titles; sentence case: checkboxes/secondary text).
- [ ] One term per concept; consistent person and flow words.
- [ ] Errors are specific, blame-free, with cause + remedy, semantically colored.
- [ ] Proper typography (…, curly quotes, real math symbols); correct punctuation.

### Component/state checklist
- [ ] Each control defines default/hover/pressed/focused/disabled (+ selected/on where relevant).
- [ ] Buttons: one filled primary; destructive separated and red; ellipsis where a dialog follows.
- [ ] Determinate progress where possible; meters update continuously and expose values.
- [ ] Advanced/rarely-used options behind progressive disclosure; essentials shown first.
- [ ] Inline validation preferred over modal; modal reserved for blocking decisions.
- [ ] Native window controls, menus, and file dialogs per OS; shared tokens for identity.

---

*Sources synthesized from Apple's Human Interface Guidelines (Foundations: Accessibility, Color, Dark Mode, Typography, Layout, Materials, Motion, Writing; Components: Buttons, Sliders, Steppers, Segmented Controls, Disclosure Controls, Progress Indicators, Gauges), Apple Developer Design Resources & Fonts pages and license terms, WCAG 2.2 (W3C/WAI), the National Eye Institute, and established cross-platform desktop conventions (macOS HIG + Microsoft Windows guidance). All text is original phrasing; numeric specifics are stated as facts.*