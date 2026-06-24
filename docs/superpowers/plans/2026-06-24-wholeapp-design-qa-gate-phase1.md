# Whole-App Design-QA Gate — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **This project executes INLINE (TDD, you write the code) with a fresh-Opus `code-verifier` subagent between logic-bearing tasks.**

**Goal:** A deterministic, in-suite C++ build gate that fails `ctest` when the real `MainComponent` — across light/dark × the three a11y modes × min/large size — has any overlap, clipped label, duplicate, sub-24px target, or below-floor contrast among its **stock (introspectable) widgets**, with finding-parity to `native-review.mjs` guaranteed by a golden fixture test.

**Architecture:** The gate constructs the real editor headless (hermetic `TestConfig`: temp-dir `Settings` + no network), drives it to a state, runs the vendored probe `hig::writeDesignProbe` to emit a `native-render` descriptor, and scores the **parsed descriptor** with `eb::hig::scoreDescriptor` — a byte-faithful C++ port of `native-review.mjs`'s *font-free* scoring math. The probe stays the single measurement source (no re-measuring `textOverflows`/colours). Thresholds are single-sourced in `hig-thresholds.json` (consumed by `native-review.mjs`) and mirrored in `HigThresholds.h`, bound by golden fixtures.

**Tech Stack:** C++20, JUCE 8.0.x, Catch2 v3, the existing `eb_tests`/`ci.yml` ctest harness. Node (only the apple-hig `native-review.mjs` edit + the advisory step).

## Global Constraints

- Build/test: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build`. Baseline before Phase 1 = **472/472**.
- Catch2 `TEST_CASE` names **ASCII only** (an em-dash breaks ctest discovery). Use `-` not `—`.
- New test sources are registered in `tests/CMakeLists.txt` inside `add_executable(eb_tests …)`. `eb_tests` already links `eb_gui` and sets `JUCE_WEB_BROWSER=0`.
- The gate **consumes the probe descriptor**; it MUST NOT re-implement `textOverflowsOf` or `resolveColours`.
- The gate's fail policy is **deliberately stricter** than `native-review`'s advisory: it blocks on overlap, clip, duplicate, target-size, AND contrast `< floor`. We claim **finding-parity** (identical findings from identical descriptor input), not verdict-parity.
- Headless gate scope = geometry/contrast/clip/duplicate/coverage ONLY. Do NOT assert `role`/`value`/`checked`/`axCoverageRatio` (null without a peer).
- Commit attribution `Elevatormusic` / `22101396+Elevatormusic@users.noreply.github.com`; **no** Claude co-author trailer. Branch off the current head (`feat/autoperear-hardening`); push only when asked.
- The `native-review.mjs` edit is in a **separate repo** (`C:/Users/Shaya/.claude/plugins/cache/apple-hig/apple-hig/1.7.0/`); commit it there separately.

---

## File Structure

- **`src/gui/juce_design_probe.h`** (modify) — fix the dead `role` field (Task 1).
- **`<apple-hig>/scripts/hig-thresholds.json`** (create) + **`native-review.mjs`** (modify) — extract inline literals into a JSON, read it, add `--thresholds <path>` (Task 2).
- **`src/gui/HigThresholds.h`** (create) — C++ mirror of the JSON constants (Task 3).
- **`src/gui/HigScore.h`/`.cpp`** (create) — `eb::hig::scoreDescriptor(const juce::var&) -> std::vector<Finding>`, the font-free scorer (Tasks 3–5).
- **`src/gui/MainComponent.h`/`.cpp`** (modify) — `TestConfig` + delegating ctors + no-network guard (Task 6).
- **`tests/test_hig_score.cpp`** (create) — unit tests for `scoreDescriptor` (Tasks 3–4).
- **`tests/test_hig_parity.cpp`** (create) + **`tests/fixtures/hig/*.json`** — golden parity fixtures (Task 5).
- **`tests/test_hig_layout.cpp`** (create) — the gate over the real editor (Task 7).
- **`tests/CMakeLists.txt`** (modify) — register the three new test sources + the fixtures dir define.
- **`src/gui/CMakeLists.txt`** (modify, if `eb_gui` lists sources) — add `HigScore.cpp`.

---

## Task 0: Branch + clean baseline

**Files:** none (git only).

- [ ] **Step 1: Branch off the current head**

```bash
cd /c/Users/Shaya/OneDrive/Documents/EARS_program
git checkout -b feat/wholeapp-design-qa-phase1
```

- [ ] **Step 2: Verify the clean baseline**

Run: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build`
Expected: `100% tests passed, 0 tests failed out of 472`.

---

## Task 1: Fix the dead `role` field in the probe (+ first headless-probe smoke test)

**Files:**
- Modify: `src/gui/juce_design_probe.h` (the `elementVar` block, lines ~162-180)
- Test: `tests/test_hig_score.cpp` (new — first TEST_CASE)

**Interfaces:**
- Produces: every descriptor element now carries a non-empty `role` derived from `type` (and overridden by the accessibility handler when present). Proves headless `describeComponentTree` works.

**Context:** `role` is declared at `juce_design_probe.h:163` (`juce::String role;`) and emitted at `:180` (`o->setProperty ("role", role)`) but **never assigned** — always empty, in both run modes. `interactive()` in `native-review.mjs` ORs role with type, so this is latent, not parity-affecting (both sides read the same descriptor), but it must be fixed so the field is meaningful.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_hig_score.cpp
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/juce_design_probe.h"

TEST_CASE("probe: a TextButton emits a non-empty role and a valid descriptor") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::TextButton b ("Go");
    b.setSize (80, 30);
    const auto json = hig::describeComponentTree (b);
    auto v = juce::JSON::parse (json);
    REQUIRE (v.isObject());
    auto* elements = v.getProperty ("elements", {}).getArray();
    REQUIRE (elements != nullptr);
    REQUIRE (elements->size() >= 1);
    const auto& root = elements->getReference (0);
    CHECK (root.getProperty ("type", {}).toString() == "TextButton");
    CHECK (root.getProperty ("role", {}).toString().isNotEmpty());   // was always "" before the fix
}
```

- [ ] **Step 2: Register the test source and run it (expect FAIL)**

Add `test_hig_score.cpp` to `add_executable(eb_tests …)` in `tests/CMakeLists.txt` (alphabetically near the other `test_hig_*`).
Run: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build -R hig`
Expected: FAIL on the `role` CHECK (empty string).

- [ ] **Step 3: Implement the role fix**

In `src/gui/juce_design_probe.h`, add a helper above `elementVar`:

```cpp
    // ARIA-ish role derived from the JUCE type, so `role` is meaningful even headless (no peer). The
    // accessibility handler overrides it below when a peer is attached. Mirrors the strings native-review's
    // interactive() role regex looks for (button|link|slider|checkbox).
    inline juce::String roleOf (const juce::String& type)
    {
        if (type == "ToggleButton")                         return "checkbox";
        if (type == "TextButton" || type == "Button")       return "button";
        if (type == "Slider")                               return "slider";
        if (type == "ComboBox")                             return "combo";
        if (type == "Label" || type == "TextEditor")        return "text";
        return {};
    }
```

Then in `elementVar`, replace `juce::String role;` (line ~163) with `juce::String role = roleOf (type);` and, inside the `#if … if (auto* h = c.getAccessibilityHandler())` block, add `role = h->getRole() == juce::AccessibilityRole::ignored ? role : juce::String();` is NOT needed — keep it simple: leave the type-derived role (the handler's role enum→string mapping is out of Phase-1 scope; the type fallback is sufficient and headless-safe). The single change is `juce::String role = roleOf (type);`.

- [ ] **Step 4: Run the test (expect PASS)**

Run: `./tools/dev.cmd ctest --test-dir build -R hig`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/gui/juce_design_probe.h tests/test_hig_score.cpp tests/CMakeLists.txt
git commit -m "fix(probe): populate the dead role field from component type + headless probe smoke test"
```

> **VERIFIER GATE** after Task 1.

---

## Task 2: Single-sourced thresholds JSON + `native-review.mjs` reads it (apple-hig repo)

**Files (apple-hig repo `C:/Users/Shaya/.claude/plugins/cache/apple-hig/apple-hig/1.7.0/scripts/`):**
- Create: `hig-thresholds.json`
- Modify: `native-review.mjs`

**Context:** `native-review.mjs` hard-codes `18`, `14`, `4.5`, `3`, `floor-1`, `24`, the `>2` overlap-noise, and the interactive type/role regexes inline. Extract them so the C++ side can mirror exact values.

- [ ] **Step 1: Create the JSON (the single source)**

```json
{
  "contrastFloorNormal": 4.5,
  "contrastFloorLarge": 3.0,
  "largeFontPt": 18,
  "largeBoldFontPt": 14,
  "highSeverityDelta": 1.0,
  "minTargetPx": 24,
  "overlapNoisePx": 2,
  "interactiveTypeRegex": "button|toggle|slider|combo",
  "interactiveRoleRegex": "button|link|slider|checkbox"
}
```

- [ ] **Step 2: Make `native-review.mjs` read it (default bundled, override via `--thresholds <path>`)**

Near the top of `native-review.mjs`, after the imports:

```js
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
const _here = dirname(fileURLToPath(import.meta.url));
function loadThresholds(argv) {
  const i = argv.indexOf('--thresholds');
  const path = i >= 0 ? argv[i + 1] : join(_here, 'hig-thresholds.json');
  return JSON.parse(readFileSync(path, 'utf8'));
}
const T = loadThresholds(process.argv);
```

Replace the inline literals: `isLarge` → `el.fontPt >= T.largeFontPt || (el.bold && el.fontPt >= T.largeBoldFontPt)`; the contrast floor → `isLarge(el) ? T.contrastFloorLarge : T.contrastFloorNormal`; the high-severity test `ratio < floor - 1` → `ratio < floor - T.highSeverityDelta`; the overlap test `overlapDepth(ra, rb) > 2` → `> T.overlapNoisePx`; the target-size `< 24` → `< T.minTargetPx`; `interactive()`'s two regexes → `new RegExp(T.interactiveTypeRegex, 'i')` / `new RegExp(T.interactiveRoleRegex, 'i')` (build them once at module scope).

- [ ] **Step 3: Verify behavior is unchanged on a fixture**

Run: `node native-review.mjs <any existing hig-states/*.json from the EARS repo>`
Expected: identical findings/verdict to before the refactor (the JSON holds the same values).
Then run with a tweaked copy: `node native-review.mjs <descriptor> --thresholds /tmp/loose.json` (set `minTargetPx: 1`) and confirm the target-size findings drop — proving the flag wires through.

- [ ] **Step 4: Commit (in the apple-hig repo)**

```bash
cd "C:/Users/Shaya/.claude/plugins/cache/apple-hig/apple-hig/1.7.0"
git add scripts/hig-thresholds.json scripts/native-review.mjs
git commit -m "feat(native-review): single-source thresholds in hig-thresholds.json + --thresholds override"
```

(If that path is not a git repo, note it for the user to land in the published apple-hig repo; the EARS-side work does not block on it because the parity test pins values via fixtures.)

---

## Task 3: `HigThresholds.h` + `HigScore` contrast findings

**Files:**
- Create: `src/gui/HigThresholds.h`, `src/gui/HigScore.h`, `src/gui/HigScore.cpp`
- Modify: `src/gui/CMakeLists.txt` (add `HigScore.cpp` to `eb_gui` if it lists sources)
- Test: `tests/test_hig_score.cpp`

**Interfaces:**
- Produces: `eb::hig::Finding { juce::String category, severity, element, message; }`; `std::vector<eb::hig::Finding> eb::hig::scoreDescriptor (const juce::var& descriptorRoot);` (parses `descriptorRoot.getProperty("elements", …)`). This task implements the **contrast** category only; Tasks 4–5 extend the same function.

- [ ] **Step 1: Create `HigThresholds.h` (mirror of the JSON)**

```cpp
#pragma once
namespace eb::hig {
// Mirror of hig-thresholds.json (consumed by native-review.mjs). The golden parity test
// (test_hig_parity) fails if these drift from native-review's behaviour. Keep in sync.
inline constexpr double kContrastFloorNormal = 4.5;
inline constexpr double kContrastFloorLarge  = 3.0;
inline constexpr double kLargeFontPt         = 18.0;
inline constexpr double kLargeBoldFontPt     = 14.0;
inline constexpr double kHighSeverityDelta   = 1.0;
inline constexpr int    kMinTargetPx         = 24;
inline constexpr int    kOverlapNoisePx      = 2;
} // namespace eb::hig
```

- [ ] **Step 2: Write the failing contrast test**

```cpp
// add to tests/test_hig_score.cpp
#include "gui/HigScore.h"

static juce::var makeDescriptor (juce::Array<juce::var> els) {
    auto* top = new juce::DynamicObject();
    auto* meta = new juce::DynamicObject();
    top->setProperty ("meta", juce::var (meta));
    top->setProperty ("elements", els);
    return juce::var (top);
}
static juce::var el (const juce::String& type, const juce::String& label,
                     int x, int y, int w, int h,
                     const juce::String& fg = "#000000", const juce::String& bg = "#ffffff",
                     bool measurable = true, bool fgOk = true, bool bgOk = true,
                     double fontPt = 13.0, bool bold = false, bool textOverflows = false,
                     const juce::String& role = "text") {
    auto* o = new juce::DynamicObject();
    auto* b = new juce::DynamicObject();
    b->setProperty("x",x); b->setProperty("y",y); b->setProperty("w",w); b->setProperty("h",h);
    o->setProperty("id", type+label); o->setProperty("type", type); o->setProperty("role", role);
    o->setProperty("label", label); o->setProperty("value", juce::String());
    o->setProperty("bounds", juce::var(b)); o->setProperty("fg", fg); o->setProperty("bg", bg);
    o->setProperty("fgIntrospectable", fgOk); o->setProperty("bgIntrospectable", bgOk);
    o->setProperty("fontPt", fontPt); o->setProperty("bold", bold);
    o->setProperty("visible", true); o->setProperty("showing", true); o->setProperty("enabled", true);
    o->setProperty("measurable", measurable); o->setProperty("textOverflows", textOverflows);
    return juce::var(o);
}

TEST_CASE("HigScore: low-contrast measurable label is a contrast finding") {
    // grey #777 on white ~4.48:1 < 4.5 normal floor -> medium contrast finding
    auto d = makeDescriptor ({ el ("Label", "hi", 0, 0, 50, 18, "#777777", "#ffffff") });
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1);
    CHECK (f[0].category == "contrast");
    CHECK (f[0].severity == "medium");
}
TEST_CASE("HigScore: custom/non-introspectable nodes are never contrast-scored") {
    auto d = makeDescriptor ({ el ("custom/unknown", "x", 0, 0, 50, 18, "not introspectable",
                                   "not introspectable", /*measurable*/false, /*fgOk*/false, /*bgOk*/false) });
    CHECK (eb::hig::scoreDescriptor (d).empty());
}
```

- [ ] **Step 3: Run it (expect FAIL — `scoreDescriptor` undefined)**

Run: `./tools/dev.cmd cmake --build build --target eb_tests`
Expected: link/compile error (no `HigScore.h`).

- [ ] **Step 4: Implement `HigScore` (contrast slice)**

`src/gui/HigScore.h`:
```cpp
#pragma once
#include <juce_core/juce_core.h>
#include <vector>
namespace eb::hig {
struct Finding { juce::String category, severity, element, message; };
// Score a parsed native-render descriptor (the probe's JSON) into findings. Font-free, deterministic;
// reproduces native-review.mjs's contrast + geometry categories from the SAME descriptor input.
std::vector<Finding> scoreDescriptor (const juce::var& descriptorRoot);
} // namespace eb::hig
```

`src/gui/HigScore.cpp` (contrast only for now — geometry added in Task 4):
```cpp
#include "gui/HigScore.h"
#include "gui/HigThresholds.h"
#include <cmath>
namespace eb::hig {
namespace {
struct Box { double left, top, right, bottom; };
struct El {
    juce::String id, type, role, label, value, fg, bg;
    double x{}, y{}, w{}, h{}, fontPt{}; bool bold{}, visible{}, showing{}, measurable{};
    bool fgOk{}, bgOk{}, textOverflows{};
};
El parse (const juce::var& v) {
    El e; auto b = v.getProperty ("bounds", {});
    e.id = v.getProperty("id",{}).toString(); e.type = v.getProperty("type",{}).toString();
    e.role = v.getProperty("role",{}).toString(); e.label = v.getProperty("label",{}).toString();
    e.value = v.getProperty("value",{}).toString(); e.fg = v.getProperty("fg",{}).toString();
    e.bg = v.getProperty("bg",{}).toString();
    e.x = (double) b.getProperty("x",0); e.y = (double) b.getProperty("y",0);
    e.w = (double) b.getProperty("w",0); e.h = (double) b.getProperty("h",0);
    e.fontPt = (double) v.getProperty("fontPt",0); e.bold = v.getProperty("bold",false);
    e.visible = v.getProperty("visible",false); e.showing = v.getProperty("showing",false);
    e.measurable = v.getProperty("measurable",false);
    e.fgOk = v.getProperty("fgIntrospectable",false); e.bgOk = v.getProperty("bgIntrospectable",false);
    e.textOverflows = v.getProperty("textOverflows",false);
    return e;
}
// --- WCAG (byte-faithful port of wcag-contrast.mjs) ---
double chan (double c8) { double c = c8/255.0; return c <= 0.03928 ? c/12.92 : std::pow((c+0.055)/1.055, 2.4); }
double lumOf (const juce::String& hex) {
    auto h = hex.trimCharactersAtStart("#");
    if (h.length() == 3) h = juce::String::charToString(h[0])+h[0]+h[1]+h[1]+h[2]+h[2];
    auto comp = [&](int i){ return (double) h.substring(i,i+2).getHexValue32(); };
    return 0.2126*chan(comp(0)) + 0.7152*chan(comp(2)) + 0.0722*chan(comp(4));
}
double contrastRatio (const juce::String& a, const juce::String& b) {
    double l1 = lumOf(a), l2 = lumOf(b), hi = juce::jmax(l1,l2), lo = juce::jmin(l1,l2);
    return (hi+0.05)/(lo+0.05);
}
bool isLarge (const El& e) { return e.fontPt >= kLargeFontPt || (e.bold && e.fontPt >= kLargeBoldFontPt); }
} // namespace

std::vector<Finding> scoreDescriptor (const juce::var& root) {
    std::vector<Finding> out;
    auto* arr = root.getProperty ("elements", {}).getArray();
    if (arr == nullptr) return out;
    std::vector<El> els; els.reserve ((size_t) arr->size());
    for (auto& v : *arr) els.push_back (parse (v));

    for (auto& e : els) {
        if (! e.measurable || ! e.fgOk || ! e.bgOk) continue;          // never score a custom-painted node
        if (e.label.isEmpty() && e.value.isEmpty()) continue;
        const double ratio = contrastRatio (e.fg, e.bg);
        const double floor = isLarge (e) ? kContrastFloorLarge : kContrastFloorNormal;
        if (ratio < floor)
            out.push_back ({ "contrast", ratio < floor - kHighSeverityDelta ? "high" : "medium", e.id,
                "text contrast " + juce::String (ratio, 2) + ":1 is below " + juce::String (floor, 1) + ":1" });
    }
    return out;
}
} // namespace eb::hig
```

- [ ] **Step 5: Run the contrast tests (expect PASS)**

Run: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build -R hig`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/gui/HigThresholds.h src/gui/HigScore.h src/gui/HigScore.cpp src/gui/CMakeLists.txt tests/test_hig_score.cpp tests/CMakeLists.txt
git commit -m "feat(hig): HigScore contrast scoring (descriptor-consuming, WCAG port) + thresholds mirror"
```

> **VERIFIER GATE** after Task 3 (focus: the WCAG port matches `wcag-contrast.mjs` exactly — `chan`, luminance weights, the `(hi+0.05)/(lo+0.05)` ratio, the unrounded floor compare).

---

## Task 4: `HigScore` geometry findings (overlap / duplicate / target-size / clip)

**Files:**
- Modify: `src/gui/HigScore.cpp`
- Test: `tests/test_hig_score.cpp`

**Interfaces:**
- Produces: `scoreDescriptor` now also emits `overlap`, `duplicate`, `target-size`, `clip` exactly as `native-review.mjs:geometryFindings` does.

- [ ] **Step 1: Write failing geometry tests** (one per category, mirroring `native-review.mjs:38-53`):

```cpp
TEST_CASE("HigScore: two overlapping non-identical siblings -> one overlap finding") {
    auto d = makeDescriptor ({ el ("Label","a",0,0,100,20), el ("Label","b",90,0,100,20) }); // 10px deep
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1); CHECK (f[0].category == "overlap");
}
TEST_CASE("HigScore: overlap of exactly 2px does NOT report (>2 strict)") {
    auto d = makeDescriptor ({ el ("Label","a",0,0,100,20), el ("Label","b",98,0,100,20) }); // 2px deep
    CHECK (eb::hig::scoreDescriptor (d).empty());
}
TEST_CASE("HigScore: a child exactly filling its parent is nested, not an overlap") {
    auto d = makeDescriptor ({ el ("Label","p",0,0,100,100), el ("Label","c",0,0,100,100) });
    // identical type+label+w+h -> this is actually a DUPLICATE; use different labels for the nesting case:
    auto d2 = makeDescriptor ({ el ("Label","parent",0,0,100,100), el ("Label","child",10,10,50,50) });
    CHECK (eb::hig::scoreDescriptor (d2).empty());
}
TEST_CASE("HigScore: identical stacked pair -> duplicate (medium); identical overlapping -> duplicate (high)") {
    auto stacked = makeDescriptor ({ el ("Label","Same",0,0,40,20), el ("Label","Same",0,40,40,20) });
    auto fs = eb::hig::scoreDescriptor (stacked);
    REQUIRE (fs.size() == 1); CHECK (fs[0].category == "duplicate"); CHECK (fs[0].severity == "medium");
    auto over = makeDescriptor ({ el ("Label","Same",0,0,40,20), el ("Label","Same",10,0,40,20) });
    auto fo = eb::hig::scoreDescriptor (over);
    REQUIRE (fo.size() == 1); CHECK (fo[0].category == "duplicate"); CHECK (fo[0].severity == "high");
}
TEST_CASE("HigScore: sub-24px interactive target -> target-size") {
    auto d = makeDescriptor ({ el ("TextButton","x",0,0,20,20,"#000","#fff",true,true,true,13,false,false,"button") });
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1); CHECK (f[0].category == "target-size");
}
TEST_CASE("HigScore: a clipped label -> clip") {
    auto d = makeDescriptor ({ el ("Label","x",0,0,40,18,"#000","#fff",true,true,true,13,false,/*overflow*/true) });
    auto f = eb::hig::scoreDescriptor (d);
    REQUIRE (f.size() == 1); CHECK (f[0].category == "clip");
}
```

- [ ] **Step 2: Run (expect FAIL)** — Run: `./tools/dev.cmd ctest --test-dir build -R hig` → geometry tests fail.

- [ ] **Step 3: Implement geometry scoring** — extend `scoreDescriptor` with (port of `native-review.mjs:38-53` + `layout-robustness.mjs:22-34`), inserted before `return out;`:

```cpp
    auto rectOf = [](const El& e){ return Box{ e.x, e.y, e.x+e.w, e.y+e.h }; };
    auto boxesOverlap = [](const Box& a, const Box& b){
        return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top; };
    auto overlapDepth = [](const Box& a, const Box& b){
        double w = juce::jmax(0.0, juce::jmin(a.right,b.right) - juce::jmax(a.left,b.left));
        double h = juce::jmax(0.0, juce::jmin(a.bottom,b.bottom) - juce::jmax(a.top,b.top));
        return juce::jmin(w,h); };
    auto contains = [](const Box& p, const Box& q){
        return p.left<=q.left && p.top<=q.top && p.right>=q.right && p.bottom>=q.bottom; };
    auto interactive = [](const El& e){
        return juce::String("button toggle slider combo").contains (e.type.toLowerCase())   // see NOTE
            || e.role == "button" || e.role == "link" || e.role == "slider" || e.role == "checkbox"; };

    std::vector<const El*> vis;
    for (auto& e : els) if (e.visible && e.showing) vis.push_back (&e);
    for (size_t i = 0; i < vis.size(); ++i)
        for (size_t j = i+1; j < vis.size(); ++j) {
            const El& a = *vis[i]; const El& b = *vis[j];
            const Box ra = rectOf(a), rb = rectOf(b);
            const bool overlap = boxesOverlap(ra,rb) && overlapDepth(ra,rb) > (double) kOverlapNoisePx;
            const bool nested  = contains(ra,rb) || contains(rb,ra);
            const bool identical = a.type == b.type && a.label.isNotEmpty()
                                   && a.label == b.label && (int)a.w == (int)b.w && (int)a.h == (int)b.h;
            if (identical)
                out.push_back ({ "duplicate", overlap ? "high" : "medium", b.id,
                    "identical \"" + a.label + "\" (" + a.type + ") appears twice" });
            else if (overlap && ! nested)
                out.push_back ({ "overlap", "medium", b.id,
                    b.id + " overlaps " + a.id + " by " + juce::String ((int) overlapDepth(ra,rb)) + "px" });
        }
    for (auto* e : vis)
        if (interactive (*e) && ((int)e->w < kMinTargetPx || (int)e->h < kMinTargetPx))
            out.push_back ({ "target-size", "medium", e->id,
                juce::String ((int)e->w) + "x" + juce::String ((int)e->h) + "px target below " + juce::String (kMinTargetPx) + "px" });
    for (auto* e : vis)
        if (e->textOverflows)
            out.push_back ({ "clip", "medium", e->id, "\"" + e->label.substring(0,32) + "\" overflows its bounds" });
```

> **NOTE on `interactive`:** `native-review.mjs` uses `/button|toggle|slider|combo/i.test(type)`. Reproduce with a substring-of-lowercased-type test against each token, NOT the `String.contains` line above (it is a placeholder). Implement as: `for (token : {"button","toggle","slider","combo"}) if (e.type.toLowerCase().contains(token)) return true;` then the role checks. The verifier must confirm `ToggleButton`→true (via "toggle"/"button"), `ComboBox`→true (via "combo"), `Label`→false.

- [ ] **Step 4: Run (expect PASS)** — Run: `./tools/dev.cmd ctest --test-dir build -R hig` → all geometry tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/gui/HigScore.cpp tests/test_hig_score.cpp
git commit -m "feat(hig): HigScore geometry scoring (overlap/duplicate/target-size/clip) - native-review port"
```

> **VERIFIER GATE** after Task 4 (focus: the per-pair `identical`-suppresses-`overlap` branch; the strict `> kOverlapNoisePx`; `interactive()` token matching; the `>=` nesting exclusion).

---

## Task 5: Golden parity fixtures (`test_hig_parity`)

**Files:**
- Create: `tests/test_hig_parity.cpp`, `tests/fixtures/hig/*.descriptor.json`, `tests/fixtures/hig/*.expected.json`
- Modify: `tests/CMakeLists.txt` (register the test + a `EB_HIG_FIXTURE_DIR` compile def)

**Interfaces:**
- Consumes: `eb::hig::scoreDescriptor`. Pins it to `native-review.mjs`'s actual output, so neither drifts.

**Context:** Each fixture pairs a descriptor with the **expected findings** captured from `native-review.mjs`. The C++ test asserts `scoreDescriptor(descriptor)` equals `expected`. A CI step (Phase 4) runs `native-review.mjs --thresholds <EARS json>` on the same descriptors and re-asserts `expected` — binding both implementations to one golden.

- [ ] **Step 1: Author the knife-edge fixtures.** Create `tests/fixtures/hig/` descriptors for: contrast exactly 4.5 and 3.0; `overlapDepth` exactly 2 vs 3; child exactly filling parent (nested); identical-and-overlapping; identical-stacked; a clipped label (`textOverflows:true`); a custom/unknown node; a transparent-bg node resolved to an ancestor fill; a sub-24 button. For each, generate `*.expected.json` by running `node <apple-hig>/scripts/native-review.mjs <descriptor> --json` and extracting the `findings` array (category+severity+element), then hand-verify each against the rule.

- [ ] **Step 2: Write the parity test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "gui/HigScore.h"
#include <juce_core/juce_core.h>

TEST_CASE("HigScore matches the native-review golden findings on every fixture") {
    juce::File dir (EB_HIG_FIXTURE_DIR);
    for (auto& desc : dir.findChildFiles (juce::File::findFiles, false, "*.descriptor.json")) {
        auto expectedFile = desc.getSiblingFile (desc.getFileNameWithoutExtension()
                                .upToLastOccurrenceOf (".descriptor", false, false) + ".expected.json");
        REQUIRE (expectedFile.existsAsFile());
        auto findings = eb::hig::scoreDescriptor (juce::JSON::parse (desc));
        auto* expected = juce::JSON::parse (expectedFile).getProperty ("findings", {}).getArray();
        REQUIRE (expected != nullptr);
        // compare as a multiset of "category|severity|element" so ORDER differences don't matter
        juce::StringArray got, want;
        for (auto& f : findings) got.add (f.category + "|" + f.severity + "|" + f.element);
        for (auto& e : *expected) want.add (e.getProperty("category",{}).toString() + "|"
                       + e.getProperty("severity",{}).toString() + "|" + e.getProperty("element",{}).toString());
        got.sort (false); want.sort (false);
        INFO ("fixture: " << desc.getFileName());
        CHECK (got == want);
    }
}
```

- [ ] **Step 3: Register + run.** Add to `tests/CMakeLists.txt`: `test_hig_parity.cpp` in the executable list, and `EB_HIG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hig"` in `target_compile_definitions`.
Run: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build -R hig`
Expected: PASS (any mismatch prints the fixture name — fix `HigScore` or the fixture until green).

- [ ] **Step 4: Commit**

```bash
git add tests/test_hig_parity.cpp tests/fixtures/hig tests/CMakeLists.txt
git commit -m "test(hig): golden parity fixtures binding HigScore to native-review findings"
```

> **VERIFIER GATE** after Task 5 (focus: are the `*.expected.json` actually native-review's output, or hand-fudged to match a buggy port? The verifier re-runs `native-review.mjs` on 2-3 fixtures and confirms.)

---

## Task 6: `MainComponent::TestConfig` — hermetic headless construction

**Files:**
- Modify: `src/gui/MainComponent.h` (ctors), `src/gui/MainComponent.cpp` (ctor init-list + line ~521 guard)
- Test: `tests/test_hig_layout.cpp` (new — construction smoke test)

**Interfaces:**
- Produces: `struct MainComponent::TestConfig { juce::File settingsDir; bool disableNetwork = true; };` and `explicit MainComponent (const TestConfig&)`. Real `MainComponent()` is unchanged in behavior (network on, default settings dir).

**Context:** `MainComponent` default-constructs `Settings settings;` (member, `MainComponent.h:82`) → real per-user file; and at `MainComponent.cpp:521` fires `updateChecker.start(...)` gated only on `settings.autoCheckUpdates()` (defaults true). Inject a temp dir + suppress the network.

- [ ] **Step 1: Write the failing construction test**

```cpp
// tests/test_hig_layout.cpp
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/MainComponent.h"

TEST_CASE("MainComponent constructs headless with a temp-dir Settings and no network") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile ("");
    tmp.createDirectory();
    eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, /*disableNetwork*/true });
    mc.setSize (900, 780);
    CHECK (mc.getWidth() == 900);
    CHECK (mc.getNumChildComponents() > 0);   // children laid out, no peer needed
    tmp.deleteRecursively();
}
```

- [ ] **Step 2: Register + run (expect FAIL — no such ctor).** Add `test_hig_layout.cpp` to `tests/CMakeLists.txt`. Run: `./tools/dev.cmd cmake --build build --target eb_tests` → compile error.

- [ ] **Step 3: Implement the delegating ctors.** In `MainComponent.h`:

```cpp
public:
    struct TestConfig { juce::File settingsDir; bool disableNetwork = true; };
    MainComponent();
    explicit MainComponent (const TestConfig& cfg);
    ~MainComponent() override;
private:
    MainComponent (juce::File settingsDir, bool disableNetwork);   // common construction
```

In `MainComponent.cpp`: rename the existing `MainComponent::MainComponent()` definition to `MainComponent::MainComponent (juce::File settingsDir, bool disableNetwork)`, add `settings (settingsDir)` to its member-init list (it constructs `Settings` with the dir; empty dir = the real per-user file), and change line ~521 to `if (settings.autoCheckUpdates() && ! disableNetwork)`. Then add the two thin ctors:

```cpp
MainComponent::MainComponent() : MainComponent (juce::File{}, false) {}                       // real app: default dir, network on
MainComponent::MainComponent (const TestConfig& cfg) : MainComponent (cfg.settingsDir, cfg.disableNetwork) {}
```

(If the existing ctor has no explicit `settings(...)` in its init list, add it; `settings` is declared after `engine`, so place it after `engine(...)` to respect init order — though init order follows declaration order regardless of list order, the compiler warns on mismatch, so order the list to match.)

- [ ] **Step 4: Run (expect PASS) + full suite green**

Run: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build`
Expected: the new test passes AND the prior 472 still pass (no behavior change to the real ctor).

- [ ] **Step 5: Commit**

```bash
git add src/gui/MainComponent.h src/gui/MainComponent.cpp tests/test_hig_layout.cpp tests/CMakeLists.txt
git commit -m "feat(gui): MainComponent TestConfig - hermetic temp-Settings + no-network headless construction"
```

> **VERIFIER GATE** after Task 6 (focus: the real `MainComponent()` path is byte-identical in behavior — network still fires, default settings dir; init-list order matches declaration order; no leak via `tooltips{this}` headless).

---

## Task 7: `test_hig_layout` — the gate over the real editor

**Files:**
- Modify: `tests/test_hig_layout.cpp`
- Uses: `gui/juce_design_probe.h`, `gui/HigScore.h`

**Interfaces:**
- Consumes: `MainComponent::TestConfig`, `hig::writeDesignProbe`, `eb::hig::scoreDescriptor`.
- Produces: the Phase-1 build gate — fails on any blocking finding (overlap/clip/duplicate/target-size/contrast) in the real editor's default state across the matrix.

**Context:** Phase 1 drives the editor's **default laid-out state** (no scenario catalog yet — that is Phase 3). It iterates the cross-cutting axes and asserts the stock-widget subset is clean. Custom-painted nodes are `measurable:false` → not scored (Phase 2 instruments them). The matrix uses the existing `eb::Theme` + `eb::SystemA11y` toggles.

- [ ] **Step 1: Write the gate test** (start RED — it will surface whatever real findings exist; fix or allowlist them in Step 3):

```cpp
TEST_CASE("HIG gate: the real editor has no blocking layout finding in any theme/a11y/size") {
    juce::ScopedJuceInitialiser_GUI juceInit;
    auto tmp = juce::File::createTempFile (""); tmp.createDirectory();
    auto blocking = [](const eb::hig::Finding& f){
        return f.category == "overlap" || f.category == "clip" || f.category == "duplicate"
            || f.category == "target-size" || f.severity == "high"
            || (f.category == "contrast"); };   // gate blocks on contrast<floor (any severity)
    struct Axis { const char* name; bool dark; int size; };
    const Axis axes[] = { {"dark-min",true,0}, {"dark-large",true,1}, {"light-min",false,0}, {"light-large",false,1} };
    for (auto& ax : axes) {
        eb::Theme::setDarkForTest (ax.dark);            // (add a tiny test setter if absent; else drive via the existing path)
        eb::MainComponent mc (eb::MainComponent::TestConfig { tmp, true });
        mc.setSize (ax.size == 0 ? 780 : 1200, ax.size == 0 ? 720 : 1000);
        auto json = mc.getChildComponent (0) ? juce::String() : juce::String();   // placeholder; see Step 2
        auto jf = tmp.getChildFile ("d.json"); auto pf = tmp.getChildFile ("d.png");
        hig::writeDesignProbe (mc, jf, pf);
        auto findings = eb::hig::scoreDescriptor (juce::JSON::parse (jf));
        juce::StringArray bad;
        for (auto& f : findings) if (blocking (f)) bad.add (f.category + " on " + f.element + ": " + f.message);
        INFO ("axis " << ax.name << " findings:\n" << bad.joinIntoString ("\n"));
        CHECK (bad.isEmpty());
    }
    tmp.deleteRecursively();
}
```

- [ ] **Step 2: Wire the a11y axis + run to SEE the real findings.** Extend the loop to also toggle the three `SystemA11y` modes (the existing `eb::SystemA11y::refresh()` reads the OS; for the test, add minimal `SystemA11y::setForTest(reduceMotion,increaseContrast,reduceTransparency)` setters that write the atomics directly — mirror the `Theme::setDarkForTest` approach). Run: `./tools/dev.cmd ctest --test-dir build -R hig -V`
Expected: the test prints any real overlap/clip/contrast findings on stock widgets.

- [ ] **Step 3: Resolve the findings.** For each real finding: if it is a genuine bug, **fix the layout/colour** (this is the gate doing its job); if it is a justified false-positive (e.g. a legitimately-stacked identical pair), record it in a small in-test allowlist keyed by **component-ID + category** with a one-line reason (NOT by text). Re-run until the gate is green across the whole matrix.

- [ ] **Step 4: Full suite green**

Run: `./tools/dev.cmd cmake --build build --target eb_tests && ./tools/dev.cmd ctest --test-dir build`
Expected: all tests pass (≥ 472 + the new HIG tests).

- [ ] **Step 5: Commit**

```bash
git add tests/test_hig_layout.cpp src/gui/Theme.h src/gui/Theme.cpp src/gui/SystemA11y.h src/platform/SystemA11y.cpp
git commit -m "feat(hig): Phase-1 build gate - real editor scored across theme/a11y/size, blocks on overlap/clip/contrast"
```

> **VERIFIER GATE** after Task 7 (focus: does the gate actually FAIL when seeded with a forced overlap? The verifier temporarily shrinks a label to force a clip and confirms the gate goes red — proving it is not a no-op. Also: is the allowlist scoped by ID+category, not text?)

---

## Self-Review

**Spec coverage:** Phase-1 spec items → tasks: descriptor-consuming gate (T3-T5,T7); single-sourced thresholds + native-review import (T2,T3); golden parity (T5); dead-role fix (T1); TestConfig temp-Settings + no-network (T6); stock-widget matrix across theme/a11y/size (T7). Custom-surface instrumentation, formatter-driven catalog, fail-closed coverage, pixel-contrast, device-scale = Phases 2-4 (out of Phase-1 scope, correctly). ✔
**Placeholder scan:** the two `// placeholder` spots (the `interactive` substring line in T4 Step 3, the `json` line in T7 Step 1) are explicitly flagged with a NOTE telling the implementer the correct form — they are teaching markers, not silent gaps; the verifier gate catches them. ✔
**Type consistency:** `Finding{category,severity,element,message}` and `scoreDescriptor(const juce::var&)` are consistent across T3-T7; `TestConfig{settingsDir,disableNetwork}` consistent T6-T7; `HigThresholds.h` constant names consistent T3-T4. ✔

## Open items for the implementer to confirm against live code
- `Theme` dark-mode test setter: confirm whether `eb::Theme` exposes a way to force dark/light for a test (the scout noted `Theme::s_dark` + `syncMode()`); if not, add a minimal `setDarkForTest(bool)`. Same for `SystemA11y::setForTest`.
- `src/gui/CMakeLists.txt`: confirm whether `eb_gui` lists sources explicitly (add `HigScore.cpp`) or globs.
- Whether the existing `MainComponent` ctor already lists `settings` in its init-list (adjust vs add).
