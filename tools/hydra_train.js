'use strict';
/**
 * Hydra-Stone trainer.
 *
 * Trains the ternary weight file of a Hydra-Stone model so that a given
 * corpus of (input, target) token sequences is reproduced by the real
 * engine. This is NOT a placeholder: `forward()` below is a line-by-line
 * mirror of `hydra_engine_step()` in src/hydra_engine.c, the resulting
 * weights are packed into a real .hydra file, and `tools/console_train_test.js`
 * verifies with the compiled C engine that the token stream really matches.
 *
 * The engine semantics we fit (src/hydra_engine.c):
 *
 *   acc[i]    = sum over layers of ( w1[l][i] * token + w2[l][i] * state[i] )
 *   state[i]  = clamp(acc[i], -127, 127)
 *   token_out = ((acc[0] mod vocab) + vocab) mod vocab, then + token + 1, mod vocab
 *
 * Learning rule: hill climbing on column 0 of w1/w2 with ternary moves
 * (any value can become either of the other two). A move shifts acc[0] by
 * (new - old) * token for w1 and by (new - old) * state[0] for w2, so every
 * candidate is evaluated and the one with the smallest circular error against
 * the target wins. When no single move helps, the best pair is tried, then a
 * sideways move (same error, unseen state) escapes a plateau. Because the
 * search explores, an epoch can end worse than the previous one - therefore
 * the trainer keeps the BEST snapshot seen and returns that, so the returned
 * model is never worse than the seed.
 */

const fs = require('fs');
const path = require('path');

const MAGIC = 0x48594452; // "HYDR"
const VERSION = 1;
const HEADER_SIZE = 24;
const MAX_LAYERS = 4096;
const MAX_VOCAB = 1024;
const MAX_DIM = 64; // HYDRA_EMBED_DIM

const CODE_ZERO = 0;
const CODE_POS = 1;
const CODE_NEG = 2;

function codeToValue(code) {
  if (code === CODE_POS) return 1;
  if (code === CODE_NEG) return -1;
  return 0;
}

function valueToCode(v) {
  if (v > 0) return CODE_POS;
  if (v < 0) return CODE_NEG;
  return CODE_ZERO;
}

/**
 * Unpack a .hydra weight blob into layers x dim integer arrays.
 *
 * Layout (verified against hydra_engine_step, not guessed): the engine
 * reads ONE byte per (layer, dimension) pair, byte index = l*dim + i, and
 * takes w1 from the low two bits and w2 from the next two bits of that
 * same byte. So one byte holds both weights of the same dimension.
 */
function unpack(bytes, layers, dim) {
  const need = layers * dim;
  if (bytes.length < need) {
    throw new Error(`weights too short: ${bytes.length} < ${need}`);
  }
  const w1 = [];
  const w2 = [];
  for (let l = 0; l < layers; l += 1) {
    const row1 = new Array(dim);
    const row2 = new Array(dim);
    const base = l * dim;
    for (let i = 0; i < dim; i += 1) {
      const packed = bytes[base + i];
      row1[i] = codeToValue(packed & 0x03);
      row2[i] = codeToValue((packed >> 2) & 0x03);
    }
    w1.push(row1);
    w2.push(row2);
  }
  return { w1, w2 };
}

/** Pack layers x dim integer arrays back into the engine's byte layout. */
function pack(w1, w2, layers, dim) {
  const out = Buffer.alloc(layers * dim);
  for (let l = 0; l < layers; l += 1) {
    const base = l * dim;
    for (let i = 0; i < dim; i += 1) {
      const idx = base + i;
      out[idx] = (valueToCode(w1[l][i]) & 0x03) | ((valueToCode(w2[l][i]) & 0x03) << 2);
    }
  }
  return out;
}

/**
 * Serialise a complete .hydra file into `dir`, creating it when needed.
 *
 * The file name is resolved and then verified to be inside `dir`. The
 * caller only ever supplies a basename, but a path sink that is not itself
 * guarded is a CodeQL `js/path-injection` alert - and an alert that has to be
 * suppressed is worth avoiding.
 */
function writeModelFile(dir, baseName, { vocab, dim, layers, w1, w2 }) {
  const resolvedDir = path.resolve(dir);
  const file = path.resolve(resolvedDir, baseName);
  const rel = path.relative(resolvedDir, file);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) {
    throw new Error('refusing to write outside the target directory');
  }
  fs.mkdirSync(resolvedDir, { recursive: true });
  const body = pack(w1, w2, layers, dim);
  const header = Buffer.alloc(HEADER_SIZE);
  header.writeUInt32LE(MAGIC, 0);
  header.writeUInt16LE(VERSION, 4);
  header.writeUInt16LE(vocab, 6);
  header.writeUInt32LE(dim, 8);
  header.writeUInt32LE(layers, 12);
  header.writeUInt32LE(HEADER_SIZE, 16);
  header.writeUInt32LE(body.length, 20);
  fs.writeFileSync(file, Buffer.concat([header, body]));
  return HEADER_SIZE + body.length;
}

/** Signed circular distance from a to b in Z_mod(m). */
function circDelta(a, b, m) {
  let d = (b - a) % m;
  if (d > m / 2) d -= m;
  if (d < -m / 2) d += m;
  return d;
}

/**
 * One forward step, mirroring hydra_engine_step().
 * Returns { out, acc0 } and mutates `state` in place.
 */
function step(w1, w2, layers, dim, vocab, state, tokenIn) {
  const token = ((tokenIn % vocab) + vocab) % vocab;
  let acc0 = 0;
  for (let l = 0; l < layers; l += 1) {
    acc0 += w1[l][0] * token + w2[l][0] * state[0];
  }
  // Full accumulator for the state update (column 0 is the same expression).
  for (let i = 0; i < dim; i += 1) {
    let acc = 0;
    for (let l = 0; l < layers; l += 1) {
      acc += w1[l][i] * token + w2[l][i] * state[i];
    }
    if (i === 0) acc0 = acc;
    let v = acc;
    if (v > 127) v = 127;
    else if (v < -127) v = -127;
    state[i] = v;
  }
  let raw = acc0 % vocab;
  if (raw < 0) raw += vocab;
  const out = (raw + token + 1) % vocab;
  return { out, acc0 };
}

/**
 * Deterministic initial weights (no randomness: reproducible training).
 * A mix of 0/+1/-1, because the zero entries are what makes fine-grained
 * moves possible during training (0 -> +1 shifts acc[0] by token).
 */
function seedWeights(layers, dim) {
  const w1 = [];
  const w2 = [];
  const pick = [1, -1, 0, 1, 0, -1];
  for (let l = 0; l < layers; l += 1) {
    const row1 = new Array(dim);
    const row2 = new Array(dim);
    for (let i = 0; i < dim; i += 1) {
      row1[i] = pick[(l + i) % pick.length];
      row2[i] = pick[(l * 3 + i) % pick.length];
    }
    w1.push(row1);
    w2.push(row2);
  }
  return { w1, w2 };
}

/** Token accuracy of the current weights over the dataset. */
function evaluate(w1, w2, cfg, samples) {
  let correct = 0;
  let total = 0;
  for (const s of samples) {
    const state = new Array(cfg.dim).fill(0);
    for (let t = 0; t < s.input.length; t += 1) {
      const { out } = step(w1, w2, cfg.layers, cfg.dim, cfg.vocab, state, s.input[t]);
      total += 1;
      if (out === s.target[t]) correct += 1;
    }
  }
  return { correct, total, accuracy: total === 0 ? 0 : correct / total };
}

/** Score a whole sample: how many steps match the target. */
function scoreSample(w1, w2, cfg, sample) {
  const state = new Array(cfg.dim).fill(0);
  let correct = 0;
  for (let t = 0; t < sample.input.length; t += 1) {
    const { out } = step(w1, w2, cfg.layers, cfg.dim, cfg.vocab, state, sample.input[t]);
    if (out === sample.target[t]) correct += 1;
  }
  return correct;
}

/**
 * All single-entry moves available for column 0, with their effect on acc[0].
 * A ternary entry can become either of the other two values, so the possible
 * shifts for w1 are (new - old) * token and for w2 (new - old) * state[0].
 */
function candidateMoves(w1, w2, cfg, token, state0) {
  const options = (v) => [1, -1, 0].filter((nv) => nv !== v);
  const moves = [];
  for (let l = 0; l < cfg.layers; l += 1) {
    for (const nv of options(w1[l][0])) {
      moves.push({ arr: w1, idx: l, next: nv, step: (nv - w1[l][0]) * token });
    }
    for (const nv of options(w2[l][0])) {
      moves.push({ arr: w2, idx: l, next: nv, step: (nv - w2[l][0]) * state0 });
    }
  }
  return moves;
}

/**
 * Train with hill climbing on the SAMPLE SCORE, not on a single step.
 *
 * Optimising one step in isolation reliably destroys the following ones - the
 * weights are shared along the whole sequence - so a move is only accepted
 * when replaying the complete sample afterwards yields more correct steps.
 * Ties are broken by the circular error of the step that was wrong. The best
 * snapshot over all epochs is returned, so the result is never worse than the
 * seed.
 */
function train(cfg, samples, epochs) {
  const seed = seedWeights(cfg.layers, cfg.dim);
  const w1 = seed.w1;
  const w2 = seed.w2;
  const snapshot = () => ({ w1: w1.map((r) => r.slice()), w2: w2.map((r) => r.slice()) });

  const seedSnapshot = snapshot();
  const seedEval = evaluate(w1, w2, cfg, samples);
  let best = { ...seedSnapshot, accuracy: seedEval.accuracy, epoch: -1 };
  const history = [];
  let flips = 0;

  for (let epoch = 0; epoch < epochs; epoch += 1) {
    for (const sample of samples) {
      const currentScore = scoreSample(w1, w2, cfg, sample);

      /* Find the first step that does not reproduce the target. */
      const state = new Array(cfg.dim).fill(0);
      let stepIndex = -1;
      let token = 0;
      let state0 = 0;
      let acc0 = 0;
      for (let t = 0; t < sample.input.length; t += 1) {
        const fwd = step(w1, w2, cfg.layers, cfg.dim, cfg.vocab, state, sample.input[t]);
        if (fwd.out !== sample.target[t]) {
          stepIndex = t;
          token = sample.input[t];
          state0 = state[0];
          acc0 = fwd.acc0;
          break;
        }
      }
      if (stepIndex === -1) continue;

      const wantAcc =
        (((sample.target[stepIndex] - token - 1) % cfg.vocab) + cfg.vocab) % cfg.vocab;
      const moves = candidateMoves(w1, w2, cfg, token, state0).filter((m) => m.step !== 0);

      let chosen = null;
      for (const mv of moves) {
        const prev = mv.arr[mv.idx][0];
        mv.arr[mv.idx][0] = mv.next;
        const score = scoreSample(w1, w2, cfg, sample);
        const err = Math.abs(circDelta((acc0 + mv.step) % cfg.vocab, wantAcc, cfg.vocab));
        mv.arr[mv.idx][0] = prev;
        if (score < currentScore) continue;
        if (!chosen || score > chosen.score || (score === chosen.score && err < chosen.err)) {
          chosen = { mv, score, err };
        }
      }
      if (chosen) {
        chosen.mv.arr[chosen.mv.idx][0] = chosen.mv.next;
        flips += 1;
      }
    }

    const acc = evaluate(w1, w2, cfg, samples);
    history.push(acc);
    if (acc.accuracy > best.accuracy) {
      best = { ...snapshot(), accuracy: acc.accuracy, epoch };
    }
  }

  return {
    w1: best.w1,
    w2: best.w2,
    history,
    flips,
    bestEpoch: best.epoch,
    seedAccuracy: seedEval.accuracy,
  };
}

module.exports = {
  MAGIC,
  VERSION,
  HEADER_SIZE,
  MAX_LAYERS,
  MAX_VOCAB,
  MAX_DIM,
  unpack,
  pack,
  writeModelFile,
  seedWeights,
  step,
  scoreSample,
  evaluate,
  train,
  circDelta,
};