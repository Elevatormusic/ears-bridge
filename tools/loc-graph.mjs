#!/usr/bin/env node
// Generates assets/loc.svg + assets/loc-history.json — the README "lines of code" graph.
//
// What it counts: non-blank source lines of FIRST-PARTY C++ under src/ and tests/, broken
// down by language (the JUCE/Catch2 dependencies live under build/_deps and are never in the
// repo, so they are excluded automatically). It also walks the v* release tags to draw a
// growth curve across releases.
//
// Self-contained — no npm deps, just Node + git. Run locally to (re)seed, or from
// .github/workflows/loc.yml on every push to keep the graph current. The "now" data point and
// the "updated" caption use the HEAD commit date, so the output is deterministic per commit.

import { execSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync, statSync } from 'node:fs';
import { join, extname } from 'node:path';

const ROOT = process.cwd();
const PATHS = ['src', 'tests'];
const CODE_EXT = new Set(['.cpp', '.cc', '.cxx', '.mm', '.h', '.hpp']);

const langOf = (f) => {
  const e = extname(f).toLowerCase();
  if (e === '.cpp' || e === '.cc' || e === '.cxx') return 'C++';
  if (e === '.mm') return 'Obj-C++';
  if (e === '.h' || e === '.hpp') return 'Headers';
  return null;
};
const nonBlank = (s) => {
  let n = 0;
  for (const l of s.split(/\r?\n/)) if (l.trim() !== '') n++;
  return n;
};
const git = (args) =>
  execSync(`git ${args}`, { cwd: ROOT, encoding: 'utf8', maxBuffer: 128 * 1024 * 1024 });

// ---- count the current working tree ----
function walk(dir, out = []) {
  if (!existsSync(dir)) return out;
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (CODE_EXT.has(extname(name).toLowerCase())) out.push(p);
  }
  return out;
}
function countWorkingTree() {
  const byLang = {}; let src = 0, tests = 0;
  for (const base of PATHS)
    for (const f of walk(join(ROOT, base))) {
      const lang = langOf(f); if (!lang) continue;
      const n = nonBlank(readFileSync(f, 'utf8'));
      byLang[lang] = (byLang[lang] || 0) + n;
      if (base === 'tests') tests += n; else src += n;
    }
  return { total: src + tests, src, tests, byLang };
}

// ---- count a git ref (release tag) without touching the working tree ----
function countRef(ref) {
  let files;
  try { files = git(`ls-tree -r --name-only ${ref} -- ${PATHS.join(' ')}`).split('\n').filter(Boolean); }
  catch { return null; }
  const byLang = {}; let src = 0, tests = 0;
  for (const f of files) {
    const lang = langOf(f); if (!lang) continue;
    let content; try { content = git(`show ${ref}:${f}`); } catch { continue; }
    const n = nonBlank(content);
    byLang[lang] = (byLang[lang] || 0) + n;
    if (f.startsWith('tests/')) tests += n; else src += n;
  }
  return { total: src + tests, src, tests, byLang };
}

// ---- assemble history: every v* tag (by commit date) + the current "now" point ----
const ASSETS = join(ROOT, 'assets');
const HIST = join(ASSETS, 'loc-history.json');
let history = [];
if (existsSync(HIST)) { try { history = JSON.parse(readFileSync(HIST, 'utf8')); } catch { history = []; } }
const haveRef = new Set(history.map((h) => h.ref));

let tags = [];
try { tags = git('tag --list v*').split('\n').map((s) => s.trim()).filter(Boolean); } catch {}
const dateOf = (ref) => { try { return git(`log -1 --format=%cI ${ref}`).trim().slice(0, 10); } catch { return null; } };
const tagPoints = tags.map((t) => ({ t, d: dateOf(t) })).filter((x) => x.d).sort((a, b) => (a.d < b.d ? -1 : 1));

for (const { t, d } of tagPoints) {
  if (haveRef.has(t)) continue;
  const c = countRef(t);
  if (c && c.total > 0) history.push({ ref: t, date: d, ...c });
}

const headDate = dateOf('HEAD') || tagPoints.at(-1)?.d || '0000-00-00';
history = history.filter((h) => h.ref !== 'now');
history.push({ ref: 'now', date: headDate, ...countWorkingTree() });
history.sort((a, b) => (a.date < b.date ? -1 : a.date > b.date ? 1 : 0));

mkdirSync(ASSETS, { recursive: true });
writeFileSync(HIST, JSON.stringify(history, null, 2) + '\n');

// ---- render the SVG ----
const cur = history.at(-1);
const fmt = (n) => n.toLocaleString('en-US');
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

const C = {
  bg: '#15171C', border: '#282B32', text: '#F5F5F7', dim: '#9AA0AA', faint: '#6B7079',
  grid: '#23262D', accent: '#0A84FF',
  lang: { 'C++': '#0A84FF', 'Headers': '#5AC8FA', 'Obj-C++': '#BF8CFF', other: '#8E8E93' },
};
const W = 900, H = 392, M = { l: 56, r: 32, t: 134, b: 92 };
const plotW = W - M.l - M.r, plotH = H - M.t - M.b;
const baseY = M.t + plotH;            // chart baseline (y of value 0)

const totals = history.map((h) => h.total);
const niceMax = (m) => { const s = m > 12000 ? 4000 : m > 6000 ? 2000 : 1000; return Math.ceil(m / s) * s; };
const yMax = niceMax(Math.max(...totals, 1));
const n = history.length;
const xAt = (i) => M.l + (n === 1 ? plotW / 2 : (plotW * i) / (n - 1));
const yAt = (v) => baseY - (plotH * v) / yMax;

let grid = '';
const ticks = 4;
for (let i = 0; i <= ticks; i++) {
  const v = (yMax / ticks) * i, y = yAt(v);
  grid += `<line x1="${M.l}" y1="${y.toFixed(1)}" x2="${W - M.r}" y2="${y.toFixed(1)}" stroke="${C.grid}" stroke-width="1"/>`;
  const lbl = v >= 1000 ? v / 1000 + 'k' : '' + v;
  grid += `<text x="${M.l - 10}" y="${(y + 4).toFixed(1)}" text-anchor="end" font-size="11" fill="${C.faint}">${lbl}</text>`;
}

const line = history.map((h, i) => `${i ? 'L' : 'M'}${xAt(i).toFixed(1)} ${yAt(h.total).toFixed(1)}`).join(' ');
const area = `${line} L${xAt(n - 1).toFixed(1)} ${baseY.toFixed(1)} L${xAt(0).toFixed(1)} ${baseY.toFixed(1)} Z`;
const dots = history
  .map((h, i) => (h.ref === 'now' || i === 0)
    ? `<circle cx="${xAt(i).toFixed(1)}" cy="${yAt(h.total).toFixed(1)}" r="3.5" fill="${C.accent}"/>` : '')
  .join('');

const xLabelY = baseY + 22;
const xLabels =
  `<text x="${M.l}" y="${xLabelY}" font-size="11" fill="${C.faint}">${history[0].date}</text>` +
  `<text x="${(M.l + W - M.r) / 2}" y="${xLabelY}" text-anchor="middle" font-size="11" fill="${C.faint}">${tags.length} releases</text>` +
  `<text x="${W - M.r}" y="${xLabelY}" text-anchor="end" font-size="11" fill="${C.faint}">${cur.date}</text>`;

// breakdown (current snapshot): a legend row above a full-width stacked bar, both clear of the chart
const langs = Object.entries(cur.byLang).sort((a, b) => b[1] - a[1]);
const legendY = baseY + 46, barY = baseY + 58, barH = 8;
let lx = M.l, legend = '';
for (const [name, v] of langs) {
  const col = C.lang[name] || C.lang.other;
  legend += `<circle cx="${lx + 4}" cy="${legendY - 4}" r="4" fill="${col}"/>` +
            `<text x="${lx + 14}" y="${legendY}" font-size="11" fill="${C.dim}">${esc(name)} ${fmt(v)}</text>`;
  lx += 28 + (name.length + fmt(v).length) * 7.2 + 22;
}
let bx = M.l, bar = '';
for (const [name, v] of langs) {
  const w = (plotW * v) / cur.total, col = C.lang[name] || C.lang.other;
  bar += `<rect x="${bx.toFixed(1)}" y="${barY}" width="${Math.max(0, w - 1.5).toFixed(1)}" height="${barH}" rx="2" fill="${col}"/>`;
  bx += w;
}

const numStr = fmt(cur.total);
const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" font-family="-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif" role="img" aria-label="EARS Bridge lines of code: ${numStr} across ${tags.length} releases">
  <rect x="0.5" y="0.5" width="${W - 1}" height="${H - 1}" rx="14" fill="${C.bg}" stroke="${C.border}"/>
  <text x="${M.l}" y="46" font-size="12" letter-spacing="1.5" fill="${C.dim}">LINES OF CODE</text>
  <text x="${M.l}" y="94" font-size="46" font-weight="700" fill="${C.text}">${numStr}</text>
  <text x="${M.l + numStr.length * 26 + 12}" y="94" font-size="18" fill="${C.faint}">lines</text>
  <text x="${M.l}" y="118" font-size="13" fill="${C.dim}">${fmt(cur.src)} in src · ${fmt(cur.tests)} in tests · updated ${cur.date}</text>
  ${grid}
  <path d="${area}" fill="${C.accent}" fill-opacity="0.12"/>
  <path d="${line}" fill="none" stroke="${C.accent}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>
  ${dots}
  ${xLabels}
  ${legend}
  ${bar}
</svg>
`;
writeFileSync(join(ASSETS, 'loc.svg'), svg);
console.log(`loc.svg — ${numStr} lines (${fmt(cur.src)} src / ${fmt(cur.tests)} tests), ${history.length} points across ${tags.length} tags`);
for (const [k, v] of langs) console.log(`  ${k}: ${fmt(v)}`);
