#!/usr/bin/env node
/* Endpoint tests for server.js, using node:test only.
 *
 * The server runs as a real child process, so these tests exercise the
 * actual HTTP surface (routing, validation, path handling) instead of
 * re-implementing it. Most of the bugs fixed in this repository were
 * exactly "the code looked right but the route accepted something it
 * should not".
 *
 * Run: node --test tools/server_test.js
 */
'use strict';

const { test, before, after } = require('node:test');
const assert = require('node:assert');
const { spawn, execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { writeGgufFile } = require('./gguf_fixture');

const ROOT = path.resolve(__dirname, '..');
const PORT = 8000 + Math.floor(process.pid % 1000);
const BASE = `http://127.0.0.1:${PORT}`;

let child = null;
let tmpModels = null;

/* Files a test creates inside models/uploaded/; removed in after(). */
const createdFiles = [];

function request(pathname, options = {}) {
  return fetch(BASE + pathname, options);
}

async function json(pathname, options = {}) {
  const res = await request(pathname, options);
  const body = await res.json().catch(() => ({}));
  return { status: res.status, body };
}

function postJSON(pathname, payload) {
  return json(pathname, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
}

before(async () => {
  /* The engine binary must exist: the tests assert real engine answers,
   * not mocked ones. */
  try {
    execFileSync('make', ['all'], { cwd: ROOT, stdio: 'ignore' });
  } catch {
    // already built or no make; the first request will tell us
  }
  tmpModels = fs.mkdtempSync(path.join(os.tmpdir(), 'hydra-models-'));
  child = spawn(process.execPath, ['server.js'], {
    cwd: ROOT,
    env: {
      ...process.env,
      PORT: String(PORT),
      /* Unset on purpose: the console must bind to loopback by default. */
      HYDRA_ALLOW_REMOTE: '',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stderr.on('data', () => {});

  // wait for readiness
  const deadline = Date.now() + 15000;
  for (;;) {
    try {
      const res = await fetch(`${BASE}/api/models`);
      if (res.ok) break;
    } catch {
      /* not up yet */
    }
    if (Date.now() > deadline) throw new Error('server did not start');
    await new Promise((r) => setTimeout(r, 150));
  }
});

after(async () => {
  if (child) child.kill('SIGKILL');
  if (tmpModels) fs.rmSync(tmpModels, { recursive: true, force: true });
  /* Files the diagnostics tests wrote inside models/ must not survive: a
   * leftover "kaputt-dim.hydra" would show up in the console's model list
   * and in the next run's own assertions. */
  for (const f of createdFiles) fs.rmSync(f, { force: true });
});

test('/api/models returns JSON with an array of models', async () => {
  const { status, body } = await json('/api/models');
  assert.strictEqual(status, 200);
  assert.ok(Array.isArray(body.models), 'models must be an array');
  assert.ok(body.models.length > 0, 'the starter model must be listed');
  for (const m of body.models) {
    assert.ok(m.path.endsWith('.hydra'), `bad path: ${m.path}`);
    assert.strictEqual(typeof m.dim, 'number');
    assert.strictEqual(typeof m.vocab, 'number');
  }
});

test('/api/infer rejects a non-object body', async () => {
  const { status } = await json('/api/infer', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '[1,2,3]',
  });
  assert.strictEqual(status, 400);
});

test('/api/infer rejects invalid JSON', async () => {
  const { status } = await json('/api/infer', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '{not json',
  });
  assert.strictEqual(status, 400);
});

test('/api/infer rejects non-finite token and steps', async () => {
  const { status } = await postJSON('/api/infer', { token: 'abc', steps: 4 });
  assert.strictEqual(status, 400);
});

test('safeModelPath rejects path escapes and non-.hydra names', async () => {
  const escapes = [
    '../../etc/passwd',
    '/etc/passwd',
    'models/../../etc/hostname',
    '../../../etc/hostname.hydra',
    'server.js',
    'models/starter.hydra/../../../../etc/hostname',
  ];
  for (const p of escapes) {
    const { status, body } = await postJSON('/api/infer', { path: p, token: 1, steps: 1 });
    assert.strictEqual(status, 400, `expected 400 for ${p}, got ${status}`);
    assert.ok(body.error, `expected an error for ${p}`);
  }
});

test('/api/infer on a valid model returns a token stream', async () => {
  const { body: list } = await json('/api/models');
  const model = list.models.find((m) => m.valid) || list.models[0];
  const { status, body } = await postJSON('/api/infer', {
    path: model.path,
    token: 7,
    steps: 5,
  });
  assert.strictEqual(status, 200);
  assert.ok(Array.isArray(body.tokens), 'tokens must be an array');
  assert.strictEqual(body.tokens.length, 5);
  assert.ok(Array.isArray(body.state));
});

test('/api/infer forwards the whole prompt, not just its first token', async () => {
  const { body: list } = await json('/api/models');
  const model = list.models.find((m) => m.valid) || list.models[0];

  const { status, body } = await postJSON('/api/infer', {
    path: model.path,
    token: 7,
    steps: 4,
    prompt: [7, 9, 11],
  });
  assert.strictEqual(status, 200);
  assert.deepStrictEqual(body.prompt, [7, 9, 11], 'the prompt must reach the engine');

  // A one-token prompt must produce the same stream as the bare seed:
  // prefill with n == 1 is defined to be a no-op.
  const bare = await postJSON('/api/infer', { path: model.path, token: 7, steps: 4 });
  const single = await postJSON('/api/infer', {
    path: model.path,
    token: 7,
    steps: 4,
    prompt: [7],
  });
  assert.deepStrictEqual(
    single.body.tokens,
    bare.body.tokens,
    'prompt [7] must equal seed 7',
  );

  // A longer prompt must change the result, otherwise prefill is a no-op
  // and the whole feature would be theatre.
  const longer = await postJSON('/api/infer', {
    path: model.path,
    token: 11,
    steps: 4,
    prompt: [3, 5, 7, 9, 11],
  });
  assert.notDeepStrictEqual(
    longer.body.tokens,
    bare.body.tokens,
    'a longer prompt must change the output',
  );
});

test('/api/infer rejects a malformed prompt', async () => {
  const { body: list } = await json('/api/models');
  const model = list.models[0];
  for (const prompt of ['nope', [1, 'x'], [-1], [70000], [1, 2, 3, 4, 5, 6, 7, 8].concat(new Array(300).fill(1))]) {
    const { status } = await postJSON('/api/infer', { path: model.path, prompt, steps: 2 });
    assert.strictEqual(status, 400, `expected 400 for prompt ${JSON.stringify(prompt).slice(0, 40)}`);
  }
});

test('/api/models/upload stores a valid model and it appears in the list', async () => {
  const modelPath = path.join(os.tmpdir(), `hydra-upload-${process.pid}.hydra`);
  execFileSync('python3', [path.join(ROOT, 'tools', 'make_model.py'), modelPath], {
    stdio: 'ignore',
  });
  const bytes = fs.readFileSync(modelPath);

  const before = (await json('/api/models')).body.models.length;
  const res = await fetch(`${BASE}/api/models/upload?name=uploaded-${process.pid}.hydra`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: bytes,
  });
  /* Registered for cleanup: this test used to leave its upload behind, and
   * after a few runs models/uploaded/ was full of them - which then showed
   * up in the console's model list and in every later assertion about it. */
  createdFiles.push(path.join(ROOT, 'models', 'uploaded', `uploaded-${process.pid}.hydra`));
  assert.strictEqual(res.status, 200);
  const body = await res.json();
  assert.strictEqual(body.ok, true);
  assert.strictEqual(body.model.valid, true);
  assert.ok(body.model.path.includes(`uploaded-${process.pid}`), body.model.path);

  const after = (await json('/api/models')).body.models.length;
  assert.ok(after > before, 'the uploaded model must be listed');

  fs.unlinkSync(modelPath);
});

test('/api/models/upload rejects invalid bytes and stores nothing', async () => {
  const before = (await json('/api/models')).body.models.length;

  const cases = [
    ['too small', Buffer.alloc(10)],
    ['wrong magic', (() => {
      const b = Buffer.alloc(24 + 64);
      b.writeUInt32LE(0xdeadbeef, 0);
      return b;
    })()],
    ['bad version', (() => {
      const b = Buffer.alloc(24 + 64);
      b.writeUInt32LE(0x48594452, 0);
      b.writeUInt16LE(99, 4);
      return b;
    })()],
    ['dim out of range', (() => {
      const b = Buffer.alloc(24 + 64);
      b.writeUInt32LE(0x48594452, 0);
      b.writeUInt16LE(1, 4);
      b.writeUInt16LE(512, 6);
      b.writeUInt32LE(999, 8);
      b.writeUInt32LE(1, 12);
      b.writeUInt32LE(24, 16);
      b.writeUInt32LE(64, 20);
      return b;
    })()],
    ['weights past EOF', (() => {
      const b = Buffer.alloc(24 + 8);
      b.writeUInt32LE(0x48594452, 0);
      b.writeUInt16LE(1, 4);
      b.writeUInt16LE(512, 6);
      b.writeUInt32LE(16, 8);
      b.writeUInt32LE(1, 12);
      b.writeUInt32LE(24, 16);
      b.writeUInt32LE(4096, 20);
      return b;
    })()],
    ['layers*dim > weights_len', (() => {
      const b = Buffer.alloc(24 + 8);
      b.writeUInt32LE(0x48594452, 0);
      b.writeUInt16LE(1, 4);
      b.writeUInt16LE(512, 6);
      b.writeUInt32LE(16, 8);
      b.writeUInt32LE(8, 12);
      b.writeUInt32LE(24, 16);
      b.writeUInt32LE(16, 20);
      return b;
    })()],
  ];

  for (const [name, bytes] of cases) {
    const res = await fetch(`${BASE}/api/models/upload?name=bad-${process.pid}-${name.replace(/\W+/g, '-')}`, {
      method: 'POST',
      body: bytes,
    });
    assert.strictEqual(res.status, 400, `${name}: expected 400, got ${res.status}`);
    const body = await res.json();
    assert.ok(body.error, `${name}: expected an error message`);
  }

  const after = (await json('/api/models')).body.models.length;
  assert.strictEqual(after, before, 'a rejected upload must not be stored');
});

test('/api/models/upload rejects a dangerous file name', async () => {
  for (const name of ['../../evil', 'evil.hydra.hydra/../x', '/etc/evil', 'no-suffix']) {
    const res = await fetch(`${BASE}/api/models/upload?name=${encodeURIComponent(name)}`, {
      method: 'POST',
      body: Buffer.alloc(64),
    });
    assert.strictEqual(res.status, 400, `expected 400 for name ${name}`);
  }
});

test('unknown API endpoints are 404, static traversal is refused', async () => {
  assert.strictEqual((await json('/api/nope')).status, 404);
  for (const p of ['/../server.js', '/..%2fserver.js', '/./../package.json']) {
    const res = await request(p);
    assert.ok(res.status === 403 || res.status === 404, `${p}: got ${res.status}`);
    const text = await res.text();
    assert.ok(!text.includes('createServer'), `${p} leaked server.js`);
  }
});

test('every element the console looks up by id exists in the markup', () => {
  // There is no DOM in node:test, so this is the cheap guard that catches the
  // most common console breakage: a renamed id in index.html that app.js
  // still queries. Without it the failure only shows up as a blank panel.
  const js = fs.readFileSync(path.join(ROOT, 'public', 'app.js'), 'utf8');
  const html = fs.readFileSync(path.join(ROOT, 'public', 'index.html'), 'utf8');
  const ids = new Set();
  for (const m of js.matchAll(/\$\('([^']+)'\)/g)) ids.add(m[1]);
  for (const m of js.matchAll(/getElementById\('([^']+)'\)/g)) ids.add(m[1]);
  const present = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));
  const missing = [...ids].filter((id) => !present.has(id));
  assert.deepStrictEqual(missing, [], `app.js queries ids that do not exist: ${missing}`);
});

test('accessibility wiring in the markup points at existing ids', () => {
  const html = fs.readFileSync(path.join(ROOT, 'public', 'index.html'), 'utf8');
  const present = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));
  for (const attr of ['aria-labelledby', 'aria-controls', 'for']) {
    for (const m of html.matchAll(new RegExp(`${attr}="([^"]+)"`, 'g'))) {
      for (const ref of m[1].split(/\s+/)) {
        assert.ok(present.has(ref), `${attr}="${ref}" has no matching element`);
      }
    }
  }
  // The composer must have a real label, not only a placeholder.
  assert.match(html, /<label[^>]*for="input"/, 'the composer needs a <label for="input">');
  // The canvas needs a text alternative that is updated with the data.
  assert.match(html, /id="chart-summary"/, 'the canvas needs a visually hidden summary');
});

/* ------------------------------------------------------------------ */
/* Headless smoke test of public/app.js.
 *
 * There is no browser in CI, so a typo in the frontend would otherwise only
 * surface as an empty page. The stub below is deliberately dumb: it knows
 * the ids in index.html and returns plain objects for them. That is enough
 * to catch the failure mode that actually happens - app.js asking for an
 * element that does not exist, or calling a method on null - because every
 * such call throws during init().
 * ------------------------------------------------------------------ */
function makeStubDocument() {
  const html = fs.readFileSync(path.join(ROOT, 'public', 'index.html'), 'utf8');
  const ids = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));

  const make = (tag) => ({
    tagName: tag,
    id: '',
    className: '',
    hidden: false,
    disabled: false,
    value: '',
    textContent: '',
    dataset: {},
    style: {},
    files: [],
    classList: {
      _s: new Set(),
      add(...c) { c.forEach((x) => this._s.add(x)); },
      remove(...c) { c.forEach((x) => this._s.delete(x)); },
      toggle(c, on) { if (on === undefined) { this._s.has(c) ? this._s.delete(c) : this._s.add(c); } else if (on) this._s.add(c); else this._s.delete(c); },
      contains(c) { return this._s.has(c); },
    },
    listeners: {},
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    removeEventListener() {},
    appendChild(c) { (this.children ||= []).push(c); return c; },
    removeChild(c) { this.children = (this.children || []).filter((x) => x !== c); },
    querySelectorAll() { return []; },
    querySelector() { return null; },
    setAttribute(k, v) { this[k] = v; },
    getAttribute() { return null; },
    focus() { doc.activeElement = this; },
    showModal() { this.open = true; },
    close() { this.open = false; },
    dispatch(type, ev = {}) { (this.listeners[type] || []).forEach((fn) => fn({ preventDefault() {}, ...ev })); },
    getContext() {
      return { setTransform() {}, clearRect() {}, createLinearGradient: () => ({ addColorStop() {} }), fillRect() {}, fillText() {} };
    },
    get clientWidth() { return 600; },
    get offsetParent() { return this; },
  });

  const doc = {
    readyState: 'complete',
    activeElement: null,
    createElement: (tag) => make(tag),
    createTextNode: (t) => ({ text: t }),
    getElementById(id) { return ids.has(id) ? Object.assign(make('div'), { id }) : null; },
    addEventListener() {},
  };
  return { doc, ids };
}

test('public/app.js initialises against the real markup without throwing', async () => {
  const { doc } = makeStubDocument();
  const errors = [];
  const realFetch = globalThis.fetch;
  globalThis.fetch = async (p) => {
    if (String(p).startsWith('/api/models')) {
      return { ok: true, json: async () => ({ models: [{ path: 'models/starter.hydra', dim: 16, vocab: 512, layers: 2, bytes: 280, magic: 1213809746, valid: true }], default: 'models/starter.hydra' }) };
    }
    return { ok: true, json: async () => ({}) };
  };
  globalThis.window = { devicePixelRatio: 1, addEventListener() {} };
  globalThis.performance = globalThis.performance || { now: () => 0 };

  const appPath = path.join(ROOT, 'public', 'app.js');
  delete require.cache[require.resolve(appPath)];
  try {
    const vm = require('node:vm');
    vm.runInNewContext(fs.readFileSync(appPath, 'utf8'), {
      document: doc,
      window: globalThis.window,
      fetch: globalThis.fetch,
      console,
      performance: globalThis.performance,
      AbortController,
      navigator: { clipboard: null },
      location: { href: 'http://127.0.0.1/' },
      setInterval: () => 0,
      Object,
      Array,
      Number,
      JSON,
      Math,
      String,
      Error,
      Promise,
      setTimeout,
      document_: null,
    });
  } catch (e) {
    errors.push(e);
  } finally {
    globalThis.fetch = realFetch;
  }
  assert.deepStrictEqual(errors.map((e) => e.message), [], 'app.js must initialise cleanly');
});

/* A stand-in engine that outlives the request, so "the child was killed"
 * is an observable fact instead of a race.
 *
 * The real engine answers 256 steps in microseconds, so counting processes
 * after a cancel proved nothing: the child had already exited on its own and
 * the test passed with the kill removed (verified - see errors.md). This
 * fake records its own pid, sleeps, and only then prints a valid answer, so
 * the pid disappearing can only mean the server signalled it. */
function withSlowEngine(run) {
  const bin = path.join(ROOT, 'hydra-run');
  const real = path.join(ROOT, 'hydra-run.real-for-test');
  const pidFile = path.join(ROOT, 'hydra-run.fake-pid');
  fs.rmSync(pidFile, { force: true });
  fs.renameSync(bin, real);
  fs.writeFileSync(bin,
    '#!/bin/bash\n'
    + `echo $$ > ${pidFile}\n`
    + 'exec -a hydra-fake-engine sleep 10\n');
  fs.chmodSync(bin, 0o755);

  /* If the test process is killed outright the finally block never runs and
   * the repository is left with a sleeping stand-in named hydra-run, which
   * makes every later test look broken. The exit hook is the safety net. */
  const restore = () => {
    fs.rmSync(pidFile, { force: true });
    if (!fs.existsSync(real)) return;
    fs.rmSync(bin, { force: true });
    fs.renameSync(real, bin);
  };
  process.once('exit', restore);

  const readPid = () => {
    try {
      return Number(fs.readFileSync(pidFile, 'utf8').trim());
    } catch {
      return 0;
    }
  };
  const alive = (pid) => {
    try {
      process.kill(pid, 0);
      return true;
    } catch {
      return false;
    }
  };
  const waitFor = async (fn, ms, what) => {
    const deadline = Date.now() + ms;
    for (;;) {
      if (fn()) return true;
      if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
      await new Promise((r) => setTimeout(r, 25));
    }
  };
  const waitGone = async (pid, ms) => {
    const deadline = Date.now() + ms;
    while (alive(pid) && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 25));
    }
    return !alive(pid);
  };

  return run({ readPid, alive, waitFor, waitGone }).finally(() => {
    const pid = readPid();
    if (pid && alive(pid)) {
      try { process.kill(pid, 'SIGKILL'); } catch { /* gone */ }
    }
    process.removeListener('exit', restore);
    restore();
  });
}

test('POST /api/cancel kills the engine process it addresses', async () => {
  const { body: list } = await json('/api/models');
  const model = list.models.find((m) => m.valid) || list.models[0];
  const callId = `cancel-explicit-${process.pid}`;

  await withSlowEngine(async (env) => {
    /* The pending request must be bounded: a server that never answers a
     * cancelled call would hang the test instead of failing it. */
    const pending = fetch(`${BASE}/api/infer`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: model.path, token: 1, steps: 8, request_id: callId }),
      signal: AbortSignal.timeout(5000),
    }).then((r) => r.status).catch((e) => `fetch failed: ${e.name}`);

    await env.waitFor(() => env.readPid() > 0, 5000, 'the engine child to start');
    const pid = env.readPid();
    assert.ok(env.alive(pid), 'the slow engine must be running before we cancel');

    const res = await postJSON('/api/cancel', { id: callId });
    assert.strictEqual(res.status, 200, 'a live request id must be cancellable');
    assert.strictEqual(res.body.killed, true);

    assert.ok(await env.waitGone(pid, 3000),
      `engine process ${pid} survived an explicit cancel`);

    /* A cancelled call must not look like an engine failure, and it must
     * not leave the caller waiting forever either. */
    assert.strictEqual(await pending, 409,
      'a cancelled run must answer 409, not hang and not report an error');
  });

  /* The server is unharmed afterwards: a real request still works. */
  const res = await postJSON('/api/infer', { path: model.path, token: 1, steps: 4 });
  assert.strictEqual(res.status, 200);
  assert.strictEqual(res.body.tokens.length, 4);
});

test('a client that hangs up kills its engine process', async () => {
  const { body: list } = await json('/api/models');
  const model = list.models.find((m) => m.valid) || list.models[0];

  await withSlowEngine(async (env) => {
    const ac = new AbortController();
    fetch(`${BASE}/api/infer`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: model.path, token: 1, steps: 8 }),
      signal: ac.signal,
    }).catch(() => {});

    await env.waitFor(() => env.readPid() > 0, 5000, 'the engine child to start');
    const pid = env.readPid();
    ac.abort();

    assert.ok(await env.waitGone(pid, 3000),
      `engine process ${pid} survived an abandoned request`);
  });
});

test('/api/cancel rejects garbage', async () => {
  for (const body of [{ id: '' }, { id: 'nope-' + process.pid }, { }]) {
    const res = await postJSON('/api/cancel', body);
    assert.strictEqual(res.status, 404, `expected 404 for ${JSON.stringify(body)}`);
  }
});

test('/api/errors exposes the crash log and reports a real failure', async () => {
  const { status, body } = await json('/api/errors');
  assert.strictEqual(status, 200);
  assert.ok(Array.isArray(body.errors));

  /* Provoke a real 500 by taking the engine binary away for one request:
   safeModelPath still succeeds, the execFile then fails, and the route
   lands in the outer catch. Restored immediately afterwards. */
  const bin = path.join(ROOT, 'hydra-run');
  const hidden = path.join(ROOT, 'hydra-run.hidden-for-test');
  const { body: list2 } = await json('/api/models');
  const model = list2.models.find((m) => m.valid) || list2.models[0];
  fs.renameSync(bin, hidden);
  let bad;
  try {
    bad = await postJSON('/api/infer', { path: model.path, token: 1, steps: 1 });
  } finally {
    fs.renameSync(hidden, bin);
  }
  assert.strictEqual(bad.status, 500, `expected a 500, got ${bad.status}`);
  assert.strictEqual(bad.body.error, 'internal error',
    'the client must not learn what failed internally');

  const after = await json('/api/errors');
  assert.ok(after.body.errors.length > body.errors.length,
    'a failed request must be recorded in the crash log');
  const last = after.body.errors[0];
  assert.ok(last.when && last.message, 'an error entry needs a timestamp and a message');
  /* Stack traces must never be echoed back into a regular response. */
  assert.ok(!JSON.stringify(bad.body).includes('at Object.'),
    'the HTTP response must not leak stack traces');
});

test('the crash dialog and the error button exist and are wired', () => {
  const js = fs.readFileSync(path.join(ROOT, 'public', 'app.js'), 'utf8');
  const html = fs.readFileSync(path.join(ROOT, 'public', 'index.html'), 'utf8');
  assert.match(html, /id="error-dialog"/, 'the crash dialog must exist');
  assert.match(html, /id="error-copy"/, 'the copy button must exist');
  assert.match(html, /id="errors-open"/, 'the error indicator must exist');
  assert.match(js, /addEventListener\('error'/, 'window.onerror must be handled');
  assert.match(js, /addEventListener\('unhandledrejection'/, 'promise rejections must be handled');
  assert.match(js, /clipboard/, 'the copy must use the clipboard API with a fallback');
  assert.match(js, /execCommand\('copy'\)/, 'a non-secure-context fallback must exist');
});

/* ------------------------------------------------------------------ */
/* Model diagnostics: why is my file not loading?                       */
/* ------------------------------------------------------------------ */

/** Writes a .hydra header with chosen values into models/uploaded/. */
function writeModelFile(name, fields) {
  const dir = path.join(ROOT, 'models', 'uploaded');
  fs.mkdirSync(dir, { recursive: true });
  const b = Buffer.alloc(24);
  b.writeUInt32LE(fields.magic ?? 0x48594452, 0);
  b.writeUInt16LE(fields.version ?? 1, 4);
  b.writeUInt16LE(fields.vocab ?? 512, 6);
  b.writeUInt32LE(fields.dim ?? 64, 8);
  b.writeUInt32LE(fields.layers ?? 4, 12);
  b.writeUInt32LE(fields.weightsOffset ?? 24, 16);
  b.writeUInt32LE(fields.weightsLen ?? 256, 20);
  const file = path.join(dir, name);
  fs.writeFileSync(file, Buffer.concat([b, Buffer.alloc(fields.pad ?? 256)]));
  createdFiles.push(file);
  return 'models/uploaded/' + name;
}

/** Boots an extra server with extra env, e.g. a smaller upload cap. */
async function bootServer(t, extraEnv) {
  const port = PORT + 3 + Math.floor(Math.random() * 300);
  const srv = spawn(process.execPath, ['server.js'], {
    cwd: ROOT,
    env: { ...process.env, PORT: String(port), HYDRA_ALLOW_REMOTE: '', ...extraEnv },
    stdio: ['ignore', 'ignore', 'ignore'],
  });
  t.after(() => srv.kill('SIGKILL'));
  const base = `http://127.0.0.1:${port}`;
  const deadline = Date.now() + 15000;
  for (;;) {
    try {
      const res = await fetch(`${base}/api/models`);
      if (res.ok) return base;
    } catch {
      /* not up yet */
    }
    if (Date.now() > deadline) throw new Error('secondary server did not start');
    await new Promise((r) => setTimeout(r, 100));
  }
}

test('an over-sized upload answers 413 with a body the client can read', async (t) => {
  /* Before the fix the limit path called req.destroy() first, so the client
   * received an EMPTY reply: a broken connection instead of a status code.
   * Asserting a non-empty JSON body is therefore the regression test. */
  const base = await bootServer(t, { HYDRA_MAX_UPLOAD: '1MiB' });
  const body = Buffer.alloc(3 * 1024 * 1024, 0x41);

  const res = await fetch(`${base}/api/models/upload?name=zu-gross.hydra`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body,
  });
  const text = await res.text();
  assert.strictEqual(res.status, 413, 'an over-sized upload must be refused with 413');
  assert.ok(text.length > 0, 'the 413 must carry a body - an empty reply is the bug');
  const json = JSON.parse(text);
  assert.match(json.error, /larger than the 1\.0 MiB limit/,
    `the message must name the limit, got: ${json.error}`);
  assert.strictEqual(json.rejected, true);
  assert.strictEqual(json.limitBytes, 1024 * 1024);
  assert.strictEqual(json.raiseWith, 'HYDRA_MAX_UPLOAD',
    'the error must say how to raise the limit');

  /* Nothing may be left behind: the body was streamed to a temp file. */
  const uploaded = path.join(ROOT, 'models', 'uploaded');
  const leftovers = fs.existsSync(uploaded)
    ? fs.readdirSync(uploaded).filter((f) => f.startsWith('.upload-'))
    : [];
  assert.deepStrictEqual(leftovers, [], 'a refused upload must leave no temp file');
  assert.ok(!fs.existsSync(path.join(uploaded, 'zu-gross.hydra')),
    'a refused upload must not reach its final name');
});

test('the same upload succeeds once the cap allows it (negative control)', async (t) => {
  /* If 413 were returned by an over-eager check, this is the case that
   * catches it: under a generous cap the identical body must be stored. */
  const base = await bootServer(t, { HYDRA_MAX_UPLOAD: '16MiB' });
  const list = await (await fetch(`${base}/api/models`)).json();
  const starter = fs.readFileSync(path.join(ROOT, list.models[0].path));

  const res = await fetch(`${base}/api/models/upload?name=passt.hydra`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: starter,
  });
  const json = await res.json();
  assert.strictEqual(res.status, 200, `expected 200, got ${res.status}: ${JSON.stringify(json)}`);
  assert.strictEqual(json.ok, true);
  t.after(() => fs.rmSync(path.join(ROOT, 'models', 'uploaded', 'passt.hydra'), { force: true }));
});

test('an over-sized upload is refused even without a Content-Length header', async (t) => {
  /* The streaming guard is a different code path from the Content-Length
   * pre-check, and it is the one that used to kill the socket. */
  const base = await bootServer(t, { HYDRA_MAX_UPLOAD: '1MiB' });
  const http = require('node:http');
  const port = Number(new URL(base).port);

  const result = await new Promise((resolve) => {
    const req = http.request({
      host: '127.0.0.1',
      port,
      path: '/api/models/upload?name=chunked.hydra',
      method: 'POST',
      headers: { 'Transfer-Encoding': 'chunked', 'Content-Type': 'application/octet-stream' },
    }, (res) => {
      let body = '';
      res.on('data', (c) => { body += c; });
      res.on('end', () => resolve({ status: res.statusCode, body }));
    });
    req.on('error', (e) => resolve({ status: 0, body: `socket error: ${e.message}` }));
    const chunk = Buffer.alloc(256 * 1024, 0x41);
    let sent = 0;
    const pump = () => {
      while (sent < 3 * 1024 * 1024) {
        sent += chunk.length;
        if (!req.write(chunk)) {
          req.once('drain', pump);
          return;
        }
      }
      req.end();
    };
    pump();
  });

  assert.strictEqual(result.status, 413,
    `a chunked over-sized upload must be refused with 413, got ${result.status}: ${result.body}`);
  assert.match(result.body, /larger than the 1\.0 MiB limit/,
    'the streaming refusal must carry the same clear message');
});

test('a GGUF upload is refused with a GGUF-specific message', async () => {
  const gguf = Buffer.concat([
    Buffer.from('GGUF'), Buffer.from([3, 0, 0, 0]), Buffer.alloc(60),
  ]);
  const up = await fetch(`${BASE}/api/models/upload?name=mein-llama.hydra`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: gguf,
  });
  const json = await up.json();
  assert.strictEqual(up.status, 400);
  assert.match(json.error, /GGUF/,
    `the error must name the format, got: ${json.error}`);
  assert.match(json.error, /\.hydra/, 'the error must say what IS expected');
  assert.doesNotMatch(json.error, /wrong magic\)$/,
    'the generic wrong-magic message must be gone');
  createdFiles.push(path.join(ROOT, 'models', 'uploaded', 'mein-llama.hydra'));
});

test('/api/models/inspect returns the header and every violated rule', async () => {
  /* Valid model: values present, nothing violated. */
  const { body: list } = await json('/api/models');
  const starter = list.models.find((m) => m.valid) || list.models[0];
  const ok = await json(`/api/models/inspect?path=${encodeURIComponent(starter.path)}`);
  assert.strictEqual(ok.status, 200);
  assert.strictEqual(ok.body.valid, true, `starter model must be valid: ${JSON.stringify(ok.body.violations)}`);
  assert.deepStrictEqual(ok.body.violations, []);
  assert.strictEqual(ok.body.header.dim, starter.dim);
  assert.strictEqual(ok.body.header.layers, starter.layers);
  assert.strictEqual(ok.body.header.weightsLen, starter.weightsLen);
  assert.strictEqual(ok.body.header.magic, 0x48594452);
  assert.strictEqual(ok.body.detected.format, 'HYDRA');
  assert.ok(Array.isArray(ok.body.rules) && ok.body.rules.length >= 8,
    'every rule must be reported, not only the failed ones');
  for (const r of ok.body.rules) {
    assert.strictEqual(typeof r.id, 'string');
    assert.strictEqual(typeof r.ok, 'boolean');
    assert.ok(r.message && typeof r.message === 'string', `rule ${r.id} needs a message`);
  }

  /* Broken model: the violated rules are named individually. */
  const rel = writeModelFile('kaputt-dim.hydra', { dim: 4096, layers: 4, weightsLen: 65536, pad: 65536 });
  const bad = await json(`/api/models/inspect?path=${encodeURIComponent(rel)}`);
  assert.strictEqual(bad.status, 200, 'inspect answers 200 even for a broken model');
  assert.strictEqual(bad.body.valid, false);
  assert.ok(bad.body.violations.includes('dim'),
    `dim must be reported as violated: ${JSON.stringify(bad.body.violations)}`);
  const dimRule = bad.body.rules.find((r) => r.id === 'dim');
  assert.strictEqual(dimRule.ok, false);
  assert.match(dimRule.message, /4096/, 'the message must name the offending value');
  assert.match(dimRule.message, /1\.\.64/, 'the message must name the allowed range');
  assert.strictEqual(bad.body.header.dim, 4096, 'the raw header value is still reported');

  /* Offset overflow: the 32-bit wrap, reported rather than followed. */
  const wrap = writeModelFile('kaputt-offset.hydra', {
    weightsOffset: 0xfffffff0, weightsLen: 0x20, pad: 64,
  });
  const wrapped = await json(`/api/models/inspect?path=${encodeURIComponent(wrap)}`);
  assert.ok(wrapped.body.violations.includes('weights_fit'),
    `a wrapped offset must be reported: ${JSON.stringify(wrapped.body.violations)}`);

  /* Shape ceiling: valid today, but stated, so nobody waits for 20 GB. */
  assert.ok(ok.body.limits.maxWeightsBytesForShape < 1024 * 1024,
    'the report must state how large a model the current format can describe');
  assert.strictEqual(ok.body.limits.maxWeightsBytesInHeader, 0xffffffff,
    'the report must state the 32-bit header limit');
});

test('/api/models/inspect names a GGUF and refuses paths outside the models', async () => {
  const rel = writeModelFile('llama.hydra', { magic: 0x46554747, version: 3 });
  const res = await json(`/api/models/inspect?path=${encodeURIComponent(rel)}`);
  assert.strictEqual(res.status, 200);
  assert.strictEqual(res.body.detected.format, 'GGUF');
  assert.match(res.body.detected.advice, /convert/i,
    'a GGUF must come with an actionable hint');
  assert.ok(res.body.violations.includes('magic'));

  /* Diagnostics must not become a file-read oracle for the host. */
  const escape = await json('/api/models/inspect?path=' + encodeURIComponent('../../etc/hostname'));
  assert.strictEqual(escape.status, 400, 'a path outside the models must be refused');
  const missing = await json('/api/models/inspect?path=models/uploadend/fehlt.hydra');
  assert.strictEqual(missing.status, 404, 'a missing model must be a 404');
  const noParam = await json('/api/models/inspect');
  assert.strictEqual(noParam.status, 400, 'path is required');
});

test('the upload refusal and the inspect report state the same reason', async () => {
  /* Two code paths reporting on the same file must not drift apart: the
   * user is told one thing on upload and another in the console. */
  const rel = writeModelFile('abweichend.hydra', {
    layers: 99999, weightsLen: 65536, pad: 65536,
  });
  const bytes = fs.readFileSync(path.join(ROOT, rel));

  const up = await fetch(`${BASE}/api/models/upload?name=abweichend-upload.hydra`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: bytes,
  });
  const upJson = await up.json();
  createdFiles.push(path.join(ROOT, 'models', 'uploaded', 'abweichend-upload.hydra'));

  const report = await json(`/api/models/inspect?path=${encodeURIComponent(rel)}`);
  const firstFailed = report.body.rules.find((r) => !r.ok);
  assert.ok(firstFailed, 'the crafted file must violate a rule');
  assert.strictEqual(upJson.error, firstFailed.message,
    'upload and inspect must quote the same first violation');
});

/* ------------------------------------------------------------------ */
/* GGUF import: recognise it, convert it, and prove the result is real  */
/* ------------------------------------------------------------------ */

/** Writes a syntactically valid GGUF with two dense F32 tensors. */
function writeGguf(name, { rows = 64, cols = 32, tokens = ['a', 'b', '▁c', 'd'] } = {}) {
  const dir = path.join(ROOT, 'models', 'converted');
  return writeGgufFile(dir, name, { rows, cols, tokens });
}

test('a GGUF upload is converted into a loadable .hydra with a full report', async () => {
  const src = writeGguf('server-fixture.gguf');
  createdFiles.push(src);
  const bytes = fs.readFileSync(src);

  const res = await fetch(`${BASE}/api/models/import?name=server-fixture.gguf`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: bytes,
  });
  const json = await res.json();
  assert.strictEqual(res.status, 200, `expected 200, got ${res.status}: ${JSON.stringify(json)}`);
  assert.strictEqual(json.ok, true);
  assert.strictEqual(json.model.path, 'models/converted/server-fixture.hydra');
  assert.strictEqual(json.model.valid, true, 'the converted model must pass the header rules');
  assert.strictEqual(json.model.dim, 64);
  assert.strictEqual(json.model.converted, true, 'it must be marked as converted, not trained');
  createdFiles.push(path.join(ROOT, json.model.path));

  /* The conversion report is the answer, not a log line: without the
   * tensors and the scale, the user cannot tell what they just got. */
  const c = json.conversion;
  assert.strictEqual(c.ok, true);
  assert.strictEqual(c.gguf.magic, 'GGUF');
  assert.strictEqual(c.gguf.architecture, 'llama');
  assert.strictEqual(c.planes.length, 2, 'two planes: A and B');
  assert.ok(c.planes[0].gamma > 0, 'the absmean gamma must be reported');
  assert.strictEqual(c.aggregation.exact, true);
  assert.strictEqual(c.planes[0].rowsRead, 64);

  /* And the converted model has to actually run - a file the engine
   * refuses is worse than a refused import. */
  const run = await postJSON('/api/infer', { path: json.model.path, token: 1, steps: 8 });
  assert.strictEqual(run.status, 200, `the converted model must run: ${JSON.stringify(run.body)}`);
  assert.strictEqual(run.body.tokens.length, 8);
  assert.ok(run.body.tokens.every((t) => t >= 0 && t < run.body.vocab),
    'every token must be inside the vocabulary');

  /* The vocabulary sidecar is what makes the chat usable with the model. */
  const vocab = JSON.parse(fs.readFileSync(
    path.join(ROOT, 'models', 'vocab', 'server-fixture.json'), 'utf8'));
  assert.strictEqual(vocab.c, 2, 'the U+2581 space mark must be stripped from "▁c"');
  assert.strictEqual(Object.keys(vocab).length, 4);
  createdFiles.push(path.join(ROOT, 'models', 'vocab', 'server-fixture.json'));
});

test('the import refuses a file that is not a GGUF and leaves nothing behind', async () => {
  const junk = Buffer.alloc(512, 0x41);
  const res = await fetch(`${BASE}/api/models/import?name=not-a-model.gguf`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: junk,
  });
  const json = await res.json();
  assert.strictEqual(res.status, 400);
  assert.match(json.error, /not a GGUF/i,
    `the message must say what the file is not, got: ${json.error}`);
  assert.ok(!fs.existsSync(path.join(ROOT, 'models', 'converted', 'not-a-model.hydra')),
    'a refused import must not leave a model behind');
  createdFiles.push(path.join(ROOT, 'models', 'converted', 'not-a-model.hydra'));
});

test('the import route only accepts .gguf and points .hydra at the other one', async () => {
  const wrong = await fetch(`${BASE}/api/models/import?name=model.hydra`, { method: 'POST' });
  assert.strictEqual(wrong.status, 400);
  assert.match((await wrong.json()).error, /api\/models\/upload/,
    'the refusal must name the route that does take a .hydra');
});

test('a converted model shows up in the chat model list as converted', async () => {
  await json('/api/models');
  const src = writeGguf('listed.gguf');
  createdFiles.push(src);
  const up = await fetch(`${BASE}/api/models/import?name=listed.gguf`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: fs.readFileSync(src),
  });
  assert.strictEqual(up.status, 200);
  createdFiles.push(path.join(ROOT, 'models', 'converted', 'listed.hydra'));
  createdFiles.push(path.join(ROOT, 'models', 'vocab', 'listed.json'));

  const after = await json('/api/models');
  const entry = after.body.models.find((m) => m.path === 'models/converted/listed.hydra');
  assert.ok(entry, 'the converted model must be listed');
  assert.strictEqual(entry.converted, true);
  assert.strictEqual(entry.trained, false, 'it must not claim to be trained');
  assert.strictEqual(entry.uploaded, false, 'it must not claim to be uploaded');
});

test('/api/infer returns the real state, not a token-only stand-in', async () => {
  /* The console renders the whole state vector as a strip. That is only
   * honest if the engine ran the FULL step: the token-only fast path leaves
   * state[1..dim-1] stale by design (include/hydra_model.h), so a strip fed
   * from it would show old numbers as if they were current. This test is the
   * guard against someone switching the server to --fast for speed. */
  const { body: list } = await json('/api/models');
  const model = list.models.find((m) => m.valid) || list.models[0];
  const res = await postJSON('/api/infer', { path: model.path, token: 1, steps: 8 });

  assert.strictEqual(res.status, 200);
  assert.ok(Array.isArray(res.body.state), 'the response must carry a state vector');
  assert.strictEqual(res.body.state.length, res.body.dim,
    'the state vector must have one entry per dimension');
  assert.strictEqual(res.body.tokens.length, 8);

  /* A state that never leaves the seed value would satisfy the length check
   * above while being just as wrong. */
  assert.ok(res.body.state.some((v) => v !== 0),
    'the full step must actually move the state');

  const server = fs.readFileSync(path.join(ROOT, 'server.js'), 'utf8');
  assert.ok(!/runInference\([^)]*--fast/.test(server),
    'the console must not run inference in token-only mode');
});

test('the console binds to loopback unless HYDRA_ALLOW_REMOTE=1', async (t) => {
  /* Reading the bind address is the only honest way to test this.
   *
   * The obvious test - connect to a non-loopback local address and expect
   * ECONNREFUSED - measured the sandbox, not the server. Dev hosts mirror
   * every port the console listens on onto the workspace address with
   * socat, so such a probe can succeed while the server itself is still
   * bound to 127.0.0.1 (see errors.md, "a reachability probe is not a
   * bind-address test"). The socket has to be identified by the pid that
   * owns it, not by its port number.
   *
   * Both directions are checked, because a one-sided assertion cannot be
   * falsified: remove the gate and the first case fails, invert it and the
   * second does. */
  const boot = async (port, allowRemote) => {
    const srv = spawn(process.execPath, ['server.js'], {
      cwd: ROOT,
      env: { ...process.env, PORT: String(port), HYDRA_ALLOW_REMOTE: allowRemote },
      stdio: ['ignore', 'ignore', 'ignore'],
    });
    t.after(() => srv.kill('SIGKILL'));
    const deadline = Date.now() + 15000;
    for (;;) {
      try {
        const res = await fetch(`http://127.0.0.1:${port}/api/models`);
        if (res.ok) return srv;
      } catch {
        /* not up yet */
      }
      if (Date.now() > deadline) throw new Error(`server on ${port} did not start`);
      await new Promise((r) => setTimeout(r, 150));
    }
  };

  const closed = await boot(PORT + 1, '');
  const open = await boot(PORT + 2, '1');

  /* Read the kernel's view AFTER both are up: a snapshot taken earlier
   * would only prove the sockets did not exist yet. */
  const rows = execFileSync('ss', ['-Hltnp'], { encoding: 'utf8' })
    .split('\n')
    .map((l) => l.trim().split(/\s+/))
    .filter((f) => f[0] === 'LISTEN');

  /* The addresses this process bound, and nothing a mirror added later.
   * The owner is matched anywhere in the row: the column count of `ss -p`
   * output is not something to hard-code. */
  const boundBy = (port, pid) => rows
    .filter((f) => f[3] && f[3].endsWith(':' + port))
    .filter((f) => f.some((part) => part.includes(`pid=${pid},`)))
    .map((f) => f[3].slice(0, f[3].length - String(port).length - 1));

  const loopback = boundBy(PORT + 1, closed.pid);
  assert.ok(loopback.length > 0, 'the default server must be listening');
  assert.deepStrictEqual(loopback, ['127.0.0.1'],
    'by default the console must only listen on loopback');

  const publicBind = boundBy(PORT + 2, open.pid);
  assert.ok(publicBind.length > 0, 'the opt-in server must be listening');
  assert.ok(publicBind.includes('0.0.0.0') || publicBind.includes('*'),
    `HYDRA_ALLOW_REMOTE=1 must bind every interface, got ${publicBind.join(',')}`);
});