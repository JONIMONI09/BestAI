#!/usr/bin/env node
/* Hydra-Stone Console Server
 *
 * Minimaler HTTP-Server (keine npm-Abhaengigkeiten), der:
 *  - die statische Weboberflaeche unter public/ ausliefert
 *  - die C-Engine (hydra-run --json) als REST-API anbindet
 *
 * Endpoints:
 *  GET  /api/model               -> Info zum geladenen Standardmodell
 *  POST /api/infer {token,steps} -> Inferenz via hydra-run --json
 *  GET  /api/axiom?h=0.5         -> Koexistenz-Axiom-Check
 *
 * Sicherheit: Modellpfade werden auf *.hydra im Projektverzeichnis
 * eingeschraenkt, keine Shell-Interpolation, execFile ohne shell.
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');

const PORT = parseInt(process.env.PORT || '8787', 10);
const ROOT = __dirname;
const PUBLIC = path.join(ROOT, 'public');
const HYDRA_RUN = path.join(ROOT, 'hydra-run');
const DEFAULT_MODEL = path.join(ROOT, 'models', 'demo.hydra');

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
      if (data.length > 64 * 1024) reject(new Error('body too large'));
    });
    req.on('end', () => resolve(data));
    req.on('error', reject);
  });
}

/* Sicherheit: nur *.hydra-Dateien innerhalb von ROOT erlauben */
function safeModelPath(p) {
  if (typeof p !== 'string' || !p.endsWith('.hydra')) return null;
  const abs = path.resolve(ROOT, p);
  if (!abs.startsWith(ROOT + path.sep)) return null;
  if (!fs.existsSync(abs)) return null;
  return abs;
}

function runInference(modelPath, token, steps) {
  return new Promise((resolve, reject) => {
    execFile(
      HYDRA_RUN,
      [modelPath, String(token), String(steps), '--json'],
      { timeout: 5000, maxBuffer: 1024 * 1024 },
      (err, stdout) => {
        if (err) return reject(err);
        try {
          resolve(JSON.parse(stdout));
        } catch (e) {
          reject(new Error('engine output not JSON: ' + e.message));
        }
      }
    );
  });
}

function loadModelInfo(modelPath) {
  return new Promise((resolve, reject) => {
    execFile(
      HYDRA_RUN,
      [modelPath, '0', '1', '--json'],
      { timeout: 5000, maxBuffer: 1024 * 1024 },
      (err, stdout) => {
        if (err) return reject(err);
        try {
          const j = JSON.parse(stdout);
          resolve({ dim: j.dim, vocab: j.vocab, layers: j.layers, path: modelPath });
        } catch (e) {
          reject(new Error('engine output not JSON: ' + e.message));
        }
      }
    );
  });
}

function axiomCheck(h) {
  const v = Number(h);
  if (!Number.isFinite(v)) return { error: 'h must be a number' };
  const safe = v <= 0 ? -1e9 : v; /* Engine-Logik gespiegelt: U = R * I{H=1} */
  return { humanity: v, allowed: v > 0, safe_score: v > 0 ? 99.5 * v : safe };
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);

  try {
    /* ---------- API ---------- */
    if (url.pathname === '/api/model' && req.method === 'GET') {
      /* Wie /api/infer: ein explizit angefragter, aber unzulaessiger Pfad
       * wird mit 400 quittiert statt auf das Defaultmodell zurueckzufallen. */
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
      /* Bugfix (Audit): JSON.parse('null') liefert null, '[]' ein Array —
       * body.token warf dann einen TypeError und antwortete mit HTTP 500
       * samt internem Fehlertext. Body-Form jetzt explizit pruefen. */
      let body;
      try {
        body = JSON.parse((await readBody(req)) || '{}');
      } catch (e) {
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

      /* Ein explizit angefragter, aber unzulaessiger Modellpfad darf nicht
       * stillschweigend auf das Defaultmodell zurueckfallen — der Client
       * wuerde ein Ergebnis fuer ein voellig anderes Modell bekommen. */
      let p = DEFAULT_MODEL;
      if (body.path !== undefined && body.path !== null && body.path !== '') {
        p = safeModelPath(body.path);
        if (!p) return sendJSON(res, 400, { error: 'invalid model path' });
      }
      const result = await runInference(p, token, steps);
      return sendJSON(res, 200, result);
    }

    if (url.pathname === '/api/axiom' && req.method === 'GET') {
      return sendJSON(res, 200, axiomCheck(url.searchParams.get('h')));
    }

    if (url.pathname.startsWith('/api/')) {
      return sendJSON(res, 404, { error: 'unknown endpoint' });
    }

    /* ---------- Statische UI ---------- */
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
    sendJSON(res, 500, { error: e.message });
  }
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[Hydra Console] http://0.0.0.0:${PORT}`);
});
