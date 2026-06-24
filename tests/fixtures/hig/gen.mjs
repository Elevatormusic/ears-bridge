// Regenerate the HigScore golden parity fixtures: build knife-edge native-render descriptors, run the apple-hig
// native-review.mjs reviewNativeDescriptor on each, and write <name>.descriptor.json + <name>.expected.json
// (the expected findings filtered to the GATE categories the C++ HigScore reproduces; hierarchy/coverage are
// advisory-only and intentionally excluded). The C++ test (tests/test_hig_parity.cpp) asserts HigScore == these.
// Run from anywhere:  node tests/fixtures/hig/gen.mjs
import { writeFileSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const NRPATH = 'C:/Users/Shaya/.claude/plugins/cache/apple-hig/apple-hig/1.7.0/scripts/native-review.mjs';
const { reviewNativeDescriptor } = await import(pathToFileURL(NRPATH).href);

const GATE = new Set(['contrast', 'overlap', 'duplicate', 'target-size', 'clip']);
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
});
const desc = (els) => ({
  meta: { juceVersion: '8', scaleFactor: 1.0, rootBounds: { x: 0, y: 0, w: 900, h: 600 },
          snapshotPath: 'x.png', shown: false, axCoverageRatio: 0 },
  elements: els,
});

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
};

for (const [name, d] of Object.entries(fixtures)) {
  writeFileSync(join(here, name + '.descriptor.json'), JSON.stringify(d, null, 2) + '\n');
  const findings = reviewNativeDescriptor(d).findings
    .filter((f) => GATE.has(f.category))
    .map((f) => ({ category: f.category, severity: f.severity, element: f.element }));
  writeFileSync(join(here, name + '.expected.json'), JSON.stringify({ findings }, null, 2) + '\n');
  console.log(name.padEnd(24), '->', String(findings.length).padStart(2), 'gate findings:',
              findings.map((f) => `${f.category}/${f.severity}/${f.element}`).join(', ') || '(none)');
}
