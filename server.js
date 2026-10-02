#!/usr/bin/env node
/* Hydra-Stone Console Server
 *
 * Dependency-free HTTP server that
 *  - serves the web console from public/
 *  - exposes the C engine (hydra-run --json) as a REST API
 *  - LISTS and inspects real .hydra models
 *  - TRAINS a new model from a user corpus and stores it as a real file
 *  - stores a token<->string vocabulary per model so the chat shows words
 *
 * Endpoints:
 *  GET  /api/models                -> every model found on disk, with header
 *  GET  /api/model?path=...        -> detail for one model
 *  POST /api/infer {token,steps}   -> inference via hydra-run --json
 *  POST /api/train {...}           -> train a model from a corpus
 *  GET  /api/vocab?model=...       -> token<->string map for a model
 *  PUT  /api/vocab                 -> store/replace that map
 *  GET  /api/axiom?h=...           -> coexistence axiom check
 *
 * Security invariants kept from the audit:
 *  - model paths are restricted to *.hydra inside the project directory and
 *    resolved with realpath + lstat (a symlink must not escape ROOT)
 *  - no shell: execFile only, arguments are passed as an array
 *  - internal error details never reach the client
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFile, execFileSync } = require('child_process');
const trainer = require('./tools/hydra_train');

const PORT = parseInt(process.env.PORT || '8787', 10);
const ROOT = __dirname;
const PUBLIC = path.join(ROOT, 'public');
const HYDRA_RUN = path.join(ROOT, 'hydra-run');
const MODELS_DIR = path.join(ROOT, 'models');
const TRAINED_DIR = path.join(MODELS_DIR, 'trained');
const VOCAB_DIR = path.join(ROOT, 'models', 'vocab');
const DEFAULT_MODEL = path.join(MODELS_DIR, 'demo.hydra');

const MAX_BODY = 2 * 1024 * 1024; /* training corpora are text; 2 MiB is ample */
const ENGINE_TIMEOUT_MS = 20000;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

function send(res, code, body, type) {
  res.writeHead(code, {
    'Content-Type': type || 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

function sendJSON(res, code, obj) {
  send(res, code, JSON.stringify(obj));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let data = '';
    req.on('data', (c) => {
      data += c;
      if (data.length > MAX_BODY) {
        reject(new Error('body too large'));
        req.destroy();
      }
    });
    req.on('end', () => resolve(data));
    req.on('error', reject);
  });
}

/* Security: only *.hydra files inside ROOT are allowed.
 *
 * path.resolve() is purely *lexical* and does not follow symlinks, so the
 * prefix check alone only inspected the name. A symlink models/x.hydra ->
 * /etc/hostname used to pass and the engine opened the target (verified: the
 * error message revealed the target's size). The path is therefore resolved
 * with realpath and checked again, and it must be a regular file. */
function safeModelPath(p) {
  if (typeof p !== 'string' || !p.endsWith('.hydra')) return null;
  if (p.includes('\0')) return null;
  const abs = path.resolve(ROOT, p);
  if (!abs.startsWith(ROOT + path.sep)) return null;

  let real;
  try {
    real = fs.realpathSync(abs);
  } catch {
    return null;
  }
  if (real !== ROOT && !real.startsWith(ROOT + path.sep)) return null;
  let st;
  try {
    st = fs.lstatSync(real);
  } catch {
    return null;
  }
  if (!st.isFile()) return null;
  return real;
}

function engineJson(args) {
  return new Promise((resolve, reject) => {
    execFile(
      HYDRA_RUN,
      args,
      { timeout: ENGINE_TIMEOUT_MS, maxBuffer: 4 * 1024 * 1024 },
      (err, stdout, stderr) => {
        if (err) {
          /* stderr goes to the server log, never into the HTTP response. */
          const e = new Error('engine failed');
          e.detail = stderr || String(err.message);
          return reject(e);
        }
        try {
          resolve(JSON.parse(stdout));
        } catch (parseErr) {
          reject(new Error('engine output not JSON: ' + parseErr.message));
        }
      },
    );
  });
}

function runInference(modelPath, token, steps) {
  return engineJson([modelPath, String(token), String(steps), '--json']);
}

function loadModelInfo(modelPath) {
  return engineJson([modelPath, '0', '1', '--json']).then((j) => ({
    dim: j.dim,
    vocab: j.vocab,
    layers: j.layers,
    path: path.relative(ROOT, modelPath),
  }));
}

/* ------------------------------------------------------------------ */
/* Model discovery                                                     */
/* ------------------------------------------------------------------ */

function listHydraFiles(dir, prefix) {
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return [];
  }
  const out = [];
  for (const e of entries) {
    if (e.isFile() && e.name.endsWith('.hydra')) {
      out.push(path.join(prefix, e.name));
    } else if (e.isDirectory() && e.name !== 'vocab' && !e.name.startsWith('.')) {
      out.push(...listHydraFiles(path.join(dir, e.name), path.join(prefix, e.name)));
    }
  }
  return out;
}

/** Read the header directly - no engine start, so listing stays cheap. */
function inspectHeader(relPath) {
  const abs = safeModelPath(relPath);
  if (!abs) return null;
  let fd;
  try {
    fd = fs.openSync(abs, 'r');
    const buf = Buffer.alloc(24);
    const n = fs.readSync(fd, buf, 0, 24, 0);
    if (n < 24) return null;
    const st = fs.fstatSync(fd);
    return {
      path: relPath,
      bytes: st.size,
      magic: buf.readUInt32LE(0),
      version: buf.readUInt16LE(4),
      vocab: buf.readUInt16LE(6),
      dim: buf.readUInt32LE(8),
      layers: buf.readUInt32LE(12),
      weightsOffset: buf.readUInt32LE(16),
      weightsLen: buf.readUInt32LE(20),
      valid: buf.readUInt32LE(0) === trainer.MAGIC,
      trained: relPath.startsWith('models' + path.sep + 'trained' + path.sep),
    };
  } catch {
    return null;
  } finally {
    if (fd !== undefined) fs.closeSync(fd);
  }
}

function listModels() {
  return listHydraFiles(MODELS_DIR, 'models').map(inspectHeader).filter(Boolean);
}

/* ------------------------------------------------------------------ */
/* Training                                                            */
/* ------------------------------------------------------------------ */

/**
 * Parse a corpus into samples. Accepted line forms:
 *   "1 2 3 -> 2 3 4"   explicit input and target token ids
 *   "1 2 3"            input only, target is the next-token shift
 * Words are resolved through the vocabulary when one exists for the model,
 * so the console can train from text the user actually typed.
 */
function parseCorpus(text, vocabMap) {
  const samples = [];
  const unknown = new Set();
  const lines = String(text).split(/\r?\n/);
  for (const raw of lines) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const [left, right] = line.split('->');
    const parseSide = (part) =>
      part
        .trim()
        .split(/\s+/)
        .filter(Boolean)
        .map((tok) => {
          if (/^\d+$/.test(tok)) return Number(tok);
          const id = vocabMap ? vocabMap[tok] : undefined;
          if (id === undefined) {
            unknown.add(tok);
            return null;
          }
          return id;
        });
    const input = parseSide(left);
    const target = right !== undefined ? parseSide(right) : null;
    if (input.length === 0) continue;
    if (input.some((v) => v === null)) continue;
    if (target && (target.length === 0 || target.some((v) => v === null))) continue;
    samples.push({
      input,
      target: target || input.slice(1).concat([(input[input.length - 1] + 1) % 1024]),
    });
  }
  return { samples, unknown: Array.from(unknown) };
}

async function handleTrain(body) {
  const name = String(body.name || 'trained-model')
    .toLowerCase()
    .replace(/[^a-z0-9._-]/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 48);
  if (!name) return { error: 'invalid name' };

  const vocabMap = loadVocab(name);
  const { samples, unknown } = parseCorpus(body.corpus || '', vocabMap);
  if (samples.length === 0) return { error: 'no usable training lines' };

  const cfg = {
    vocab: clampInt(body.vocab, 2, trainer.MAX_VOCAB, 64),
    dim: clampInt(body.dim, 2, trainer.MAX_DIM, 32),
    layers: clampInt(body.layers, 1, trainer.MAX_LAYERS, 8),
  };
  const epochs = clampInt(body.epochs, 1, 50, 12);

  /* Every token id used must fit into the vocabulary of the new model. */
  let maxToken = 0;
  for (const s of samples) {
    for (const t of s.input.concat(s.target)) maxToken = Math.max(maxToken, t);
  }
  if (maxToken >= cfg.vocab) {
    return { error: `token ${maxToken} does not fit vocab ${cfg.vocab}; raise vocab` };
  }

  const before = trainer.evaluate(
    ...(() => {
      const seed = trainer.seedWeights(cfg.layers, cfg.dim);
      return [seed.w1, seed.w2, cfg, samples];
    })(),
  );
  const result = trainer.train(cfg, samples, epochs);
  fs.mkdirSync(TRAINED_DIR, { recursive: true });
  const file = path.join(TRAINED_DIR, `${name}.hydra`);
  const bytes = trainer.writeModelFile(file, { ...cfg, ...result });
  const verification = await verifyModelFile(file, cfg, samples[0].input[0], 8);
  if (!verification.match) {
    return { error: 'verification failed: engine and trainer disagree', verification };
  }

  return {
    ok: true,
    model: path.relative(ROOT, file),
    bytes,
    config: cfg,
    epochs,
    flips: result.flips,
    samples: samples.length,
    before,
    after: result.history[Math.max(0, result.bestEpoch)],
    bestEpoch: result.bestEpoch,
    seedAccuracy: result.seedAccuracy,
    history: result.history,
    verification,
    unknownTokens: unknown,
  };
}

/**
 * Independent verification of a freshly written model: read the bytes back
 * from disk, decode them with the engine's own byte layout, simulate a run
 * and compare it with what the COMPILED C engine produces. If the two
 * disagree the file is wrong, and we refuse to report success.
 */
async function verifyModelFile(file, cfg, seedToken, steps) {
  const buf = fs.readFileSync(file);
  const header = {
    magic: buf.readUInt32LE(0),
    version: buf.readUInt16LE(4),
    vocab: buf.readUInt16LE(6),
    dim: buf.readUInt32LE(8),
    layers: buf.readUInt32LE(12),
  };
  const decoded = trainer.unpack(buf.subarray(24), cfg.layers, cfg.dim);

  const state = new Array(cfg.dim).fill(0);
  const simulated = [];
  let token = seedToken;
  for (let t = 0; t < steps; t += 1) {
    const r = trainer.step(decoded.w1, decoded.w2, cfg.layers, cfg.dim, cfg.vocab, state, token);
    simulated.push(r.out);
    token = r.out;
  }

  const engine = await engineJson([file, String(seedToken), String(steps), '--json']);
  return {
    header,
    simulated,
    engine: engine.tokens,
    match: JSON.stringify(simulated) === JSON.stringify(engine.tokens),
  };
}

function clampInt(value, min, max, fallback) {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.min(max, Math.trunc(n)));
}

/* ------------------------------------------------------------------ */
/* Vocabulary                                                          */
/* ------------------------------------------------------------------ */

function vocabFile(name) {
  const safe = String(name).replace(/[^A-Za-z0-9._-]/g, '-');
  return path.join(VOCAB_DIR, `${safe}.json`);
}

function loadVocab(name) {
  try {
    const raw = JSON.parse(fs.readFileSync(vocabFile(name), 'utf8'));
    /* Accept both shapes: the bare {word: id} map we write and an older
     * {map: {...}} wrapper. */
    if (raw && typeof raw.map === 'object' && raw.map !== null) return raw.map;
    if (raw && typeof raw === 'object') return raw;
    return {};
  } catch {
    return {};
  }
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

function axiomCheck(h) {
  const v = Number(h);
  if (!Number.isFinite(v)) return { error: 'h must be a number' };
  const safe = v <= 0 ? -1e9 : v; /* mirrors the engine: U = R * I{H=1} */
  return { humanity: v, allowed: v > 0, safe_score: v > 0 ? 99.5 * v : safe };
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);

  try {
    /* ---------- API ---------- */
    if (url.pathname === '/api/models' && req.method === 'GET') {
      return sendJSON(res, 200, { models: listModels(), default: path.relative(ROOT, DEFAULT_MODEL) });
    }

    if (url.pathname === '/api/model' && req.method === 'GET') {
      const reqPath = url.searchParams.get('path');
      let p = DEFAULT_MODEL;
      if (reqPath) {
        p = safeModelPath(reqPath);
        if (!p) return sendJSON(res, 400, { error: 'invalid model path' });
      }
      const info = await loadModelInfo(p);
      return sendJSON(res, 200, info);
    }

    if (url.pathname === '/api/infer' && req.method === 'POST') {
      /* Audit fix: JSON.parse('null') yields null and '[]' an array, so the
       * body shape is checked explicitly instead of producing a 500 with an
       * internal TypeError message. */
      let body;
      try {
        body = JSON.parse((await readBody(req)) || '{}');
      } catch {
        return sendJSON(res, 400, { error: 'invalid JSON body' });
      }
      if (body === null || typeof body !== 'object' || Array.isArray(body)) {
        return sendJSON(res, 400, { error: 'body must be a JSON object' });
      }

      const tokenRaw = body.token === undefined ? 0 : Number(body.token);
      const stepsRaw = body.steps === undefined ? 16 : Number(body.steps);
      if (!Number.isFinite(tokenRaw) || !Number.isFinite(stepsRaw)) {
        return sendJSON(res, 400, { error: 'token and steps must be finite numbers' });
      }
      const token = Math.max(0, Math.min(0xFFFF, Math.trunc(tokenRaw)));
      const steps = Math.max(1, Math.min(256, Math.trunc(stepsRaw)));

      /* An explicitly requested but forbidden path must not silently fall
       * back to the default model. */
      let p = DEFAULT_MODEL;
      if (body.path !== undefined && body.path !== null && body.path !== '') {
        p = safeModelPath(body.path);
        if (!p) return sendJSON(res, 400, { error: 'invalid model path' });
      }
      const result = await runInference(p, token, steps);
      return sendJSON(res, 200, result);
    }

    if (url.pathname === '/api/train' && req.method === 'POST') {
      let body;
      try {
        body = JSON.parse((await readBody(req)) || '{}');
      } catch {
        return sendJSON(res, 400, { error: 'invalid JSON body' });
      }
      if (body === null || typeof body !== 'object' || Array.isArray(body)) {
        return sendJSON(res, 400, { error: 'body must be a JSON object' });
      }
      if (typeof body.corpus !== 'string' || body.corpus.length === 0) {
        return sendJSON(res, 400, { error: 'corpus is required' });
      }
      const out = await handleTrain(body);
      if (out.error) return sendJSON(res, 400, out);
      return sendJSON(res, 200, out);
    }

    if (url.pathname === '/api/vocab' && req.method === 'GET') {
      return sendJSON(res, 200, { map: loadVocab(url.searchParams.get('model') || '') });
    }

    if (url.pathname === '/api/vocab' && req.method === 'PUT') {
      let body;
      try {
        body = JSON.parse((await readBody(req)) || '{}');
      } catch {
        return sendJSON(res, 400, { error: 'invalid JSON body' });
      }
      if (body === null || typeof body !== 'object' || typeof body.map !== 'object') {
        return sendJSON(res, 400, { error: 'map object is required' });
      }
      const clean = {};
      for (const [k, v] of Object.entries(body.map)) {
        const id = Number(v);
        if (Number.isInteger(id) && id >= 0 && id < trainer.MAX_VOCAB) clean[k] = id;
      }
      fs.mkdirSync(VOCAB_DIR, { recursive: true });
      fs.writeFileSync(vocabFile(body.model || ''), JSON.stringify(clean, null, 2));
      return sendJSON(res, 200, { ok: true, entries: Object.keys(clean).length });
    }

    if (url.pathname === '/api/axiom' && req.method === 'GET') {
      return sendJSON(res, 200, axiomCheck(url.searchParams.get('h')));
    }

    if (url.pathname.startsWith('/api/')) {
      return sendJSON(res, 404, { error: 'unknown endpoint' });
    }

    /* ---------- Static UI ---------- */
    let file = url.pathname === '/' ? '/index.html' : url.pathname;
    file = path.normalize(file).replace(/^(\.\.[/\\])+/, '');
    const abs = path.join(PUBLIC, file);
    if (!abs.startsWith(PUBLIC + path.sep) && abs !== PUBLIC) {
      return send(res, 403, 'forbidden', 'text/plain');
    }
    fs.readFile(abs, (err, data) => {
      if (err) return send(res, 404, 'not found', 'text/plain');
      const ext = path.extname(abs).toLowerCase();
      send(res, 200, data, MIME[ext] || 'application/octet-stream');
    });
  } catch (e) {
    /* Internal details (paths, engine stderr, stack traces) never reach the
     * client - they reveal file sizes and server structure. */
    console.error('[Hydra Console] request failed:', e && e.stack ? e.stack : e);
    sendJSON(res, 500, { error: 'internal error' });
  }
});

/**
 * The demo model is generated output, not a checked-in binary. A clean
 * checkout therefore has no models/ directory at all, so we create it from
 * the repository's own writer instead of shipping a stale blob.
 */
function ensureDefaultModel() {
  fs.mkdirSync(MODELS_DIR, { recursive: true });
  if (fs.existsSync(DEFAULT_MODEL)) return;
  try {
    execFileSync('python3', [
      path.join(ROOT, 'tools', 'make_dummy_model.py'),
      DEFAULT_MODEL,
    ]);
    console.log('[Hydra Console] generated the default demo model');
  } catch (e) {
    console.error('[Hydra Console] could not generate the demo model:', e.message);
  }
}

ensureDefaultModel();

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[Hydra Console] http://0.0.0.0:${PORT}`);
});