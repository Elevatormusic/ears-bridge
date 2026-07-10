// Regenerate the HigScore golden parity fixtures: build knife-edge native-render descriptors, run the apple-hig
// native-review.mjs reviewNativeDescriptor on each, and write <name>.descriptor.json + <name>.expected.json
// (the expected findings filtered to the categories the C++ HigScore reproduces; hierarchy/coverage are
// advisory-only and intentionally excluded). The C++ test (tests/test_hig_parity.cpp) asserts HigScore == these.
// P4 T7: state-* fixtures parity-lock scoreStateSweep against stateFindings tiers 1-2 (plugin v1.10.0);
// their expected findings filter to the STATE categories instead of the GATE set.
// Run from anywhere:  node tests/fixtures/hig/gen.mjs
import { writeFileSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const NRPATH = 'C:/Users/Shaya/.claude/plugins/cache/apple-hig/apple-hig/1.10.0/scripts/native-review.mjs';
const { reviewNativeDescriptor } = await import(pathToFileURL(NRPATH).href);

const GATE  = new Set(['contrast', 'overlap', 'duplicate', 'target-size', 'clip']);
const STATE = new Set(['unstyled-control-states', 'two-state-inert', 'disabled-louder']);
let n = 0;
const el = (o) => ({
  id: o.id ?? ('e' + (n++)), type: o.type ?? 'Label', role: o.role ?? 'text',
  label: o.label ?? '', value: o.value ?? '',
  bounds: { x: o.x ?? 0, y: o.y ?? 0, w: o.w ?? 50, h: o.h ?? 20 },
  fg: o.fg ?? '#000000', bg: o.bg ?? '#ffffff',
  fgIntrospectable: o.fgOk ?? true, bgIntrospectable: o.bgOk ?? true,
  fontPt: o.fontPt ?? 13, bold: o.bold ?? false,
  visible: o.visible ?? true, showing: o.showing ?? true, enabled: true,
  checkable: false, checked: false, measurable: o.measurable ?? true,
  snapshotMayBeBlank: false, textOverflows: o.textOverflows ?? false,
  ...(o.states  !== undefined ? { states:  o.states  } : {}),
  ...(o.primary !== undefined ? { primary: o.primary } : {}),
});
// One swept-state sample: mean rgb triplet + alpha (+ optional 16-cell grid of [r,g,b]).
const st = (rgb, alpha = 1, grid = undefined) => ({ rgb, alpha, ...(grid !== undefined ? { grid } : {}) });
const desc = (els) => ({
  meta: { juceVersion: '8', scaleFactor: 1.0, rootBounds: { x: 0, y: 0, w: 900, h: 600 },
          snapshotPath: 'x.png', shown: false, axCoverageRatio: 0 },
  elements: els,
});

// 16 identical grid cells, with an optional single-cell override.
const grid16 = (rgb, oddIndex = -1, oddRgb = null) =>
  Array.from({ length: 16 }, (_, i) => (i === oddIndex ? oddRgb : rgb));

const fixtures = {
  // contrast at a spread of ratios bracketing the 4.5 and 3.5 (floor-1) boundaries; spread vertically so no
  // pair overlaps and labels are distinct (no duplicate). native-review captures the truth; C++ must match.
  'contrast-spread': desc([
    el({ id: 'c0', label: 'a', y: 0,  fg: '#767676' }),
    el({ id: 'c1', label: 'b', y: 30, fg: '#777777' }),
    el({ id: 'c2', label: 'c', y: 60, fg: '#888888' }),
    el({ id: 'c3', label: 'd', y: 90, fg: '#949494' }),
  ]),
  'overlap-2px':           desc([ el({ id: 'a', label: 'a', x: 0, w: 100 }), el({ id: 'b', label: 'b', x: 98, w: 100 }) ]),
  'overlap-3px':           desc([ el({ id: 'a', label: 'a', x: 0, w: 100 }), el({ id: 'b', label: 'b', x: 97, w: 100 }) ]),
  'nested':                desc([ el({ id: 'p', label: 'parent', x: 0, y: 0, w: 100, h: 100 }), el({ id: 'c', label: 'child', x: 10, y: 10, w: 50, h: 50 }) ]),
  'identical-stacked':     desc([ el({ id: 'a', label: 'Same', y: 0, w: 40 }), el({ id: 'b', label: 'Same', y: 40, w: 40 }) ]),
  'identical-overlapping': desc([ el({ id: 'a', label: 'Same', x: 0, w: 40 }), el({ id: 'b', label: 'Same', x: 10, w: 40 }) ]),
  'target-sub24':          desc([ el({ id: 't', type: 'TextButton', role: 'button', label: 'x', w: 20, h: 20 }) ]),
  'clip':                  desc([ el({ id: 'c', label: 'overflowing text here', w: 40, h: 18, textOverflows: true }) ]),
  'custom-node':           desc([ el({ id: 'cu', type: 'custom/unknown', label: 'x', fg: 'not introspectable', bg: 'not introspectable', measurable: false, fgOk: false, bgOk: false }) ]),

  // ---- P4 T7 state-sweep fixtures (knife-edge; expected filtered to the STATE categories) ----
  // 1. >=3 measurable states all identical -> unstyled-control-states, medium.
  'state-inert-3': desc([ el({ id: 'i3', type: 'TextButton', role: 'button', label: 'x', states: {
    normal: st([56, 56, 58]), over: st([56, 56, 58]), down: st([56, 56, 58]), disabled: st([56, 56, 58]) } }) ]),
  // 2. Same but primary -> high.
  'state-inert-3-primary': desc([ el({ id: 'i3p', type: 'TextButton', role: 'button', label: 'x', primary: true, states: {
    normal: st([56, 56, 58]), over: st([56, 56, 58]), down: st([56, 56, 58]), disabled: st([56, 56, 58]) } }) ]),
  // 3. Exactly 2 measurable states, identical -> two-state-inert, info (often sanctioned).
  'state-inert-2': desc([ el({ id: 'i2', label: 'x', states: {
    normal: st([120, 120, 122]), disabled: st([120, 120, 122]) } }) ]),
  // 4. A styled control (over differs by 10/channel) -> NO finding (negative). Disabled is LIGHTER than
  // normal (less contrast on the white bg) so tier 2 stays quiet too - on a light bg "dimmer" = lighter;
  // a darker disabled would legitimately read louder (caught live: the first cut of this fixture did).
  'state-styled': desc([ el({ id: 'sty', type: 'TextButton', role: 'button', label: 'x', states: {
    normal: st([56, 56, 58]), over: st([66, 66, 68]), down: st([76, 76, 78]), disabled: st([140, 140, 142]) } }) ]),
  // 5. Tolerance knife-edge: delta exactly 2/channel = identical (finding); sibling at 3 = styled (none).
  'state-tolerance-edge': desc([
    el({ id: 'tol2', type: 'TextButton', role: 'button', label: 'a', y: 0, states: {
      normal: st([100, 100, 100]), over: st([102, 102, 102]), down: st([100, 100, 100]), disabled: st([98, 98, 98]) } }),
    el({ id: 'tol3', type: 'TextButton', role: 'button', label: 'b', y: 40, states: {
      normal: st([100, 100, 100]), over: st([103, 103, 103]), down: st([100, 100, 100]), disabled: st([100, 100, 100]) } }),
  ]),
  // 6. Disabled contrast louder than normal vs an introspectable bg -> disabled-louder, low.
  'state-disabled-louder': desc([ el({ id: 'dl', type: 'TextButton', role: 'button', label: 'x', bg: '#1e1e1e', states: {
    normal: st([100, 100, 100]), disabled: st([220, 220, 220]) } }) ]),
  // 7. bg NOT introspectable -> the alpha fallback: disabled alpha > normal + 0.05 -> low.
  'state-disabled-louder-alpha': desc([ el({ id: 'dla', label: 'x', bg: 'not introspectable', bgOk: false, states: {
    normal: st([100, 100, 100], 0.5), disabled: st([100, 100, 100], 0.9) } }) ]),
  // 8. A >60-degree hue rotation at similar alpha = a colour SWAP, not a dimming failure -> NO finding (negative).
  'state-hue-swap': desc([ el({ id: 'hs', type: 'TextButton', role: 'button', label: 'x', bg: '#1e1e1e', states: {
    normal: st([0, 145, 255]), disabled: st([0, 255, 100]) } }) ]),
  // 9. Every state below the measurability floor (alpha 0.02) -> missing DATA, not missing styling -> none.
  'state-low-alpha': desc([ el({ id: 'la', type: 'TextButton', role: 'button', label: 'x', states: {
    normal: st([56, 56, 58], 0.02), over: st([56, 56, 58], 0.02), down: st([56, 56, 58], 0.02) } }) ]),
  // 10. Means identical but ONE grid cell differs beyond tol -> NOT inert (the grid catches local styling).
  'state-grid-diff': desc([ el({ id: 'gd', type: 'TextButton', role: 'button', label: 'x', states: {
    normal:   st([100, 100, 100], 1, grid16([100, 100, 100])),
    over:     st([100, 100, 100], 1, grid16([100, 100, 100], 5, [110, 110, 110])),
    down:     st([100, 100, 100], 1, grid16([100, 100, 100])),
    disabled: st([100, 100, 100], 1, grid16([100, 100, 100])) } }) ]),
};

for (const [name, d] of Object.entries(fixtures)) {
  const filter = name.startsWith('state-') ? STATE : GATE;
  writeFileSync(join(here, name + '.descriptor.json'), JSON.stringify(d, null, 2) + '\n');
  const findings = reviewNativeDescriptor(d).findings
    .filter((f) => filter.has(f.category))
    .map((f) => ({ category: f.category, severity: f.severity, element: f.element }));
  writeFileSync(join(here, name + '.expected.json'), JSON.stringify({ findings }, null, 2) + '\n');
  console.log(name.padEnd(28), '->', String(findings.length).padStart(2), 'findings:',
              findings.map((f) => `${f.category}/${f.severity}/${f.element}`).join(', ') || '(none)');
}
