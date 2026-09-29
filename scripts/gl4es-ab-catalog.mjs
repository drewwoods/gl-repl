#!/usr/bin/env node
// gl4es-ab-catalog.mjs - screenshot every built-in example in two builds of the
// gl-repl web app (the same app linked against two gl4es trees), diff the
// pairs, and write an HTML report. Driven by scripts/gl4es-ab.sh.
//
//   node scripts/gl4es-ab-catalog.mjs --a <webdir> --b <webdir> --out <dir>
//        [--names names.tsv] [--examples 1,5,9 | --count N] [--time secs]
//        [--frames N]
//
// Each capture loads index.html with argv `--example N --no-audio` and env:
//   GLR_TIME=<secs>, GLR_CFG=auto_time=0   t pinned before the first tick
//   GLR_TICK_PER_FRAME=1                   every other clock advances per frame
//   GLR_FREEZE_AFTER_FRAMES=<frames>       then the app stops drawing
//   GLR_NO_SPLASH, GLR_NO_INPUT
// Freezing t alone is not enough: many scenes carry state from frame to frame
// and eased cameras take a few hundred frames to land, so the capture is
// taken at an exact frame count instead. Frame <frames> is then a function of
// the example and the GL alone (a build diffed against itself is 0 px).
//
// Pixels only, no timing: a frozen scene is where gl-repl's frame pacer idles,
// so frame rates here would measure the pacer. Performance is the render-bench
// lane (gl4es-ab-render.mjs), which times a fixed workload.
//
// Output: <out>/<N>-a.png, -b.png, -diff.png, results.json, report.html.
// Pixel diffs need ImageMagick (`magick compare`).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchChrome, moduleInjection, serveDirs, sleep } from './web-chrome.mjs';

function usage(msg) {
  if (msg) console.error(`gl4es-ab-catalog: ${msg}`);
  console.error('usage: gl4es-ab-catalog.mjs --a <webdir> --b <webdir> --out <dir> ' +
                '[--names names.tsv] [--examples 1,5,9 | --count N] [--time secs] [--frames N]');
  process.exit(2);
}

const opt = { time: '2.5', frames: '300', count: 0, examples: null, names: null };
const argv = process.argv.slice(2);
while (argv.length) {
  const flag = argv.shift();
  const val = argv.shift();
  if (val === undefined) usage(`${flag} needs a value`);
  if (flag === '--a') opt.a = val;
  else if (flag === '--b') opt.b = val;
  else if (flag === '--out') opt.out = val;
  else if (flag === '--names') opt.names = val;
  else if (flag === '--time') opt.time = val;
  else if (flag === '--frames') opt.frames = val;
  else if (flag === '--count') opt.count = Number(val);
  else if (flag === '--examples') opt.examples = val.split(',').map(Number);
  else usage(`unknown flag ${flag}`);
}
if (!opt.a || !opt.b || !opt.out) usage();

// names.tsv: `<index>\t<name>` lines (gl4es-ab.sh writes it from --list-examples).
const names = new Map();
if (opt.names && existsSync(opt.names))
  for (const line of readFileSync(opt.names, 'utf8').split('\n')) {
    const [idx, ...rest] = line.split('\t');
    if (idx && rest.length) names.set(Number(idx), rest.join('\t'));
  }
const examples = opt.examples ||
  Array.from({ length: opt.count || names.size }, (_, i) => i + 1);
if (examples.length === 0) usage('no examples: pass --names, --count or --examples');
mkdirSync(opt.out, { recursive: true });

const VIEW = { width: 1280, height: 800 };
const FREEZE_MS = 240000;   // heavy scenes take a while to render N frames
const server = await serveDirs({ a: opt.a, b: opt.b });
const chrome = await launchChrome();

async function capture(side, example) {
  const tab = await chrome.openPage();
  const errors = [];
  let frozen = false;
  tab.events(msg => {
    if (msg.method === 'Runtime.exceptionThrown')
      errors.push(msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text);
    else if (msg.method === 'Runtime.consoleAPICalled' &&
             msg.params.args.some(a => /GLR_FREEZE_AFTER_FRAMES: frozen/.test(a.value ?? '')))
      frozen = true;
  });
  await tab.send('Emulation.setDeviceMetricsOverride', { ...VIEW, deviceScaleFactor: 1, mobile: false });
  await tab.send('Page.addScriptToEvaluateOnNewDocument', {
    source: moduleInjection(['--example', String(example), '--no-audio'], {
      GLR_TIME: opt.time, GLR_CFG: 'auto_time=0', GLR_TICK_PER_FRAME: '1',
      GLR_FREEZE_AFTER_FRAMES: opt.frames, GLR_NO_SPLASH: '1', GLR_NO_INPUT: '1',
    }),
  });
  await tab.send('Page.navigate', { url: `http://127.0.0.1:${server.port}/${side}/index.html` });

  const t0 = Date.now();
  while (!frozen && errors.length === 0 && Date.now() - t0 < FREEZE_MS) await sleep(250);
  // Two shots of the frozen frame must agree; if they do not, something still
  // draws after the freeze and the capture is not trustworthy.
  let shot = null;
  let stable = false;
  if (frozen) {
    await sleep(300);
    shot = (await tab.send('Page.captureScreenshot', { format: 'png' })).result?.data;
    await sleep(300);
    const again = (await tab.send('Page.captureScreenshot', { format: 'png' })).result?.data;
    stable = !!shot && shot === again;
  }
  await tab.close();
  const png = join(opt.out, `${example}-${side}.png`);
  if (shot) writeFileSync(png, Buffer.from(shot, 'base64'));
  return { frozen, stable, errors, png: shot ? png : null };
}

// Differing pixels (exact compare) plus a highlighted diff image.
function diff(example, pa, pb) {
  const out = join(opt.out, `${example}-diff.png`);
  const r = spawnSync('magick', ['compare', '-metric', 'AE', pa, pb, out], { encoding: 'utf8' });
  const n = parseFloat((r.stderr || '').trim());
  return Number.isFinite(n) ? n : null;
}

const issuesOf = r => [r.a, r.b].flatMap((s, i) => [
  !s.frozen && `${'AB'[i]} never reached the freeze`,
  s.frozen && !s.stable && `${'AB'[i]} still changed after the freeze`,
  ...s.errors.map(e => `${'AB'[i]} threw: ${e.split('\n')[0]}`),
]).filter(Boolean);

const results = [];
for (const ex of examples) {
  const a = await capture('a', ex);
  const b = await capture('b', ex);
  const r = { example: ex, name: names.get(ex) || `example ${ex}`, a, b,
              diff_pixels: a.png && b.png ? diff(ex, a.png, b.png) : null };
  results.push(r);
  const issues = issuesOf(r);
  console.log(`${String(ex).padStart(3)} ${r.name.slice(0, 40).padEnd(40)} ` +
              `diff ${String(r.diff_pixels ?? '?').padStart(7)} px` +
              (issues.length ? `  [${issues.join('; ')}]` : ''));
}

server.close();
await chrome.close();
writeFileSync(join(opt.out, 'results.json'), JSON.stringify(results, null, 2) + '\n');

// --- report -----------------------------------------------------------------
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const rows = [...results].sort((x, y) => (y.diff_pixels ?? Infinity) - (x.diff_pixels ?? Infinity));
const changed = results.filter(r => r.diff_pixels !== 0).length;
const html = `<!doctype html><meta charset="utf-8"><title>gl4es A/B catalog</title>
<style>
  :root { color-scheme: light dark; --fg:#1b1d22; --bg:#fbfbfc; --mut:#646a75; --line:#dfe2e7; --bad:#b3261e; --ok:#1e7a3c; }
  @media (prefers-color-scheme: dark) { :root { --fg:#e6e8eb; --bg:#15171b; --mut:#9aa0aa; --line:#2c3038; --bad:#ff8a80; --ok:#7bd88f; } }
  body { font: 14px/1.45 system-ui, sans-serif; color: var(--fg); background: var(--bg); margin: 24px; }
  h1 { font-size: 20px; margin: 0 0 4px; } p.meta { color: var(--mut); margin: 0 0 20px; }
  section { border-top: 1px solid var(--line); padding: 16px 0; }
  h2 { font-size: 15px; margin: 0 0 8px; } .num { font-variant-numeric: tabular-nums; }
  .bad { color: var(--bad); } .ok { color: var(--ok); }
  .imgs { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
  .imgs figure { margin: 0; } .imgs img { width: 100%; border: 1px solid var(--line); }
  figcaption { color: var(--mut); font-size: 12px; }
</style>
<h1>gl4es A/B: example catalog</h1>
<p class="meta">A = ${esc(opt.a)} · B = ${esc(opt.b)} · t = ${esc(opt.time)} (paused) · frame ${esc(opt.frames)} ·
${VIEW.width}×${VIEW.height} · headless Chrome / SwiftShader ·
${changed} of ${results.length} examples differ · sorted by pixels changed</p>
${rows.map(r => {
  const issues = issuesOf(r);
  return `<section>
  <h2>${r.example}. ${esc(r.name)} —
    <span class="num ${r.diff_pixels ? 'bad' : 'ok'}">${r.diff_pixels ?? '?'} px differ</span></h2>
  ${issues.length ? `<p class="bad">${issues.map(esc).join('<br>')}</p>` : ''}
  <div class="imgs">
    <figure><img loading="lazy" src="${r.example}-a.png" alt="A"><figcaption>A</figcaption></figure>
    <figure><img loading="lazy" src="${r.example}-b.png" alt="B"><figcaption>B</figcaption></figure>
    <figure><img loading="lazy" src="${r.example}-diff.png" alt="diff"><figcaption>diff (red = changed)</figcaption></figure>
  </div></section>`;
}).join('\n')}
`;
writeFileSync(join(opt.out, 'report.html'), html);
console.log(`${changed} of ${results.length} examples differ; report: ${join(opt.out, 'report.html')}`);
