#!/usr/bin/env node
/* WCAG 2.2 AA contrast check for the console stylesheet.
 *
 * The point is not to compute a pretty number but to make a failing colour
 * fail the build: `--text-faint` sat at ~4.0:1 on the page background for a
 * long time and nothing caught it.
 *
 * Rules used (WCAG 2.2, normal-size text):
 *   - body text and labels: 4.5:1
 *   - large text (>= 24px, or >= 18.66px bold): 3:1
 * Every colour in PAIRS is checked against every background it can appear
 * on, and the strictest applicable threshold wins.
 *
 * Run: node tools/contrast_check.js [path/to/style.css]
 * Exit code 0 = all pairs pass, 1 = at least one fails.
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const CSS = process.argv[2] || path.join(__dirname, '..', 'public', 'style.css');

function parseVars(css) {
  const vars = {};
  const block = css.match(/:root\s*\{([\s\S]*?)\}/);
  if (!block) throw new Error('no :root block found');
  for (const m of block[1].matchAll(/(--[a-z0-9-]+)\s*:\s*([^;]+);/gi)) {
    vars[m[1]] = m[2].trim();
  }
  return vars;
}

function hexToRgb(hex) {
  let h = hex.trim().replace('#', '');
  if (h.length === 3) h = h.split('').map((c) => c + c).join('');
  if (h.length !== 6) throw new Error(`not a 6-digit hex colour: ${hex}`);
  return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16));
}

function relativeLuminance(rgb) {
  const [r, g, b] = rgb.map((v) => {
    const s = v / 255;
    return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

function contrast(a, b) {
  const la = relativeLuminance(hexToRgb(a));
  const lb = relativeLuminance(hexToRgb(b));
  const hi = Math.max(la, lb);
  const lo = Math.min(la, lb);
  return (hi + 0.05) / (lo + 0.05);
}

/* Colour -> the backgrounds it is actually used on. Adding a pair here
 * means "this text really appears on that surface", which is the claim the
 * test makes. */
const PAIRS = [
  ['--text', '--bg', 4.5, 'body text on the page background'],
  ['--text', '--bg-elev', 4.5, 'body text on a panel'],
  ['--text', '--bg-elev-2', 4.5, 'body text on an input'],
  ['--text-dim', '--bg', 4.5, 'secondary text on the page'],
  ['--text-dim', '--bg-elev', 4.5, 'secondary text on a panel'],
  ['--text-dim', '--bg-elev-2', 4.5, 'secondary text on an input'],
  ['--text-faint', '--bg', 4.5, 'hint text on the page'],
  ['--text-faint', '--bg-elev', 4.5, 'hint text on a panel'],
  ['--text-faint', '--bg-elev-2', 4.5, 'hint text on an input'],
  ['--accent', '--bg', 4.5, 'code/links on the page'],
  ['--accent', '--bg-elev', 4.5, 'code/links on a panel'],
  ['--accent', '--bg', 3, 'focus ring (non-text) on the page'],
  ['--ok', '--bg-elev', 4.5, 'success badge on a panel'],
  ['--warn', '--bg-elev', 4.5, 'warning badge on a panel'],
  ['--err', '--bg-elev', 4.5, 'error badge on a panel'],
];

/* Colours that sit *on top of* a dark chip instead of the panel. The chip
 * background is hardcoded in the stylesheet, so it is spelled out here. */
const EXTRA_BACKGROUNDS = { '#04222f': 'state cell / primary button text' };

const vars = parseVars(fs.readFileSync(CSS, 'utf8'));
let failed = 0;
const rows = [];

for (const [fg, bg, min, what] of PAIRS) {
  const f = vars[fg];
  const b = vars[bg];
  if (!f || !b) {
    console.log(`SKIP  ${fg} on ${bg}: variable not defined`);
    continue;
  }
  const ratio = contrast(f, b);
  const ok = ratio >= min;
  if (!ok) failed += 1;
  rows.push({ fg, bg, ratio, min, ok, what });
}

for (const [chip, what] of Object.entries(EXTRA_BACKGROUNDS)) {
  for (const fg of ['--accent', '--text']) {
    const ratio = contrast(vars[fg], chip);
    const ok = ratio >= 4.5;
    if (!ok) failed += 1;
    rows.push({ fg, bg: chip, ratio, min: 4.5, ok, what: `${what} (${fg})` });
  }
}

for (const r of rows) {
  const status = r.ok ? 'ok  ' : 'FAIL';
  console.log(
    `${status} ${r.ratio.toFixed(2).padStart(5)}:1 (min ${r.min})  ${r.fg} on ${r.bg} — ${r.what}`
  );
}

const worst = rows.reduce((a, b) => (a.ratio < b.ratio ? a : b));
console.log(`\nworst pair: ${worst.fg} on ${worst.bg} at ${worst.ratio.toFixed(2)}:1`);

if (failed > 0) {
  console.error(`CONTRAST-CHECK: ${failed} pair(s) below WCAG AA`);
  process.exit(1);
}
console.log(`CONTRAST-CHECK: ${rows.length} pairs pass WCAG AA`);