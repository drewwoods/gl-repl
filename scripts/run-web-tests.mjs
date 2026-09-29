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
// Chrome: $CHROME, else the macOS app bundle, else google-chrome / chromium
// on PATH. No npm dependencies: node >= 22 has a global WebSocket, which is
// all the DevTools protocol needs.
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join, normalize } from 'node:path';

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

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const mac = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  if (existsSync(mac)) return mac;
  for (const name of ['google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser']) {
    const r = spawnSync('sh', ['-c', `command -v ${name}`], { encoding: 'utf8' });
    if (r.status === 0 && r.stdout.trim()) return r.stdout.trim();
  }
  usage('Chrome not found; set $CHROME');
}

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript',
  '.wasm': 'application/wasm', '.data': 'application/octet-stream',
};
const server = createServer((req, res) => {
  const rel = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname));
  const file = join(dir, rel);
  if (rel.startsWith('..') || !existsSync(file)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
  res.end(readFileSync(file));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;

const profile = mkdtempSync(join(tmpdir(), 'glr-web-tests-'));
const chrome = spawn(findChrome(), [
  '--headless=new', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
  '--no-first-run', '--no-default-browser-check', `--user-data-dir=${profile}`,
  '--remote-debugging-port=0', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
const wsUrl = await new Promise((resolve, reject) => {
  let buf = '';
  chrome.stderr.on('data', d => {
    buf += d;
    const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
    if (m) resolve(m[1]);
  });
  setTimeout(() => reject(new Error('Chrome did not report a DevTools endpoint')), 20000);
});

const ws = new WebSocket(wsUrl);
await new Promise(resolve => ws.addEventListener('open', resolve));
let nextId = 0;
const pending = new Map();
let onEvent = () => {};
ws.addEventListener('message', e => {
  const msg = JSON.parse(e.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
  else onEvent(msg);
});
const send = (method, params = {}, sessionId) => new Promise(resolve => {
  const id = ++nextId;
  pending.set(id, resolve);
  ws.send(JSON.stringify({ id, method, params, sessionId }));
});

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function runPage(page) {
  const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
  const { result: { sessionId } } = await send('Target.attachToTarget', { targetId, flatten: true });
  await send('Runtime.enable', {}, sessionId);
  const out = { page, passed: 0, run: 0, status: 'TIMEOUT', failures: [], log: [] };
  let done = false;
  onEvent = msg => {
    if (msg.sessionId !== sessionId) return;
    let text = null;
    if (msg.method === 'Runtime.consoleAPICalled')
      text = msg.params.args.map(a => a.value ?? a.description ?? '').join(' ');
    else if (msg.method === 'Runtime.exceptionThrown') {
      const d = msg.params.exceptionDetails;
      text = `EXCEPTION ${d.exception?.description || d.text}`;
      out.status = 'CRASH';
      done = true;
    }
    if (text === null) return;
    out.log.push(text);
    if (/^FAIL /.test(text)) out.failures.push(text);
    const m = text.match(/:\s*(\d+)\/(\d+) passed/);
    if (m) {
      out.passed = Number(m[1]);
      out.run = Number(m[2]);
      out.status = out.passed === out.run ? 'PASS' : 'FAIL';
      done = true;
    }
  };
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/${page}` }, sessionId);
  const start = Date.now();
  while (!done && Date.now() - start < timeoutMs) await sleep(100);
  await sleep(200);   // trailing output after the summary line
  await send('Target.closeTarget', { targetId });
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

// Chrome keeps writing its profile while it shuts down, so wait for the exit
// before removing it; a leftover temp profile is not worth failing the run.
ws.close();
server.close();
const exited = new Promise(resolve => chrome.once('exit', resolve));
chrome.kill();
await Promise.race([exited, sleep(5000)]);
try { rmSync(profile, { recursive: true, force: true, maxRetries: 5 }); } catch { /* best effort */ }
process.exit(bad ? 1 : 0);
