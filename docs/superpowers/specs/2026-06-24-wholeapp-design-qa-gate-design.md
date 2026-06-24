# Whole-App Design-QA Gate — Design

**Status:** approved approach (brainstorming complete; research-validated 2026-06-24). Umbrella design + Phase-1 detail. Phases 2–4 get their own specs as we reach them.

**Goal:** Turn the header-only `EB_HIG_STATES` harness into a whole-app, future-proof layout/HIG **regression gate** that fails the build (in the existing `ctest` CI) when any visible surface — in *every* state, theme, accessibility mode, and window size — overlaps, clips, drops below the contrast floor, or shrinks below the touch-target floor; and that **reports its own blind spots** rather than green-passing them.

**Architecture (one line):** a declarative **scenario catalog** drives the real `MainComponent` headless through a shared **applier**; the vendored **probe** snapshots each state into a `native-render` descriptor; a **descriptor-consuming C++ scorer** asserts the hard categories in-suite (the build gate); an **advisory `native-review` step** owns the snapshot/coverage-honesty layer.

**Tech stack:** C++20, JUCE 8.0.x, Catch2 v3, the existing `eb_tests`/`ci.yml` ctest harness, Node (advisory step only). The apple-hig plugin's `juce_design_probe.h` is already vendored at `src/gui/juce_design_probe.h`.

---

## The honesty contract (the core principle)

This system exists to be **trustworthy**: a green gate must mean "no measured surface regressed," and anything it *cannot* measure must be **named, not hidden**. Concretely:

1. **Never green while blind.** If a known custom-painted surface is present in a state but emits zero measured sub-elements, the gate **fails** — it reports its blindness instead of passing.
2. **Consume, don't re-measure.** The gate scores the *same descriptor* `native-review.mjs` consumes. `textOverflowsOf` and `resolveColours` stay in the probe as the single measurement source; the gate ports only the *font-free* scoring math. (Re-measuring drifts on font metrics across machine/JUCE build and flips clip at the ±1 px boundary — validated.)
3. **Faithful states, not imagined ones.** Scenarios are driven by the **real formatter functions** with synthetic telemetry and **programmatically-generated worst-case strings** — never hand-typed literals (which test the author's imagination, not production).
4. **Fail closed on growth.** Coverage is derived from the live tree: a new visible surface that no scenario reaches **fails the build** unless explicitly listed as intentionally-unmeasured with a reason.
5. **Report two coverage numbers.** Accessibility-handler coverage (`axCoverageRatio`) is *not* visible-UI coverage. The advisory step headlines **measured-pixel / total-visible-area** so low custom-surface coverage can't masquerade as "well covered."

---

## Validated design decisions (resolved before planning)

These resolve the research-validation `mustFixBeforeSpec` items:

- **Gate input = the descriptor JSON.** The C++ scorer runs `hig::writeDesignProbe` once per state, parses the descriptor, and scores the parsed elements. It MUST NOT re-implement `textOverflowsOf` or `resolveColours`. (val:measure-port)
- **Fail policy is deliberately stricter than `native-review`.** `native-review.mjs` only *blocks* on high/critical (contrast `< floor-1`, duplicate-while-overlapping); overlap/clip/target-size are `medium` → `advisory-pass`. The **build gate blocks on overlap, clip, duplicate, target-size, and contrast `< floor`** — by design. We claim **finding-parity** (identical findings from identical descriptor input), and an explicitly-stricter **verdict policy**. (val:measure-port)
- **Single-sourced thresholds.** Extract the inline literals from `native-review.mjs` (24, 18, 14, 4.5, 3.0, `floor-1`, the `>2` overlap-noise, the interactive type/role regexes, the duplicate equality keys) into `hig-thresholds.json`; generate the C++ header from it and make `native-review.mjs` import it. A golden cross-check test feeds descriptor fixtures through both paths and asserts finding-set equality. (val:measure-port)
- **Hermetic construction.** `MainComponent` gets a test seam that (a) injects a **temp-dir `Settings`** (the ctor currently default-constructs `Settings settings;` → real `%APPDATA%`/`~/Library` file) and (b) **suppresses the ctor's update-check network call** (currently `if (settings.autoCheckUpdates()) updateChecker.start(...)` at `MainComponent.cpp:521`, `autoCheckUpdates()` defaults true, no guard). Mechanism: a `MainComponent::TestConfig{ juce::File settingsDir; bool disableNetwork; }` constructor overload. (val:headless)
- **Fix the dead `role` field** in `juce_design_probe.h` (declared :163, emitted :180, never assigned — a real latent bug affecting both run modes). Either populate from `h->getRole()` in the AX block or derive a fallback from `type`. Phase-1 task. (val:snapshot-headless)
- **Drive geometry through `MainComponent::resized()`**, never `layoutRail()` directly — the two-pass scrollbar-width convergence and all text/visibility-gated rows only resolve through `resized()`. Drive Advanced via `advancedToggle.setToggleState(true)` (layoutRail owns the children's visibility). (val:scenario-forcing)
- **CalSlot injection seam.** `CalSlotComponent::applyParsed` is private and `loadFromFile` requires an on-disk file; a synthetic `CalFile` can't be loaded through the public API. Add a test-only public `applyParsedForTest(const eb::CalFile&, const juce::File& displayName)` that forwards to `applyParsed`. The catalog builds `CalFile`s with worst-case serial/filename lengths per type (HEQ/HPN/RAW/Unknown/IDF) + the swap-banner via `setProblem`. (val:scenario-forcing)
- **Headless gate scope = geometry/contrast/clip/duplicate/coverage only.** `getAccessibilityHandler()` is provably null without a peer, so `role`/`value`/`checked`/`axCoverageRatio` enrichment requires the **live-shown** `EB_HIG_STATES` harness; the headless gate must not assert ax fields. Size the root (`setSize` + `resized`) before `writeDesignProbe` so `createComponentSnapshot` doesn't hit the empty-bounds early-return. (val:snapshot-headless)
- **Pin the JUCE behaviours** the gate relies on (`createComponentSnapshot` peer-independence; `getAccessibilityHandler` null-on-no-peer) to the project's JUCE tag, so a JUCE bump triggers re-validation rather than a silent green. (val:snapshot-headless)

---

## Components

- **`hig-thresholds.json`** — single source for every numeric/regex threshold. Generates `src/gui/HigThresholds.h`; imported by `native-review.mjs`.
- **`src/gui/HigDrawnItem.h`** — `struct DrawnItem { juce::Rectangle<float> rect; juce::String text; juce::Colour fg, bg; float fontPt; bool bold; };` + interface `struct HigIntrospectable { virtual std::vector<DrawnItem> higDrawnItems() const = 0; };`. The introspection seam that makes custom-painted surfaces measurable.
- **`src/gui/HigScenarios.h`** — the scenario model + **axis-product generator** (not a curated list): the bounded axes (per-ear grade band ×4, L/R independent, banner present/absent, cal type, swap-banner, update-link, advanced, chain-advisory, clip-latch, theme ×2, a11y ×{none,reduce-motion,increase-contrast,reduce-transparency}, size ×{min,large}, scale ×{1.0,1.5}). Each generated scenario carries the *inputs* the applier feeds to the real formatters.
- **`MainComponent::applyHigScenario` + `MainComponent::TestConfig`** — the shared seam: hermetic construction, then force the editor into a scenario by calling the **real formatters** with synthetic telemetry, setting both text AND visibility on gated rows, toggling Advanced, injecting cals via `applyParsedForTest`, then `resized()`.
- **`hig::writeDesignProbe` (extended)** — queries `higDrawnItems()` on `HigIntrospectable` components and emits the sub-items as measurable descriptor nodes; tags each known-custom surface with whether it was introspected (for the fail-closed-on-blindness rule); propagates `scaleFactor`. Plus the `role` fix.
- **`src/gui/HigScore.h` / `.cpp`** — the descriptor-consuming **scorer**: a byte-faithful C++ port of `native-review.mjs`'s *pure* functions (WCAG `contrastRatio`, `boxesOverlap`/`overlapDepth`, `isLarge`, `interactive`, `contains`, `identical`, the duplicate/overlap per-pair branch). Reads thresholds from `HigThresholds.h`. Pure, font-free, deterministic.
- **`tests/test_hig_layout.cpp`** — the **build gate**: constructs the real editor hermetically, iterates the scenario product × cross-cutting axes, `applyHigScenario`, `writeDesignProbe`, scores the descriptor, asserts zero overlap/clip/duplicate/sub-24px + contrast≥floor, and runs the **fail-closed coverage** assertion. Exports descriptors+PNGs as artifacts.
- **`tests/test_hig_parity.cpp`** — the **golden cross-check**: descriptor fixtures (knife-edge cases) through both `HigScore` and `native-review.mjs`; assert finding-set equality so they never drift.
- **Advisory CI step** — runs `native-review.mjs` on the exported descriptors for hierarchy / coverage-honesty / snapshot corroboration; non-blocking except snapshot-diff on custom surfaces; headlines measured-pixel coverage.

---

## Phase decomposition

Each phase ships a **working, CI-gated** gate that is strictly more honest than the last. Review between phases; each gets its own implementation plan (and Phases 2–4 their own short specs).

### Phase 1 — Headless foundations + descriptor-consuming gate (the spine)
**Deliverable:** a deterministic in-suite build gate that fails on overlap/clip/duplicate/target-size/contrast for the **stock-widget subset** of the real editor, across light/dark + the 3 a11y modes + min/large size, with parity to `native-review` guaranteed by the golden cross-check.
- `MainComponent::TestConfig` (temp Settings + no-network) + hermetic construction.
- Fix the dead `role` field in the probe.
- `hig-thresholds.json` → `HigThresholds.h` generation; refactor `native-review.mjs` to import it.
- `HigScore` C++ port of the pure scoring math (descriptor in → findings out).
- `test_hig_parity.cpp` golden fixtures (contrast at exactly 4.5/3.0; overlapDepth exactly 2 vs 3; child exactly filling parent; one-px-over label; multi-line at the 1.8× boundary; identical-and-overlapping; custom/unknown; transparent-bg needing the ancestor walk).
- `test_hig_layout.cpp` v1: one representative state, full matrix axes, assert + export.

### Phase 2 — Custom-surface instrumentation (full coverage)
**Deliverable:** the gate measures *inside* the custom surfaces; blindness fails closed.
- `HigDrawnItem.h`; implement `higDrawnItems()` on `GradeMetricDotsView`, `LevelMeter` (dB readout + CLIP tag + zone marks), `CalSlotComponent` (chip + card title + drop-zone text), `CurveThumbnail` (curve frame + axis label + "no cal loaded").
- Probe integration: emit sub-items as measurable nodes; tag known-custom surfaces; **fail if a known-custom surface emits zero sub-items**.
- Gate now asserts overlap/clip/contrast *within* these surfaces (e.g. "CLIP" must not overlap the dB number; SNR dot stays in its cell; curve inside its frame).

### Phase 3 — Formatter-driven axis-product catalog + fail-closed coverage
**Deliverable:** whole-app, production-faithful states; the system fails when it grows blind.
- `HigScenarios.h` axis-product generator; the applier driving the **real formatters** (`renderEarStatusLine`, `qualityNote`, `checkChainConfig`, `buildStartNotes`, `invalidMeasurementMessage`, the `updateDiracCableHint` branches) with synthetic telemetry; `applyParsedForTest` cals; programmatic worst-case longest strings; real-device-name fixtures.
- Drive-through-`resized()` rule; `setToggleState` for Advanced; same-tick capture / timer-frozen so Running-state pokes aren't clobbered.
- **Fail-closed coverage:** walk the full tree; every visible component with an ID must be reached by ≥1 scenario or be on `intentionally-unmeasured` (with reason); emit + diff a coverage manifest in CI.

### Phase 4 — Pixel-contrast + device-scale + drift guards + advisory wiring
**Deliverable:** the honest, complete system.
- Pixel-sampled contrast from the snapshot PNG for painted/translucent text (covers what registered colours can't).
- Device-scale matrix (≥1.0 and 1.5×); probe propagates `scaleFactor`.
- Drift guards: allowlist scoped by component-ID-pair + state with **unused-entry-fails**; `paint()` reads the shared constants (or a grep-for-bare-literals test in the governed categories).
- Advisory `native-review` CI step on exported descriptors; the **second coverage ratio** (measured-pixel/total-visible) headlined; snapshot-diff blocking for custom surfaces; output labelled "does not gate."

---

## Phase-1 file plan (for writing-plans)

**Create:** `hig-thresholds.json`, `tools/gen-hig-thresholds.*` (or a CMake codegen step) → `src/gui/HigThresholds.h`; `src/gui/HigScore.h` + `src/gui/HigScore.cpp`; `tests/test_hig_parity.cpp`; `tests/test_hig_layout.cpp`; fixtures under `tests/fixtures/hig/`.
**Modify:** `src/gui/MainComponent.h`/`.cpp` (the `TestConfig` ctor overload + no-network guard; extract the current `timerCallback` `EB_HIG_STATES` poking into a reusable path); `src/gui/juce_design_probe.h` (`role` fix); `tests/CMakeLists.txt` (register the two tests + fixtures); `<plugin>/scripts/native-review.mjs` + helpers (import `hig-thresholds.json`).
**Constraints:** Catch2 `TEST_CASE` names ASCII only (em-dash breaks ctest discovery). Build/test: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build`. Commit attribution `Elevatormusic` / `22101396+Elevatormusic@users.noreply.github.com`, no Claude co-author trailer. New feature branch off the current head; push only when asked.

## Execution model
Per project workflow: **inline TDD** (write the code yourself, test-first) with a **fresh-Opus `code-verifier`** between logic-bearing tasks; not subagent-driven. The plugin change to `native-review.mjs` is in a separate repo (`apple-hig`) the user owns — coordinate that commit separately.

## Risks / open items
- **JUCE-version coupling** — the gate relies on `createComponentSnapshot` peer-independence and `getAccessibilityHandler` null-on-no-peer; pin the tag and re-validate on bump.
- **`native-review.mjs` lives in the apple-hig plugin repo** — the thresholds-import refactor must land there; the C++ side reads the same JSON (vendored copy kept in sync by the parity test).
- **Float ULP on the contrast knife-edge** (`std::pow` vs `Math.pow`) — only matters for a ratio exactly on 3.0/4.5; documented, accepted.
- **`drawnItems()` maintenance** — a custom surface whose `paint()` changes but whose `higDrawnItems()` doesn't will drift; the fail-closed rule catches *absence*, not *staleness*. Phase-2 spec to consider a paint/introspection consistency check.

## Out of scope (now)
RTL/bidi (JUCE has none through 8); GPU/WebBrowser subtree pixels (`JUCE_WEB_BROWSER=0`); a true `verified-pass` pixel render beyond the snapshot.
