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
 *  GET  /api/models/inspect?path=  -> full header + EVERY rule the model
 *                                     violates, with a readable reason
 *                                     (same analysis the upload route uses)
 *  POST /api/models/upload?name=   -> store an uploaded .hydra (raw body).
 *                                     Over the cap: 413 with a body, never a
 *                                     dropped connection.
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
 *  - /api/models/inspect only accepts paths inside the project, exactly
 *    like every other model route: a diagnostics endpoint that could read
 *    arbitrary host files would be a disclosure hole, not a feature
 *  - no shell: execFile only, arguments are passed as an array
 *  - internal error details never reach the client
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFile, execFileSync } = require('child_process');
const { randomUUID } = require('node:crypto');
const trainer = require('./tools/hydra_train');

const PORT = parseInt(process.env.PORT || '8787', 10);
const ROOT = __dirname;
const PUBLIC = path.join(ROOT, 'public');
const HYDRA_RUN = path.join(ROOT, 'hydra-run');
const MODELS_DIR = path.join(ROOT, 'models');
const TRAINED_DIR = path.join(MODELS_DIR, 'trained');
const UPLOAD_DIR = path.join(MODELS_DIR, 'uploaded');
const CONVERTED_DIR = path.join(MODELS_DIR, 'converted');
const GGUF_CONVERTER = path.join(ROOT, 'tools', 'gguf_to_hydra.py');
/* Conversion is CPU work in a second process. The cap is a wall-clock
 * limit on the SUBPROCESS, not a client timeout: it stops a 4 GB F32 file
 * from parking a request forever, and it is deliberately much larger than
 * the 20 s an inference gets. Measured throughput is in
 * docs/GGUF-IMPORT.md; raise it if a bigger model has to go through. */
const CONVERT_TIMEOUT_MS = parseInt(process.env.HYDRA_CONVERT_TIMEOUT_MS || '600000', 10);
const MAX_VERIFY_DIM = 4096;
const VOCAB_DIR = path.join(ROOT, 'models', 'vocab');
const DEFAULT_MODEL = path.join(MODELS_DIR, 'starter.hydra');

const MAX_BODY = 2 * 1024 * 1024; /* training corpora are text; 2 MiB is ample */
/* Upload cap, overridable because the model size ceiling is a FORMAT limit,
 * not a transport one: a future .hydra v2 is allowed to be far bigger than
 * today's (measured: v1 can describe at most 256 KiB of weights). Bytes can
 * be set directly, sizes with a suffix: HYDRA_MAX_UPLOAD=2GB. */
const MAX_UPLOAD = parseByteSize(process.env.HYDRA_MAX_UPLOAD, 64 * 1024 * 1024);
const MAX_PROMPT = 256; /* must match MAX_PROMPT in src/main.c */
const ENGINE_TIMEOUT_MS = 20000;

/* The console talks to a local engine and can run arbitrary uploaded code
 * paths, so binding it to every interface must be an explicit decision.
 * Freebuff-style dev hosts set HYDRA_ALLOW_REMOTE=1 when they need it. */
const HOST = process.env.HYDRA_ALLOW_REMOTE === '1' ? '0.0.0.0' : '127.0.0.1';

const HEADER_BYTES = 24;
const MAX_DIM = 64;

/**
 * Accepts "1048576", "64MiB", "2GB", "1.5 GiB". Returns fallback for
 * anything unparseable, because a typo in an environment variable must not
 * silently remove the limit entirely.
 */
function parseByteSize(raw, fallback) {
  if (raw === undefined || raw === null) return fallback;
  const s = String(raw).trim().toLowerCase();
  if (s === '') return fallback;
  const m = /^(\d+(?:\.\d+)?)\s*(b|kb|kib|mb|mib|gb|gib|tb|tib)?$/.exec(s);
  if (!m) return fallback;
  const units = {
    b: 1, kb: 1000, kib: 1024, mb: 1000 ** 2, mib: 1024 ** 2,
    gb: 1000 ** 3, gib: 1024 ** 3, tb: 1000 ** 4, tib: 1024 ** 4,
  };
  const value = Number(m[1]) * units[m[2] || 'b'];
  if (!Number.isFinite(value) || value <= 0) return fallback;
  return Math.floor(value);
}

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
 * the cap is HYDRA_MAX_UPLOAD (64 MiB by default), and a JSON body parser
 * would have to hold that in RAM before deciding whether the request is
 * acceptable at all. The body is written to a .tmp file first and only
 * renamed after validation, so a failed or oversized upload can never
 * leave a truncated .hydra behind. */
class UploadTooLarge extends Error {
  constructor(limit) {
    super(`file is larger than the ${formatBytes(limit)} limit`);
    this.status = 413;
    this.tooLarge = true;
    this.limitBytes = limit;
  }
}

/** 15728640 -> "15.0 MB"; used so a size limit reads like a human wrote it. */
function formatBytes(n) {
  if (!Number.isFinite(n)) return String(n);
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let v = n;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i += 1;
  }
  return `${i === 0 ? v : v.toFixed(1)} ${units[i]}`;
}

/**
 * Rejects an over-sized body WITHOUT taking the socket away from the
 * response.
 *
 * The previous version called req.destroy() at the moment the limit was
 * passed, so the 413 the route wanted to send never reached the client:
 * curl and fetch both saw an empty reply and a connection error instead of
 * a number. The client can only learn the refusal from the response, so the
 * order here is: stop reading, answer, and only then close.
 */
function streamToTmpFile(req, tmpPath, limit) {
  return new Promise((resolve, reject) => {
    /* Content-Length first: a client that declares its size is refused
     * before a single byte is written, which is the clean path. */
    const declared = Number(req.headers['content-length']);
    if (Number.isFinite(declared) && declared > limit) {
      reject(new UploadTooLarge(limit));
      return;
    }

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
        /* Unpipe and pause - do NOT destroy. req.pause() keeps the socket
         * alive so the 413 can still be written; the route closes it once
         * the answer has been flushed. */
        req.unpipe(out);
        req.pause();
        fail(new UploadTooLarge(limit));
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
 * Magics worth naming.
 *
 * "wrong magic 0x46554747" is useless to somebody who just tried to load
 * their own model. Naming the format turns that into an action: a GGUF is
 * a llama.cpp/Ollama model, and this engine reads .hydra.
 */
const KNOWN_MAGICS = [
  {
    magic: 0x46554747, mask: 0xffffffff, name: 'GGUF',
    advice: 'this is a GGUF model (llama.cpp / Ollama). Import it with '
      + 'POST /api/models/import (or the "Import .gguf" button) and it is '
      + 'converted with tools/gguf_to_hydra.py; a .hydra file can be used directly',
  },
  { magic: 0x04034b50, mask: 0xffffffff, name: 'ZIP archive', advice: 'this looks like a .zip archive' },
  { magic: 0x7f454c46, mask: 0xffffffff, name: 'ELF binary', advice: 'this looks like a compiled binary' },
  { magic: 0x89504e47, mask: 0xffffffff, name: 'PNG image', advice: 'this looks like an image' },
  { magic: 0x1f8b, mask: 0xffff, name: 'gzip archive', advice: 'this looks gzip-compressed' },
  { magic: 0x425a68, mask: 0xffff, name: 'bzip2 archive', advice: 'this looks bzip2-compressed' },
];

function describeMagic(magic) {
  if (magic === trainer.MAGIC) {
    return { expected: true, name: 'HYDRA', advice: null };
  }
  const known = KNOWN_MAGICS.find((k) => (magic & k.mask) === k.magic);
  return known
    ? { expected: false, name: known.name, advice: known.advice }
    : { expected: false, name: null, advice: null };
}

/**
 * The complete header analysis, in one place.
 *
 * The upload route and GET /api/models/inspect both call this, so the error
 * message a user gets on upload and the report they get from inspect can
 * never disagree about why a file was refused. Rules mirror the C loader in
 * src/hydra_engine.c one for one: a file this accepts is a file the engine
 * can load.
 *
 * @returns a report object; never throws for a merely invalid model.
 */
function analyseHydraFile(absPath, sizeArg) {
  const stat = fs.statSync(absPath);
  const size = sizeArg === undefined ? stat.size : sizeArg;
  const report = {
    path: null,
    bytes: size,
    headerBytesRequired: HEADER_BYTES,
    header: null,
    detected: null,
    rules: [],
    violations: [],
    valid: false,
    /* What the current on-disk format can describe at all. Reported even
     * for a valid file, because "valid" and "can grow to 20 GB" are two
     * different questions and only one of them has a yes today. */
    limits: {
      maxDim: MAX_DIM,
      maxVocab: trainer.MAX_VOCAB,
      maxLayers: trainer.MAX_LAYERS,
      headerFields: 'uint32 (weights_offset, weights_len)',
      maxWeightsBytesInHeader: 0xffffffff,
      /* One byte per (layer, dim) pair, holding w1 in bits 0-1 and w2 in
       * bits 2-3: 4 used bits = 2 ternary weights = 2 bits per weight.
       * So a shape of dim x layers occupies dim * layers BYTES, with the
       * upper 4 bits of each byte reserved. */
      maxWeightsBytesForShape: MAX_DIM * trainer.MAX_LAYERS,
    },
  };
  const rule = (id, ok, message) => {
    report.rules.push({ id, ok, message });
    if (!ok) report.violations.push(id);
    return ok;
  };

  if (size < HEADER_BYTES) {
    rule('size', false,
      `file is ${formatBytes(size)}, smaller than the ${HEADER_BYTES}-byte header`);
    return report;
  }

  const fd = fs.openSync(absPath, 'r');
  let buf;
  try {
    buf = Buffer.alloc(HEADER_BYTES);
    if (fs.readSync(fd, buf, 0, HEADER_BYTES, 0) < HEADER_BYTES) {
      rule('header_readable', false, 'the 24-byte header could not be read');
      return report;
    }
  } finally {
    fs.closeSync(fd);
  }
  rule('header_readable', true, `the ${HEADER_BYTES}-byte header was read`);

  const header = {
    magic: buf.readUInt32LE(0),
    version: buf.readUInt16LE(4),
    vocab: buf.readUInt16LE(6),
    dim: buf.readUInt32LE(8),
    layers: buf.readUInt32LE(12),
    weightsOffset: buf.readUInt32LE(16),
    weightsLen: buf.readUInt32LE(20),
  };
  report.header = header;

  const magic = describeMagic(header.magic);
  report.detected = {
    magic: header.magic,
    magicHex: '0x' + (header.magic >>> 0).toString(16).padStart(8, '0'),
    format: magic.name,
    advice: magic.advice,
  };

  if (!rule('magic', magic.expected,
    magic.expected
      ? 'magic matches .hydra'
      : (magic.advice || `wrong magic ${report.detected.magicHex}, expected 0x48594452 "HYDR"`))) {
    return report;
  }
  rule('version', header.version === trainer.VERSION,
    header.version === trainer.VERSION
      ? `version ${header.version} is supported`
      : `unsupported .hydra version ${header.version} (this build reads ${trainer.VERSION})`);
  rule('dim', header.dim >= 1 && header.dim <= MAX_DIM,
    `dim ${header.dim} in 1..${MAX_DIM}`);
  rule('vocab', header.vocab >= 1 && header.vocab <= trainer.MAX_VOCAB,
    `vocab ${header.vocab} in 1..${trainer.MAX_VOCAB}`);
  rule('layers', header.layers >= 1 && header.layers <= trainer.MAX_LAYERS,
    `layers ${header.layers} in 1..${trainer.MAX_LAYERS}`);
  rule('weights_offset', header.weightsOffset >= HEADER_BYTES,
    `weights_offset ${header.weightsOffset} >= ${HEADER_BYTES} (must not point into the header)`);

  /* Number arithmetic, not 32-bit: this is the exact wrap the C loader has
   * to avoid (0xFFFFF000 + 0x1000 === 0 in uint32). */
  const weightsEnd = header.weightsOffset + header.weightsLen;
  rule('weights_fit', weightsEnd <= size,
    `weights end at ${formatBytes(weightsEnd)} of a ${formatBytes(size)} file`);
  rule('pairs_covered', header.layers * header.dim <= header.weightsLen,
    `layers x dim = ${header.layers * header.dim} pairs, weights_len covers ${Math.floor(header.weightsLen / 4)}`);

  /* Informational: the shape ceiling of the CURRENT format. A valid model
   * can still be at the ceiling, and knowing that up front is what stops
   * somebody from waiting for a 20 GB .hydra that the header cannot
   * describe. */
  const shapeBytes = header.layers * header.dim;
  rule('shape_within_format_ceiling', shapeBytes <= report.limits.maxWeightsBytesForShape,
    `weights need ${formatBytes(shapeBytes)}; the current format can describe at most `
    + `${formatBytes(report.limits.maxWeightsBytesForShape)} (dim ${MAX_DIM} x layers ${trainer.MAX_LAYERS} bytes)`);

  report.valid = report.violations.length === 0;
  return report;
}

/**
 * The single error message for a refused model, derived from the analysis
 * above. Returns null when the file is fine.
 */
function validateHydraHeader(file, size) {
  let report;
  try {
    report = analyseHydraFile(file, size);
  } catch (e) {
    return `model could not be read: ${e.message}`;
  }
  if (report.valid) return null;
  /* First violation wins in the message; the full list is what
   * /api/models/inspect is for. */
  const first = report.rules.find((r) => !r.ok);
  return first ? first.message : 'model rejected';
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

/* Real cancellation.
 *
 * execFile gives us the ChildProcess, and an HTTP request that the client
 * has already abandoned gives us nothing - Node does not tell the server
 * that the socket is gone. So each in-flight engine run is registered with
 * an id that the client sends back, and `req.on('close')` kills the child.
 * That is the difference between "the answer was thrown away" and "the
 * work stopped": the previous Cancel button did the former and called it
 * the latter.
 *
 * SIGTERM first, SIGKILL after a grace period, so a process wedged in a
 * syscall cannot pin a core forever. */
const runningEngines = new Map();

function killEngine(id, reason) {
  const entry = runningEngines.get(id);
  if (!entry) return false;
  entry.cancelled = reason || 'cancelled';
  try {
    entry.child.kill('SIGTERM');
  } catch {
    /* already gone */
  }
  const t = setTimeout(() => {
    try {
      entry.child.kill('SIGKILL');
    } catch {
      /* already gone */
    }
  }, 2000);
  if (typeof t.unref === 'function') t.unref();
  runningEngines.set(id, entry);
  return true;
}

function engineJson(args, requestId) {
  return new Promise((resolve, reject) => {
    const child = execFile(
      HYDRA_RUN,
      args,
      { timeout: ENGINE_TIMEOUT_MS, maxBuffer: 4 * 1024 * 1024 },
      (err, stdout, stderr) => {
        /* The entry has to be read BEFORE the map slot is dropped,
         * otherwise the "was this killed on purpose?" flag is always null
         * and a cancelled run is reported as an engine failure. */
        const entry = requestId ? runningEngines.get(requestId) : null;
        if (requestId) runningEngines.delete(requestId);
        if (err) {
          /* stderr goes to the server log, never into the HTTP response. */
          const e = new Error('engine failed');
          e.detail = stderr || String(err.message);
          /* A kill we asked for shows up as err.killed / err.signal, and
           * so does a kill we recorded in the entry. Reporting either as a
           * plain engine failure would put a red error on the screen for
           * something the user asked for. */
          e.cancelled = Boolean(entry && entry.cancelled)
            || Boolean(err.killed) || err.signal === 'SIGTERM';
          reject(e);
          return;
        }
        try {
          resolve(JSON.parse(stdout));
        } catch (parseErr) {
          reject(new Error('engine output not JSON: ' + parseErr.message));
        }
      },
    );
    if (requestId) {
      runningEngines.set(requestId, { child, cancelled: false });
    }
  });
}

/* prompt: optional token sequence fed through the engine before generation.
 * An empty prompt keeps the old single-seed behaviour (token). */
function runInference(modelPath, token, steps, prompt, requestId) {
  const args = [modelPath, String(token), String(steps), '--json'];
  if (Array.isArray(prompt) && prompt.length > 0) args.push('--prompt', prompt.join(','));
  return engineJson(args, requestId);
}

function loadModelInfo(modelPath) {
  return engineJson([modelPath, '0', '1', '--json']).then((j) => ({
    dim: j.dim,
    vocab: j.vocab,
    layers: j.layers,
    /* Gemessen vom Loader im selben Aufruf, nicht geschaetzt: das Modell
     * wird fuer diese Antwort ohnehin einmal geladen. */
    load_ms: j.load_ms,
    weights_bytes: j.weights_bytes,
    path: path.relative(ROOT, modelPath),
  }));
}

/* ------------------------------------------------------------------ */
/* GGUF import                                                        */
/* ------------------------------------------------------------------ */

/** Resolve an import name to a .hydra path inside models/converted/. */
function convertedTarget(name) {
  if (typeof name !== 'string' || !name.endsWith('.hydra')) return null;
  const key = modelKey(name.slice(0, -'.hydra'.length));
  if (key === null) return null;
  const dir = path.resolve(CONVERTED_DIR);
  const file = path.resolve(dir, `${key}.hydra`);
  const rel = path.relative(dir, file);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) return null;
  return file;
}

/**
 * Run tools/gguf_to_hydra.py and return its report.
 *
 * The converter is a separate process on purpose: it is a few hundred
 * lines of pure-Python decoding that can take seconds on a real model, and
 * running it inline would block every other request in the single-threaded
 * event loop. Its stderr is captured because the message there is written
 * for the user - it names the quantised type and the flag that fixes it.
 */
function runGgufConverter(ggufPath, outPath) {
  const vocabOut = path.join(VOCAB_DIR, `${path.basename(outPath, '.hydra')}.json`);
  const args = [
    GGUF_CONVERTER, ggufPath,
    '--out', outPath,
    '--vocab-out', vocabOut,
    '--json',
  ];
  return new Promise((resolve) => {
    execFile('python3', args, {
      cwd: ROOT,
      timeout: CONVERT_TIMEOUT_MS,
      maxBuffer: 8 * 1024 * 1024,
    }, (err, stdout, stderr) => {
      let report = null;
      try {
        report = JSON.parse(stdout);
      } catch {
        /* stdout was not JSON: the converter crashed or printed usage. The
         * stderr message below is what the user needs. */
      }
      /* stdout is the converter's JSON report; stderr carries the message it
       * wrote for the user. Both are bounded by maxBuffer above. */
      if (err && !report) {
        resolve({
          ok: false,
          error: (stderr || '').trim() || `converter failed: ${err.message}`,
        });
        return;
      }
      if (!report || report.ok !== true) {
        resolve({ ok: false, error: (report && report.error) || 'conversion failed' });
        return;
      }
      resolve(report);
    });
  });
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
      /* A model that came out of the GGUF converter says so in the chat's
       * model list, so it is never mistaken for a trained model. */
      converted: relPath.startsWith('models' + path.sep + 'converted' + path.sep),
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

async function handleTrain(body, requestId) {
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
  const verification = await verifyModelFile(file, cfg, samples[0].input[0], 8, requestId);
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
async function verifyModelFile(file, cfg, seedToken, steps, requestId) {
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

  /* Registered under the caller's id so a client that hangs up during the
   * verification run can stop that engine too. */
  const engine = await engineJson(
    [file, String(seedToken), String(steps), '--json'],
    requestId,
  );
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
        /* The stream already removed the temp file. */
        if (e.tooLarge) {
          /* Answer FIRST, close afterwards. Destroying the request here is
           * what used to make this reply arrive empty. */
          res.setHeader('Connection', 'close');
          sendJSON(res, e.status, {
            error: e.message,
            rejected: true,
            limitBytes: e.limitBytes,
            limitHuman: formatBytes(e.limitBytes),
            raiseWith: 'HYDRA_MAX_UPLOAD',
          });
          res.on('finish', () => { if (!req.destroyed) req.destroy(); });
          return;
        }
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

    /* GGUF -> .hydra. One POST, one model: the bytes are streamed to disk,
     * recognised, converted by tools/gguf_to_hydra.py and validated by the
     * SAME analysis every other path uses - so a converted model that came
     * out of here is a model the engine can load, or the request fails.
     *
     * The report comes back verbatim: which tensors were read, how many rows
     * of each, what the absmean scale was, how dense the ternary result is.
     * A conversion that quietly produced something different from what was
     * asked for is worse than one that is refused. */
    if (url.pathname === '/api/models/import' && req.method === 'POST') {
      const name = url.searchParams.get('name') || '';
      if (!name.toLowerCase().endsWith('.gguf')) {
        return sendJSON(res, 400, {
          error: 'invalid file name: the import expects a .gguf suffix '
            + '(a .hydra model goes to /api/models/upload)',
        });
      }
      const target = convertedTarget(`${name.slice(0, -'.gguf'.length)}.hydra`);
      if (!target) {
        return sendJSON(res, 400, {
          error: 'invalid file name: use letters, digits, dot, dash and underscore before the suffix',
        });
      }
      fs.mkdirSync(CONVERTED_DIR, { recursive: true });
      const tmp = path.join(CONVERTED_DIR, `.import-${process.pid}-${Date.now()}.gguf`);
      try {
        await streamToTmpFile(req, tmp, MAX_UPLOAD);
      } catch (e) {
        if (e.tooLarge) {
          res.setHeader('Connection', 'close');
          sendJSON(res, e.status, {
            error: e.message,
            rejected: true,
            limitBytes: e.limitBytes,
            limitHuman: formatBytes(e.limitBytes),
            raiseWith: 'HYDRA_MAX_UPLOAD',
          });
          res.on('finish', () => { if (!req.destroyed) req.destroy(); });
          return;
        }
        return sendJSON(res, e.status || 400, { error: e.message });
      }

      const report = await runGgufConverter(tmp, target);
      /* The source GGUF never reaches a model path: only the converted
       * .hydra is offered, and only after the header check below. */
      fs.unlinkSync(tmp);

      if (!report.ok) {
        if (fs.existsSync(target)) fs.unlinkSync(target);
        return sendJSON(res, 400, { error: report.error, rejected: true });
      }

      const reason = validateHydraHeader(target);
      if (reason !== null) {
        fs.unlinkSync(target);
        return sendJSON(res, 500, {
          error: `the converter produced a model the engine refuses: ${reason}`,
          rejected: true,
        });
      }

      const info = inspectHeader(path.relative(ROOT, target));
      /* The conversion report is the answer, not a log line: it is the only
       * place that says what the new model actually is. */
      return sendJSON(res, 200, { ok: true, model: info, conversion: report });
    }

    /* Why is this file not loading? One endpoint, one answer, the same
     * analysis the upload route uses - so the message on upload and the
     * report here can never contradict each other.
     *
     * Only paths inside the model directories are accepted (safeModelPath);
     * a diagnostics endpoint that can read arbitrary files on the host
     * would be an information-disclosure hole, not a feature. */
    if (url.pathname === '/api/models/inspect' && req.method === 'GET') {
      const rel = url.searchParams.get('path') || '';
      if (!rel) return sendJSON(res, 400, { error: 'path query parameter is required' });
      /* Syntactic checks first, so "does not exist" can be a 404 instead of
       * collapsing into the same 400 a forbidden path gets. safeModelPath()
       * deliberately conflates the two (realpath fails for a missing file),
       * which would make "no such model" indistinguishable from "not
       * allowed". */
      if (rel.includes('\0') || !rel.endsWith('.hydra')) {
        return sendJSON(res, 400, { error: 'invalid model path', checked: rel });
      }
      const resolved = path.resolve(ROOT, rel);
      if (resolved !== ROOT && !resolved.startsWith(ROOT + path.sep)) {
        return sendJSON(res, 400, { error: 'invalid model path', checked: rel });
      }
      if (!fs.existsSync(resolved)) {
        return sendJSON(res, 404, { error: 'no such model file', path: rel });
      }
      const abs = safeModelPath(rel);
      if (!abs) {
        return sendJSON(res, 400, { error: 'invalid model path', checked: rel });
      }
      let report;
      try {
        report = analyseHydraFile(abs);
      } catch (e) {
        return sendJSON(res, 500, { error: 'model could not be read', detail: e.message });
      }
      report.path = rel;
      report.trained = rel.startsWith('models' + path.sep + 'trained' + path.sep);
      report.uploaded = rel.startsWith('models' + path.sep + 'uploaded' + path.sep);
      report.converted = rel.startsWith('models' + path.sep + 'converted' + path.sep);
      /* 200 even for an invalid model: the caller asked for a diagnosis,
       * and "here is why it is broken" is a successful answer. */
      return sendJSON(res, 200, report);
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
      /* Every request gets an id so the engine process can be addressed.
       * req.on('close') fires when the client goes away for ANY reason,
       * including a completed response - so the flag is cleared on
       * completion and only an actually-abandoned request kills. */
      /* The client may supply its own id so that an explicit
       * POST /api/cancel can address this exact run; otherwise we mint
       * one. */
      const supplied = typeof body.request_id === 'string' && body.request_id.length <= 64
        ? body.request_id
        : null;
      const requestId = supplied || randomUUID();
      let finished = false;
      /* The response stream is the signal, not the request stream.
       * Measured: with only req.on('close'), an aborted fetch never fired
       * the hook and the engine ran to completion - the Cancel button
       * dropped the answer and kept the work. res.on('close') fires when
       * the connection dies mid-response, and 'finish' always comes first
       * for a request that completed, so the flag separates the two. */
      res.on('finish', () => { finished = true; runningEngines.delete(requestId); });
      res.on('close', () => {
        if (!finished) killEngine(requestId, 'client disconnected');
      });

      try {
        const result = await runInference(p, token, steps, prompt, requestId);
        return sendJSON(res, 200, result);
      } catch (e) {
        if (e && e.cancelled) {
          /* Deliberately NOT logged as a server error: the user asked for
           * this, and the crash log exists for things that went wrong.
           *
           * A client that only pressed POST /api/cancel is still waiting
           * for an answer, so it gets one - 409 with an explicit
           * "cancelled", never a 500 that reads like an engine failure.
           * A client that hung up needs nothing; the write is a no-op. */
          if (!res.writableEnded && !res.destroyed) {
            sendJSON(res, 409, { error: 'cancelled', cancelled: true, id: requestId });
          }
          return;
        }
        throw e;
      }
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
      /* Honest cancellation for training: the hill climbing itself runs
       * in-process (trainer.train is synchronous JavaScript), so a client
       * that hangs up cannot interrupt it - only the engine verification
       * call at the end is a child process, and that one is killed. Saying
       * so is better than a killEngine(trainId) that never matches. */
      const trainId = randomUUID();
      let trainFinished = false;
      res.on('finish', () => { trainFinished = true; });
      res.on('close', () => {
        if (!trainFinished) killEngine(trainId, 'client disconnected');
      });
      const out = await handleTrain(body, trainId);
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

    if (url.pathname === '/api/cancel' && req.method === 'POST') {
      let body;
      try {
        body = JSON.parse((await readBody(req)) || '{}');
      } catch {
        return sendJSON(res, 400, { error: 'invalid JSON body' });
      }
      const id = String(body.id || '');
      if (!runningEngines.has(id)) {
        return sendJSON(res, 404, { error: 'no such running engine call' });
      }
      killEngine(id, 'cancelled by the client');
      return sendJSON(res, 200, { ok: true, id, killed: true });
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
 * The starter model is generated output, not a checked-in binary. A clean
 * checkout therefore has no models/ directory at all, so we create it from
 * the repository's own writer instead of shipping a stale blob.
 */
function ensureDefaultModel() {
  fs.mkdirSync(MODELS_DIR, { recursive: true });
  if (fs.existsSync(DEFAULT_MODEL)) return;
  try {
    execFileSync('python3', [
      path.join(ROOT, 'tools', 'make_model.py'),
      DEFAULT_MODEL,
    ]);
    console.log('[Hydra Console] generated the default starter model');
  } catch (e) {
    console.error('[Hydra Console] could not generate the starter model:', e.message);
  }
}

ensureDefaultModel();
installCrashHandlers();

server.listen(PORT, HOST, () => {
  console.log(`[Hydra Console] http://${HOST}:${PORT}`);
});