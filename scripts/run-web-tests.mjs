#!/usr/bin/env node
// run-web-tests.mjs - run gl-repl test pages (wasm, linked against gl4es) in
// headless Chrome and report each page's test-harness result.
//
//   node scripts/run-web-tests.mjs [--json out.json] [--known gaps.txt]
//                                  [--timeout ms] <dir> <page.html>...
//
// Each page is a test binary built with -sEXIT_RUNTIME=1 whose main() prints
// the tests/support/test_harness.h summary line ("<suite>: N/M passed"). The
// page is served from <dir> over a throwaway localhost server and loaded in
// headless Chrome on SwiftShader, so the GL under test is gl4es -> WebGL2 ->
// a software rasterizer that behaves the same on every machine.
//
// --known names an allow list of expected failures (format and rationale in
// packaging/web/gl4es-known-gaps.txt). A page whose every failure is listed is
// KNOWN rather than FAIL; entries that matched nothing are reported, because a
// gap that stopped failing should come off the list. Exit status is 0 unless
// some page FAILs (an unlisted failure, crash, or timeout).
//
// Browser plumbing (Chrome lookup, SwiftShader, file server, DevTools client)
// lives in web-chrome.mjs, shared with gl4es-ab-catalog.mjs.
import { readFileSync, writeFileSync } from 'node:fs';
import { consoleText, launchChrome, serveDirs, sleep } from './web-chrome.mjs';

function usage(msg) {
  if (msg) console.error(`run-web-tests: ${msg}`);
  console.error('usage: run-web-tests.mjs [--json out.json] [--known gaps.txt] [--timeout ms] <dir> <page.html>...');
  process.exit(2);
}

const args = process.argv.slice(2);
let jsonOut = null;
let knownFile = null;
let timeoutMs = 120000;
while (args.length && args[0].startsWith('--')) {
  const flag = args.shift();
  if (flag === '--json') jsonOut = args.shift();
  else if (flag === '--known') knownFile = args.shift();
  else if (flag === '--timeout') timeoutMs = Number(args.shift());
  else usage(`unknown flag ${flag}`);
}
const [dir, ...pages] = args;
if (!dir || pages.length === 0) usage();

// `page | label` lines; `*` is a wildcard, everything else literal.
function loadKnown(file) {
  const entries = [];
  for (const raw of readFileSync(file, 'utf8').split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const bar = line.indexOf('|');
    if (bar < 0) usage(`${file}: expected "page | label": ${line}`);
    const page = line.slice(0, bar).trim();
    const label = line.slice(bar + 1).trim();
    const re = new RegExp('^' + label.split('*').map(
      part => part.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('.*') + '$');
    entries.push({ page, label, re, hits: 0 });
  }
  return entries;
}
const known = knownFile ? loadKnown(knownFile) : [];

const server = await serveDirs({ t: dir });
const chrome = await launchChrome();

async function runPage(page) {
  const tab = await chrome.openPage();
  const out = { page, passed: 0, run: 0, status: 'TIMEOUT', failures: [], log: [] };
  let done = false;
  tab.events(msg => {
    const text = consoleText(msg);
    if (text === null) return;
    out.log.push(text);
    if (msg.method === 'Runtime.exceptionThrown') { out.status = 'CRASH'; done = true; }
    if (/^FAIL /.test(text)) out.failures.push(text);
    const m = text.match(/:\s*(\d+)\/(\d+) passed/);
    if (m) {
      out.passed = Number(m[1]);
      out.run = Number(m[2]);
      out.status = out.passed === out.run ? 'PASS' : 'FAIL';
      done = true;
    }
  });
  await tab.send('Page.navigate', { url: `http://127.0.0.1:${server.port}/t/${page}` });
  const start = Date.now();
  while (!done && Date.now() - start < timeoutMs) await sleep(100);
  await sleep(200);   // trailing output after the summary line
  await tab.close();
  return out;
}

// Split a page's failures into listed (known) and new, and settle its verdict.
function classify(r) {
  const name = r.page.replace(/\.html$/, '');
  const mine = known.filter(k => k.page === name);
  const match = label => {
    const k = mine.find(e => e.re.test(label));
    if (k) k.hits++;
    return !!k;
  };
  r.known = [];
  r.unknown = [];
  for (const f of r.failures) {
    const label = (f.match(/^FAIL \[(.*)\]/) || [null, f])[1];
    (match(label) ? r.known : r.unknown).push(f);
  }
  if (r.status === 'CRASH' || r.status === 'TIMEOUT') {
    if (!match(r.status)) r.unknown.push(r.status);
    else r.known.push(r.status);
  }
  r.verdict = r.unknown.length ? 'FAIL' : r.known.length ? 'KNOWN' : 'PASS';
}

const results = [];
for (const page of pages) {
  const r = await runPage(page);
  classify(r);
  results.push(r);
  for (const f of r.unknown) console.log(`  new: ${f}`);
  const counts = r.run ? ` (${r.passed}/${r.run}${r.known.length ? `, ${r.known.length} known` : ''})` : '';
  console.log(`${r.verdict.padEnd(7)} ${page}${r.status === 'CRASH' || r.status === 'TIMEOUT' ? ` [${r.status}]` : ''}${counts}`);
}
const tested = new Set(pages.map(p => p.replace(/\.html$/, '')));
const stale = known.filter(k => tested.has(k.page) && k.hits === 0);
for (const k of stale) console.log(`no longer failing (remove from ${knownFile}): ${k.page} | ${k.label}`);

if (jsonOut) writeFileSync(jsonOut, JSON.stringify(results, null, 2) + '\n');
const bad = results.filter(r => r.verdict === 'FAIL').length;
const knownPages = results.filter(r => r.verdict === 'KNOWN').length;
console.log(`${results.length - bad}/${results.length} pages ok` +
            (knownPages ? ` (${knownPages} with known gl4es gaps only)` : '') +
            (stale.length ? `, ${stale.length} stale known-gap entr${stale.length === 1 ? 'y' : 'ies'}` : ''));

server.close();
await chrome.close();
process.exit(bad ? 1 : 0);
