// web-chrome.mjs - shared plumbing for driving gl-repl's wasm pages in headless
// Chrome: a throwaway localhost file server, a Chrome on SwiftShader, and a
// minimal DevTools-protocol client. Used by run-web-tests.mjs (test pages) and
// gl4es-ab-catalog.mjs (example screenshots).
//
// SwiftShader is the point, not a fallback: it is a software rasterizer that
// gives the same pixels and the same GL limits on every machine, so a result
// depends on gl4es and the page, never on the host GPU.
//
// Chrome: $CHROME, else the macOS app bundle, else google-chrome / chromium on
// PATH. No npm dependencies: node >= 22 has a global WebSocket.
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { extname, join, normalize } from 'node:path';

export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const mac = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  if (existsSync(mac)) return mac;
  for (const name of ['google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser']) {
    const r = spawnSync('sh', ['-c', `command -v ${name}`], { encoding: 'utf8' });
    if (r.status === 0 && r.stdout.trim()) return r.stdout.trim();
  }
  throw new Error('Chrome not found; set $CHROME');
}

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.json': 'application/json',
  '.png': 'image/png', '.svg': 'image/svg+xml',
};

// Serve each root under its own URL prefix: serveDirs({a: dirA, b: dirB})
// answers http://127.0.0.1:<port>/a/... from dirA. Returns { port, close }.
export async function serveDirs(roots) {
  const server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    const [, prefix, ...rest] = path.split('/');
    const root = roots[prefix];
    const rel = normalize(rest.join('/'));
    const file = root && join(root, rel);
    if (!file || rel.startsWith('..') || !existsSync(file)) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(readFileSync(file));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  return { port: server.address().port, close: () => server.close() };
}

// Launch headless Chrome and connect. extraArgs go on the command line.
// Returns { send(method, params, sessionId), on(listener), openPage(), close() }.
export async function launchChrome(extraArgs = []) {
  const profile = mkdtempSync(join(tmpdir(), 'glr-chrome-'));
  const chrome = spawn(findChrome(), [
    '--headless=new', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--no-first-run', '--no-default-browser-check', `--user-data-dir=${profile}`,
    '--remote-debugging-port=0', ...extraArgs, 'about:blank',
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
  const listeners = new Set();
  ws.addEventListener('message', e => {
    const msg = JSON.parse(e.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
    else listeners.forEach(l => l(msg));
  });
  const send = (method, params = {}, sessionId) => new Promise(resolve => {
    const id = ++nextId;
    pending.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params, sessionId }));
  });

  // A fresh tab with Runtime/Page events on. `events(fn)` sees only this tab's
  // events; `eval(expr)` returns the value; `close()` closes the tab.
  async function openPage() {
    const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
    const { result: { sessionId } } = await send('Target.attachToTarget', { targetId, flatten: true });
    await send('Runtime.enable', {}, sessionId);
    await send('Page.enable', {}, sessionId);
    const mine = new Set();
    const tap = msg => { if (msg.sessionId === sessionId) mine.forEach(fn => fn(msg)); };
    listeners.add(tap);
    return {
      send: (method, params) => send(method, params, sessionId),
      events: fn => mine.add(fn),
      eval: async expr => {
        const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }, sessionId);
        return r.result?.result?.value;
      },
      close: async () => { listeners.delete(tap); await send('Target.closeTarget', { targetId }); },
    };
  }

  async function close() {
    ws.close();
    const exited = new Promise(resolve => chrome.once('exit', resolve));
    chrome.kill();
    await Promise.race([exited, sleep(5000)]);
    // Chrome writes its profile while shutting down; a leftover is harmless.
    try { rmSync(profile, { recursive: true, force: true, maxRetries: 5 }); } catch { /* best effort */ }
  }

  return { send, openPage, close };
}

// Page script for Page.addScriptToEvaluateOnNewDocument that gives an
// Emscripten page argv and environment it has no other way to receive: it
// wraps `window.Module`, so the page's own `var Module = {...}` gets
// `arguments` plus a preRun that fills Emscripten's ENV before main() runs.
export function moduleInjection(args, env = {}) {
  return `(() => {
    const inj = ${JSON.stringify({ args, env })};
    let real;
    Object.defineProperty(window, 'Module', { configurable: true,
      get() { return real; },
      set(v) {
        if (v && typeof v === 'object' && !v.__glrInjected) {
          v.__glrInjected = 1;
          v.arguments = inj.args;
          v.preRun = [].concat(v.preRun || [], () => { Object.assign(ENV, inj.env); });
        }
        real = v;
      } });
  })();`;
}

// Console text of a Runtime.consoleAPICalled / exceptionThrown event, or null.
export function consoleText(msg) {
  if (msg.method === 'Runtime.consoleAPICalled')
    return msg.params.args.map(a => a.value ?? a.description ?? '').join(' ');
  if (msg.method === 'Runtime.exceptionThrown') {
    const d = msg.params.exceptionDetails;
    return `EXCEPTION ${d.exception?.description || d.text}`;
  }
  return null;
}
