#!/usr/bin/env node
// gl4es-ab-render.mjs - the render-bench lane of the gl4es A/B: run
// gl4es-render.html (bench/bench_render.c built against each gl4es tree) in
// headless Chrome, alternating A and B, and compare per-case frame time and
// pixel oracles. Driven by scripts/gl4es-ab.sh.
//
//   node scripts/gl4es-ab-render.mjs --a <dir> --b <dir> --out <dir> [--runs N] [--reps N]
//
// Each case draws a fixed fixed-function workload `--reps` times and ends with
// glFinish, so ms/frame is the cost of the GL path itself, not of a frame
// pacer. Every run gets a fresh Chrome; one untimed warm-up run per side goes
// first, then runs alternate A, B, A, B... so host drift lands on both sides.
// The report keeps the median and the run-to-run noise, because a delta inside
// the noise is not a result. Times are SwiftShader's: comparable between A and
// B, not a prediction for a GPU.
//
// Output: <out>/results.json, <out>/report.html.
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchChrome, moduleInjection, serveDirs, sleep } from './web-chrome.mjs';

function usage(msg) {
  if (msg) console.error(`gl4es-ab-render: ${msg}`);
  console.error('usage: gl4es-ab-render.mjs --a <dir> --b <dir> --out <dir> [--runs N] [--reps N]');
  process.exit(2);
}

const opt = { runs: 7, reps: 200 };
const argv = process.argv.slice(2);
while (argv.length) {
  const flag = argv.shift();
  const val = argv.shift();
  if (val === undefined) usage(`${flag} needs a value`);
  if (flag === '--a') opt.a = val;
  else if (flag === '--b') opt.b = val;
  else if (flag === '--out') opt.out = val;
  else if (flag === '--runs') opt.runs = Number(val);
  else if (flag === '--reps') opt.reps = Number(val);
  else usage(`unknown flag ${flag}`);
}
if (!opt.a || !opt.b || !opt.out) usage();
mkdirSync(opt.out, { recursive: true });

const server = await serveDirs({ a: opt.a, b: opt.b });

// A fresh Chrome per run: successive tabs in one browser got steadily slower
// (run 5 was up to 10x run 1 on both sides), which swamped any A/B delta.
async function runOnce(side) {
  const chrome = await launchChrome();
  const tab = await chrome.openPage();
  await tab.send('Emulation.setDeviceMetricsOverride', { width: 640, height: 480, deviceScaleFactor: 1, mobile: false });
  await tab.send('Page.addScriptToEvaluateOnNewDocument', {
    source: moduleInjection(['--reps', String(opt.reps)]),
  });
  await tab.send('Page.navigate', { url: `http://127.0.0.1:${server.port}/${side}/gl4es-render.html` });
  const t0 = Date.now();
  let bench = null;
  while (!bench && Date.now() - t0 < 180000) {
    bench = await tab.eval('window.gl4esRenderBench || null');
    if (!bench) await sleep(250);
  }
  await tab.close();
  await chrome.close();
  if (!bench) throw new Error(`${side}: render bench did not finish`);
  return bench.results;
}

await runOnce('a');   // warm-up, discarded
await runOnce('b');

// cases[name] = { a: {ms: [...], oracle: Set}, b: {...} }
const cases = new Map();
for (let run = 0; run < opt.runs; run++) {
  for (const side of ['a', 'b']) {
    for (const row of await runOnce(side)) {
      if (!cases.has(row.name)) cases.set(row.name, { a: { ms: [], oracle: new Set() }, b: { ms: [], oracle: new Set() } });
      const c = cases.get(row.name)[side];
      c.ms.push(row.msPerFrame);
      c.oracle.add(row.oracle);
    }
  }
  console.log(`run ${run + 1}/${opt.runs} done`);
}
server.close();

const median = xs => { const s = [...xs].sort((p, q) => p - q); return s[Math.floor(s.length / 2)]; };
// Relative noise of one side's runs: median absolute deviation / median. MAD,
// not max - min, because the first run or two after a cold start is an outlier
// the median already ignores.
const spread = xs => { const m = median(xs); return median(xs.map(x => Math.abs(x - m))) / m; };
const rows = [...cases].map(([name, c]) => {
  const ma = median(c.a.ms), mb = median(c.b.ms);
  const delta = mb / ma - 1;
  const noise = Math.max(spread(c.a.ms), spread(c.b.ms));
  // Some cases run in one of two speeds per browser process (attrib-stack:
  // ~0.1 or ~0.6 ms). A median then depends on how the runs happened to split,
  // so a spread past 3x marks the case unstable and it is never judged.
  const unstable = [c.a.ms, c.b.ms].some(xs => Math.max(...xs) > 3 * Math.min(...xs));
  return {
    name, a_ms: ma, b_ms: mb, delta, noise, unstable,
    // Real only if it clears twice the noise, a 3% floor, and 1 ms summed over
    // a run's reps - below that the browser's coarsened timer is the signal.
    significant: !unstable && Math.abs(delta) > Math.max(2 * noise, 0.03) &&
                 Math.abs(mb - ma) * opt.reps >= 1,
    a_oracle: [...c.a.oracle].join('/'), b_oracle: [...c.b.oracle].join('/'),
    a_runs: c.a.ms, b_runs: c.b.ms,
  };
});
writeFileSync(join(opt.out, 'results.json'), JSON.stringify(rows, null, 2) + '\n');

const pct = x => `${x >= 0 ? '+' : ''}${(x * 100).toFixed(1)}%`;
for (const r of rows)
  console.log(`${r.name.padEnd(20)} ${r.a_ms.toFixed(3).padStart(8)} -> ${r.b_ms.toFixed(3).padStart(8)} ms ` +
              `${pct(r.delta).padStart(7)} (noise ±${(r.noise * 100).toFixed(0)}%)` +
              `${r.unstable ? ' unstable' : r.significant ? ' *' : ''}  oracle ${r.a_oracle} -> ${r.b_oracle}`);

const esc = s => String(s).replace(/[&<>"]/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[ch]);
writeFileSync(join(opt.out, 'report.html'), `<!doctype html><meta charset="utf-8"><title>gl4es A/B render bench</title>
<style>
  :root { color-scheme: light dark; --fg:#1b1d22; --bg:#fbfbfc; --mut:#646a75; --line:#dfe2e7; --bad:#b3261e; --ok:#1e7a3c; }
  @media (prefers-color-scheme: dark) { :root { --fg:#e6e8eb; --bg:#15171b; --mut:#9aa0aa; --line:#2c3038; --bad:#ff8a80; --ok:#7bd88f; } }
  body { font: 14px/1.45 system-ui, sans-serif; color: var(--fg); background: var(--bg); margin: 24px; }
  h1 { font-size: 20px; margin: 0 0 4px; } p { color: var(--mut); }
  table { border-collapse: collapse; } td, th { padding: 4px 12px; border-bottom: 1px solid var(--line); text-align: right; }
  td:first-child, th:first-child { text-align: left; } td { font-variant-numeric: tabular-nums; }
  .bad { color: var(--bad); } .ok { color: var(--ok); }
</style>
<h1>gl4es A/B: render bench</h1>
<p>A = ${esc(opt.a)} · B = ${esc(opt.b)} · ${opt.runs} alternating runs × ${opt.reps} reps per case ·
headless Chrome / SwiftShader, fresh browser per run. noise = median absolute deviation over runs;
a delta is highlighted only past twice the noise, 3%, and 1 ms summed over a run's reps (timer resolution). "unstable": a side's runs spread past 3x
(the case runs at one of two speeds per browser process), so no delta is claimed.</p>
<table><tr><th>case</th><th>A ms</th><th>B ms</th><th>delta</th><th>noise</th><th>oracle A → B</th></tr>
${rows.map(r => `<tr><td>${esc(r.name)}</td><td>${r.a_ms.toFixed(3)}</td><td>${r.b_ms.toFixed(3)}</td>
<td class="${r.significant ? (r.delta > 0 ? 'bad' : 'ok') : ''}">${r.unstable ? 'unstable' : pct(r.delta)}</td><td>±${(r.noise * 100).toFixed(0)}%</td>
<td class="${r.b_oracle !== 'PASS' ? 'bad' : ''}">${esc(r.a_oracle)} → ${esc(r.b_oracle)}</td></tr>`).join('\n')}
</table>
`);
