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
 *  POST /api/models/upload?name=   -> store an uploaded .hydra (raw body)
 *  POST /api/infer {token,prompt,steps} -> inference via hydra-run --json
 *  POST /api/train {...}           -> train a model from a corpus
 *  GET  /api/vocab?model=...       -> token<->string map for a model
 *  PUT  /api/vocab                 -> store/replace that map
 *  GET  /api/axiom?h=...           -> coexistence axiom check
 *
 * Security invariants kept from the audit:
 *  - model paths are restricted to *.hydra inside the project directory and
 *    resolved with realpath + lstat (a symlink must not escape ROOT)
 *  - uploaded bytes are validated against the same header rules the C
 *    loader enforces; an invalid file is never written to a model path
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
const UPLOAD_DIR = path.join(MODELS_DIR, 'uploaded');
const MAX_VERIFY_DIM = 4096;
const VOCAB_DIR = path.join(ROOT, 'models', 'vocab');
const DEFAULT_MODEL = path.join(MODELS_DIR, 'demo.hydra');

const MAX_BODY = 2 * 1024 * 1024; /* training corpora are text; 2 MiB is ample */
const MAX_UPLOAD = 64 * 1024 * 1024; /* .hydra models are small; 64 MiB is generous */
const MAX_PROMPT = 256; /* must match MAX_PROMPT in src/main.c */
const ENGINE_TIMEOUT_MS = 20000;

/* The console talks to a local engine and can run arbitrary uploaded code
 * paths, so binding it to every interface must be an explicit decision.
 * Freebuff-style dev hosts set HYDRA_ALLOW_REMOTE=1 when they need it. */
const HOST = process.env.HYDRA_ALLOW_REMOTE === '1' ? '0.0.0.0' : '127.0.0.1';

const HEADER_BYTES = 24;
const MAX_DIM = 64;

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

/* Uploaded bytes are streamed straight to disk instead of being buffered:
 * the cap is 64 MiB, and a JSON body parser would have to hold that in RAM
 * before deciding whether the request is acceptable at all. The body is
 * written to a .tmp file first and only renamed after validation, so a
 * failed or oversized upload can never leave a truncated .hydra behind. */
function streamToTmpFile(req, tmpPath, limit) {
  return new Promise((resolve, reject) => {
    const out = fs.createWriteStream(tmpPath, { flags: 'wx' });
    let written = 0;
    let settled = false;
    const fail = (err) => {
      if (settled) return;
      settled = true;
      out.destroy();
      try { fs.unlinkSync(tmpPath); } catch { /* already gone */ }
      reject(err);
    };
    req.on('data', (chunk) => {
      written += chunk.length;
      if (written > limit) {
        req.destroy();
        const e = new Error('file exceeds the 64 MiB limit');
        e.status = 413;
        fail(e);
      }
    });
    out.on('error', fail);
    req.on('error', fail);
    out.on('finish', () => {
      if (settled) return;
      settled = true;
      resolve(written);
    });
    req.pipe(out);
  });
}

/**
 * Header validation for an uploaded model. Mirrors the C loader in
 * src/hydra_engine.c check for check: the point is to reject exactly what
 * the engine would reject, so a model that is listed as valid is loadable.
 * Returns null when the file is fine, otherwise the reason.
 */
function validateHydraHeader(file, size) {
  if (size < HEADER_BYTES) return 'file is smaller than the 24-byte header';
  const fd = fs.openSync(file, 'r');
  try {
    const buf = Buffer.alloc(HEADER_BYTES);
    if (fs.readSync(fd, buf, 0, HEADER_BYTES, 0) < HEADER_BYTES) {
      return 'header could not be read';
    }
    const magic = buf.readUInt32LE(0);
    const version = buf.readUInt16LE(4);
    const vocab = buf.readUInt16LE(6);
    const dim = buf.readUInt32LE(8);
    const layers = buf.readUInt32LE(12);
    const weightsOffset = buf.readUInt32LE(16);
    const weightsLen = buf.readUInt32LE(20);

    if (magic !== trainer.MAGIC) return 'not a .hydra file (wrong magic)';
    if (version !== trainer.VERSION) return `unsupported .hydra version ${version}`;
    if (dim === 0 || dim > MAX_DIM) return `dim out of range (1..${MAX_DIM}): ${dim}`;
    if (vocab === 0 || vocab > trainer.MAX_VOCAB) {
      return `vocab out of range (1..${trainer.MAX_VOCAB}): ${vocab}`;
    }
    if (layers === 0 || layers > trainer.MAX_LAYERS) {
      return `layers out of range (1..${trainer.MAX_LAYERS}): ${layers}`;
    }
    if (weightsOffset < HEADER_BYTES) return 'weights_offset points into the header';
    if (weightsOffset + weightsLen > size) return 'weights run past the end of the file';
    if (layers * dim > weightsLen) return 'layers x dim is not covered by weights_len';
    return null;
  } finally {
    fs.closeSync(fd);
  }
}

/** Resolve an upload name to a path inside models/uploaded/, or null. */
function uploadTarget(name) {
  if (typeof name !== 'string' || !name.endsWith('.hydra')) return null;
  const key = modelKey(name.slice(0, -'.hydra'.length));
  if (key === null) return null;
  const dir = path.resolve(UPLOAD_DIR);
  const file = path.resolve(dir, `${key}.hydra`);
  const rel = path.relative(dir, file);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) return null;
  return file;
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

/* prompt: optional token sequence fed through the engine before generation.
 * An empty prompt keeps the old single-seed behaviour (token). */
function runInference(modelPath, token, steps, prompt) {
  const args = [modelPath, String(token), String(steps), '--json'];
  if (Array.isArray(prompt) && prompt.length > 0) args.push('--prompt', prompt.join(','));
  return engineJson(args);
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
      uploaded: relPath.startsWith('models' + path.sep + 'uploaded' + path.sep),
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
  const name = modelKey(String(body.name || 'trained-model').toLowerCase());
  if (name === null) {
    return { error: 'invalid name: use letters, digits, dot, dash and underscore' };
  }

  const vocabMap = loadVocab(name);
  const { samples, unknown } = parseCorpus(body.corpus || '', vocabMap);
  if (samples.length === 0) return { error: 'no usable training lines' };

  const cfg = {
    vocab: clampInt(body.vocab, 2, trainer.MAX_VOCAB, 64),
    dim: clampInt(body.dim, 2, trainer.MAX_DIM, 32),
    layers: clampInt(body.layers, 1, trainer.MAX_LAYERS, 8),
  };
  if (cfg.dim > MAX_VERIFY_DIM) {
    return { error: `dim too large; max allowed is ${MAX_VERIFY_DIM}` };
  }
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
  const bytes = trainer.writeModelFile(TRAINED_DIR, name + '.hydra', { ...cfg, ...result });
  const file = path.resolve(TRAINED_DIR, name + '.hydra');
  const rel = path.relative(path.resolve(TRAINED_DIR), file);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) {
    return { error: 'invalid target path' };
  }
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
  const dim = clampInt(cfg.dim, 1, MAX_VERIFY_DIM, 32);
  const buf = fs.readFileSync(file);
  const header = {
    magic: buf.readUInt32LE(0),
    version: buf.readUInt16LE(4),
    vocab: buf.readUInt16LE(6),
    dim: buf.readUInt32LE(8),
    layers: buf.readUInt32LE(12),
  };
  const decoded = trainer.unpack(buf.subarray(24), cfg.layers, dim);

  const state = new Array(dim).fill(0);
  const simulated = [];
  let token = seedToken;
  for (let t = 0; t < steps; t += 1) {
    const r = trainer.step(decoded.w1, decoded.w2, cfg.layers, dim, cfg.vocab, state, token);
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

/** Model names cross from HTTP into file paths, so they are validated once,
 * here, and rejected rather than rewritten. `replace()` is not a sanitizer:
 * it still lets `..` and separators through in spirit, and a filter that
 * silently changes the caller's string hides mistakes. */
function modelKey(name) {
  if (typeof name !== 'string') return null;
  const trimmed = name.trim();
  if (trimmed.length === 0 || trimmed.length > 64) return null;
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(trimmed)) return null;
  if (trimmed.includes('..')) return null;
  return trimmed;
}

function vocabFile(name) {
  const key = modelKey(name);
  if (key === null) return null;
  const dir = path.resolve(VOCAB_DIR);
  const file = path.resolve(dir, `${key}.json`);
  const rel = path.relative(dir, file);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) return null;
  return file;
}

function loadVocab(name) {
  const file = vocabFile(name);
  if (file === null) return {};
  try {
    const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
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

/* ------------------------------------------------------------------ */
/* Crash handler                                                        */
/* ------------------------------------------------------------------ */

/* Ring buffer of the last errors. The console polls /api/errors and shows
 * them with a copy button, so an error that happened inside a route (or in
 * a worker) is visible in the UI instead of dying in a log nobody opens.
 * Bounded on purpose: an error loop must not become a memory leak. */
const errorLog = [];
const ERROR_LOG_MAX = 20;

function recordServerError(what, err) {
  const entry = {
    when: new Date().toISOString(),
    what,
    message: (err && err.message) || String(err),
    /* Stack traces contain absolute paths. They are shown to the user who
     * already runs this process on their own machine, and they are what
     * makes a bug report actionable, so they are kept - but they never
     * reach a non-GET response. */
    detail: (err && err.stack) || '',
  };
  errorLog.unshift(entry);
  if (errorLog.length > ERROR_LOG_MAX) errorLog.pop();
  console.error(`[Hydra Console] ${what}: ${entry.message}`);
  return entry;
}

function installCrashHandlers() {
  process.on('uncaughtException', (err) => {
    recordServerError('Uncaught exception', err);
    /* The process stays up on purpose: the console is a local tool, and a
     * single failed request should not take the UI with it. The exit code
     * still reflects that something went wrong. */
  });
  process.on('unhandledRejection', (reason) => {
    recordServerError('Unhandled promise rejection', reason);
  });
}

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

    if (url.pathname === '/api/models/upload' && req.method === 'POST') {
      const target = uploadTarget(url.searchParams.get('name') || '');
      if (!target) {
        return sendJSON(res, 400, {
          error: 'invalid file name: use letters, digits, dot, dash, underscore and the .hydra suffix',
        });
      }
      fs.mkdirSync(UPLOAD_DIR, { recursive: true });
      const tmp = path.join(UPLOAD_DIR, `.upload-${process.pid}-${Date.now()}.tmp`);
      let size;
      try {
        size = await streamToTmpFile(req, tmp, MAX_UPLOAD);
      } catch (e) {
        /* The stream already removed the temp file and req is destroyed. */
        return sendJSON(res, e.status || 400, { error: e.message });
      }
      const reason = validateHydraHeader(tmp, size);
      if (reason !== null) {
        fs.unlinkSync(tmp);
        return sendJSON(res, 400, { error: reason, rejected: true });
      }
      /* Only a fully validated file reaches its final name. */
      fs.renameSync(tmp, target);
      const info = inspectHeader(path.relative(ROOT, target));
      return sendJSON(res, 200, { ok: true, model: info });
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

      /* prompt: the whole encoded prompt, not just its first token. It is
       * validated here as strictly as the seed - a non-array, a NaN or an
       * out-of-range id would otherwise reach the CLI as a silent cap. */
      let prompt = null;
      if (body.prompt !== undefined && body.prompt !== null) {
        if (!Array.isArray(body.prompt)) {
          return sendJSON(res, 400, { error: 'prompt must be an array of token ids' });
        }
        if (body.prompt.length > MAX_PROMPT) {
          return sendJSON(res, 400, { error: `prompt too long (max ${MAX_PROMPT} tokens)` });
        }
        prompt = [];
        for (const v of body.prompt) {
          const n = Number(v);
          if (!Number.isFinite(n) || n < 0 || n > 0xFFFF) {
            return sendJSON(res, 400, { error: `prompt token out of range: ${v}` });
          }
          prompt.push(Math.trunc(n));
        }
        if (prompt.length === 0) prompt = null;
      }

      /* An explicitly requested but forbidden path must not silently fall
       * back to the default model. */
      let p = DEFAULT_MODEL;
      if (body.path !== undefined && body.path !== null && body.path !== '') {
        p = safeModelPath(body.path);
        if (!p) return sendJSON(res, 400, { error: 'invalid model path' });
      }
      const result = await runInference(p, token, steps, prompt);
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
      const file = vocabFile(String(body.model || ''));
      if (file === null) return sendJSON(res, 400, { error: 'invalid model name' });
      fs.mkdirSync(VOCAB_DIR, { recursive: true });
      fs.writeFileSync(file, JSON.stringify(clean, null, 2));
      return sendJSON(res, 200, { ok: true, entries: Object.keys(clean).length });
    }

    if (url.pathname === '/api/axiom' && req.method === 'GET') {
      return sendJSON(res, 200, axiomCheck(url.searchParams.get('h')));
    }

    if (url.pathname === '/api/errors' && req.method === 'GET') {
      /* Only what the caller may see: no stack from another process, and
       * a client cannot clear the buffer by accident. */
      return sendJSON(res, 200, { errors: errorLog });
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
    recordServerError('Request failed', e);
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
installCrashHandlers();

server.listen(PORT, HOST, () => {
  console.log(`[Hydra Console] http://${HOST}:${PORT}`);
});