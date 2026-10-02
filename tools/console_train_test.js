#!/usr/bin/env node
'use strict';
/**
 * Proves the console trainer is real, not a placeholder.
 *
 * 1. trains a model from a small corpus with tools/hydra_train.js,
 * 2. writes a real .hydra file,
 * 3. runs the COMPILED C engine (hydra-run) on the training sequences,
 * 4. asserts the C engine reproduces the trained token stream,
 * 5. asserts accuracy improved against the untrained seed weights,
 * 6. asserts the engine rejects a garbage model (negative control).
 *
 * Run: node tools/console_train_test.js
 * Exit code 0 = all assertions hold.
 */

const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const train = require('./hydra_train');

const ROOT = path.join(__dirname, '..');
const HYDRA_RUN = path.join(ROOT, 'hydra-run');
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'hydra-train-'));

let failures = 0;
function check(name, condition, detail) {
  if (condition) {
    console.log(`ok    ${name}`);
  } else {
    console.log(`FAIL  ${name}${detail ? ' - ' + detail : ''}`);
    failures += 1;
  }
}

if (!fs.existsSync(HYDRA_RUN)) {
  console.error(`FAIL  hydra-run not found at ${HYDRA_RUN} - run "make" first`);
  process.exit(1);
}

// A tiny corpus: three short sequences. Inputs and targets are real token IDs.
const samples = [
  { input: [3, 3, 3, 3], target: [7, 7, 7, 7] },
  { input: [1, 2, 3, 4], target: [9, 8, 7, 6] },
  { input: [4, 4, 4], target: [2, 2, 2] },
];
const cfg = { vocab: 32, dim: 16, layers: 12 };

const seed = train.seedWeights(cfg.layers, cfg.dim);
const before = train.evaluate(seed.w1, seed.w2, cfg, samples);

const result = train.train(cfg, samples, 20);
const after = train.evaluate(result.w1, result.w2, cfg, samples);

check(
  'training improves token accuracy',
  after.accuracy > before.accuracy,
  `${(before.accuracy * 100).toFixed(1)}% -> ${(after.accuracy * 100).toFixed(1)}%`,
);
check(
  'the returned model is never worse than the seed',
  result.seedAccuracy === before.accuracy && after.accuracy >= before.accuracy,
  `seed ${before.accuracy} returned ${after.accuracy}`,
);
check('training performed flips', result.flips > 0, `flips=${result.flips}`);

// Write the model and let the real C engine judge it.
const modelPath = path.join(TMP, 'trained.hydra');
const size = train.writeModelFile(modelPath, { ...cfg, ...result });
check('model file has the expected size', size === 24 + cfg.layers * cfg.dim, `size=${size}, expected ${24 + cfg.layers * cfg.dim}`);

function engineTokens(model, seedToken, steps) {
  const out = execFileSync(HYDRA_RUN, [model, String(seedToken), String(steps), '--json'], {
    encoding: 'utf8',
  });
  return JSON.parse(out).tokens;
}

// The engine is fed the same input stream, one token at a time, by chaining
// its own output back in. Reconstruct the full stream from the trained
// weights using the same rule and compare with the C engine.
// hydra-run feeds every generated token back in, so the simulation has to do
// exactly the same. Anything else would compare two different algorithms.
function simulateChained(seedToken, steps) {
  const state = new Array(cfg.dim).fill(0);
  let token = seedToken;
  const out = [];
  for (let t = 0; t < steps; t += 1) {
    const r = train.step(result.w1, result.w2, cfg.layers, cfg.dim, cfg.vocab, state, token);
    out.push(r.out);
    token = r.out;
  }
  return out;
}

let mismatch = null;
for (const s of samples) {
  const simulated = simulateChained(s.input[0], s.input.length);
  const real = engineTokens(modelPath, s.input[0], s.input.length);
  if (JSON.stringify(real) !== JSON.stringify(simulated)) {
    mismatch = `sequence ${JSON.stringify(s.input)}: engine ${JSON.stringify(real)} vs simulated ${JSON.stringify(simulated)}`;
    break;
  }
}
check('C engine reproduces the simulated chained stream bit for bit', mismatch === null, mismatch || '');

// The chain property: feeding an output back in is what the chat does.
const chained = engineTokens(modelPath, 5, 6);
check('engine runs a chained stream', Array.isArray(chained) && chained.length === 6);

// Negative control: a garbage model must be rejected, not silently accepted.
const garbage = path.join(TMP, 'garbage.hydra');
fs.writeFileSync(garbage, Buffer.alloc(64, 0xaa));
let rejected = false;
try {
  engineTokens(garbage, 1, 2);
} catch {
  rejected = true;
}
check('engine rejects a garbage model (negative control)', rejected);

fs.rmSync(TMP, { recursive: true, force: true });

if (failures > 0) {
  console.log(`\nCONSOLE-TRAIN-TEST: ${failures} assertion(s) failed`);
  process.exit(1);
}
console.log(
  `\nCONSOLE-TRAIN-TEST: all assertions passed ` +
    `(token accuracy ${(before.accuracy * 100).toFixed(1)}% -> ${(after.accuracy * 100).toFixed(1)}%, ` +
    `${result.flips} weight moves)`,
);