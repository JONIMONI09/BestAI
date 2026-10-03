'use strict';
/**
 * Builds GGUF bytes for the Node tests.
 *
 * The converter is Python, so the fixture is written here rather than
 * imported from tools/gguf_test.py: posting a file through the HTTP route
 * needs bytes in this process anyway, and a second implementation of the
 * GGUF writer would be a second thing to keep correct.
 *
 * Layout mirrors ggml/docs/gguf.md and tools/gguf_reader.py:
 *   "GGUF" | u32 version | u64 tensors | u64 kv | kv... | tensor infos... |
 *   pad to alignment | tensor data
 */
const fs = require('fs');
const path = require('path');

const GGUF_MAGIC = 0x46554747;
const GGML_F32 = 0;
const GGML_Q8_0 = 8;
const QK8_0 = 32;
const Q8_0_BLOCK_BYTES = 34;
const KV_UINT64 = 10;
const KV_FLOAT32 = 6;
const KV_STRING = 8;
const KV_ARRAY = 9;
const ALIGNMENT = 32;

/** A string field: u64 length, then the bytes. */
function ggufString(s) {
  const raw = Buffer.from(s, 'utf8');
  const out = Buffer.alloc(8 + raw.length);
  out.writeBigUInt64LE(BigInt(raw.length), 0);
  raw.copy(out, 8);
  return out;
}

function kvPair(key, value) {
  if (typeof value === 'string') {
    const type = Buffer.alloc(4);
    type.writeUInt32LE(KV_STRING, 0);
    return Buffer.concat([ggufString(key), type, ggufString(value)]);
  }
  if (Array.isArray(value)) {
    const type = Buffer.alloc(4);
    type.writeUInt32LE(KV_ARRAY, 0);
    const elem = Buffer.alloc(4);
    elem.writeUInt32LE(KV_STRING, 0);
    const count = Buffer.alloc(8);
    count.writeBigUInt64LE(BigInt(value.length), 0);
    const items = Buffer.concat(value.map(ggufString));
    return Buffer.concat([ggufString(key), type, elem, count, items]);
  }
  if (typeof value === 'number') {
    const isInt = Number.isInteger(value);
    const type = Buffer.alloc(4);
    type.writeUInt32LE(isInt ? KV_UINT64 : KV_FLOAT32, 0);
    const body = Buffer.alloc(8);
    if (isInt) body.writeBigUInt64LE(BigInt(value), 0);
    else body.writeFloatLE(value, 0);
    return Buffer.concat([ggufString(key), type, body]);
  }
  throw new Error(`unsupported fixture metadata type: ${typeof value}`);
}

/**
 * A deterministic, asymmetric matrix with exact zeros and both signs, so
 * the absmean scale is exercised on something that is not already ternary.
 */
function referenceMatrix(rows, cols, seed) {
  const out = Buffer.alloc(rows * cols * 4);
  let x = seed >>> 0;
  for (let i = 0; i < rows * cols; i += 1) {
    x = (Math.imul(x, 1103515245) + 12345) >>> 0;
    const r = (x >>> 7) % 97;
    const v = r < 20 ? 0 : (r < 50 ? r / 97 : -(r / 97));
    out.writeFloatLE(v, i * 4);
  }
  return out;
}

/**
 * Packs int8 codes as Q8_0: one F16 scale per 32 values, then the codes.
 *
 * The scales are powers of two, so scale * code is exactly representable in
 * binary16 - that is what lets the test compare a Q8_0 import against an F16
 * import of the same numbers with equality instead of a tolerance.
 */
/**
 * IEEE binary16 encoder (round to nearest, ties to even).
 *
 * Node's Buffer has no writeFloat16LE, and DataView.setFloat16 only landed
 * recently, so the Q8_0 scale has to be encoded here. Only the fixture uses
 * this - the production reader uses an exhaustive 65536-entry table.
 */
const _f32 = new Float32Array(1);
const _i32 = new Int32Array(_f32.buffer);

function toHalf(value) {
  _f32[0] = value;
  const bits = _i32[0];
  const sign = (bits >>> 16) & 0x8000;
  const rawExp = (bits >>> 23) & 0xff;
  const mant = bits & 0x7fffff;

  if (rawExp === 0xff) return sign | 0x7c00 | (mant ? 0x200 : 0);   // inf / NaN
  if (rawExp === 0 && mant === 0) return sign;                     // +/-0

  const exp = rawExp - 127 + 15;
  if (exp <= 0) {
    if (exp < -10) return sign;                                     // underflow to zero
    /* Subnormal half: value = m_half * 2^-24, and the F32 is
     * (2^23 + mant) * 2^(exp-38), so m_half = (2^23 + mant) >> (14 - exp).
     * The shift is 14 - exp, NOT 1 - exp: getting that wrong zeroes every
     * subnormal instead of encoding it. A differential run against
     * struct.pack('<e') over 5000+ random values is what proved it. */
    const wide = mant | 0x800000;
    const shift = 14 - exp;                       // 14..24, always < 32
    let m = wide >>> shift;
    const rem = wide & ((1 << shift) - 1);
    const halfway = 1 << (shift - 1);
    if (rem > halfway || (rem === halfway && (m & 1))) m += 1;   // round to nearest, ties to even
    return sign | m;                                // m may reach 0x400: that is the smallest normal
  }
  if (exp >= 31) return sign | 0x7c00;                              // overflow

  let m = mant >>> 13;
  const rem = mant & 0x1fff;
  if (rem > 0x1000 || (rem === 0x1000 && (m & 1))) m += 1;          // round
  if (m === 0x400) {                                                // carry into exponent
    if (exp + 1 >= 31) return sign | 0x7c00;
    return sign | ((exp + 1) << 10);
  }
  return sign | (exp << 10) | m;
}

function q8_0Pack(codes, scales) {
  const nBlocks = codes.length / QK8_0;
  if (!Number.isInteger(nBlocks)) {
    throw new Error(`Q8_0 needs a multiple of ${QK8_0} values, got ${codes.length}`);
  }
  const out = Buffer.alloc(nBlocks * Q8_0_BLOCK_BYTES);
  for (let b = 0; b < nBlocks; b += 1) {
    out.writeUInt16LE(toHalf(scales[b % scales.length]), b * Q8_0_BLOCK_BYTES);
    for (let i = 0; i < QK8_0; i += 1) {
      const q = codes[b * QK8_0 + i];
      if (q < -128 || q > 127) throw new Error(`Q8_0 needs int8, got ${q}`);
      out.writeInt8(q, b * Q8_0_BLOCK_BYTES + 2 + i);
    }
  }
  return out;
}

/** The dequantised values a Q8_0 tensor holds, for an F32/F16 twin file.
 *
 * The scale is round-tripped through toHalf first, because that is what the
 * reader will see: encoding the scale as the exact double the test wrote
 * would compare against a number the file does not contain. */
function q8_0Values(codes, scales) {
  const out = new Float64Array(codes.length);
  for (let b = 0; b * QK8_0 < codes.length; b += 1) {
    const d = halfToFloat(toHalf(scales[b % scales.length]));
    for (let i = 0; i < QK8_0; i += 1) {
      out[b * QK8_0 + i] = d * codes[b * QK8_0 + i];
    }
  }
  return out;
}

function halfToFloat(h) {
  const sign = (h & 0x8000) ? -1 : 1;
  const exp = (h >>> 10) & 0x1f;
  const mant = h & 0x3ff;
  if (exp === 0) return sign * mant * 2 ** -24;
  if (exp === 31) return mant ? NaN : sign * Infinity;
  return sign * (1 + mant / 1024) * 2 ** (exp - 15);
}

function float32Buffer(values) {
  const out = Buffer.alloc(values.length * 4);
  for (let i = 0; i < values.length; i += 1) out.writeFloatLE(values[i], i * 4);
  return out;
}

function buildGgufBytes({
  rows = 64, cols = 32, tokens = ['a', 'b', '▁c', 'd'], alignment = ALIGNMENT,
  type = GGML_F32, q8_0Twin = false,
} = {}) {
  let tensors;
  if (type === GGML_Q8_0 || q8_0Twin) {
    /* cols must tile whole 32-value blocks for a Q8_0 tensor. */
    if ((rows * cols) % QK8_0 !== 0) {
      throw new Error(`Q8_0 fixture needs rows*cols to be a multiple of ${QK8_0}`);
    }
    const codes = [];
    let x = 12345 >>> 0;
    for (let i = 0; i < rows * cols; i += 1) {
      x = (Math.imul(x, 1103516245) + 12345) >>> 0;
      codes.push(((x >>> 9) & 0xff) - 128);
    }
    /* Six different scales, all powers of two, so each block decodes exactly
     * and a decoder that reads the wrong block's scale produces a different
     * model rather than the same one by accident. */
    const scales = [1.0, 0.5, 0.25, -2.0, 0.125, 4.0];
    const codesB = codes.slice(0, cols * cols);
    if (q8_0Twin) {
      /* The same numbers, already dequantised, stored as F32. Converting both
       * files must give the identical model; that is the equivalence test. */
      tensors = [
        { name: 'token_embd.weight', dims: [rows, cols], data: float32Buffer(q8_0Values(codes, scales)), type: GGML_F32 },
        { name: 'blk.0.attn_q.weight', dims: [cols, cols], data: float32Buffer(q8_0Values(codesB, scales)), type: GGML_F32 },
      ];
    } else {
      tensors = [
        { name: 'token_embd.weight', dims: [rows, cols], data: q8_0Pack(codes, scales), type: GGML_Q8_0 },
        { name: 'blk.0.attn_q.weight', dims: [cols, cols], data: q8_0Pack(codesB, scales), type: GGML_Q8_0 },
      ];
    }
  } else {
    tensors = [
      { name: 'token_embd.weight', dims: [rows, cols], data: referenceMatrix(rows, cols, 7), type: GGML_F32 },
      { name: 'blk.0.attn_q.weight', dims: [cols, cols], data: referenceMatrix(cols, cols, 99), type: GGML_F32 },
    ];
  }
  const metadata = {
    'general.architecture': 'llama',
    'general.name': 'server-fixture',
    'general.alignment': alignment,
    'tokenizer.ggml.model': 'gpt2',
    'tokenizer.ggml.tokens': tokens,
  };
  const kvKeys = Object.keys(metadata);

  const head = Buffer.alloc(24);
  head.writeUInt32LE(GGUF_MAGIC, 0);
  head.writeUInt32LE(3, 4);
  head.writeBigUInt64LE(BigInt(tensors.length), 8);
  head.writeBigUInt64LE(BigInt(kvKeys.length), 16);

  const kv = Buffer.concat(kvKeys.map((k) => kvPair(k, metadata[k])));

  const infos = [];
  let offset = 0;
  for (const t of tensors) {
    const buf = Buffer.alloc(4 + t.dims.length * 8 + 12);
    let p = 0;
    buf.writeUInt32LE(t.dims.length, p); p += 4;
    for (const d of t.dims) { buf.writeBigUInt64LE(BigInt(d), p); p += 8; }
    buf.writeUInt32LE(t.type, p); p += 4;
    buf.writeBigUInt64LE(BigInt(offset), p);
    infos.push(Buffer.concat([ggufString(t.name), buf]));
    offset += t.data.length;
  }
  const infoBlock = Buffer.concat(infos);

  const pad = (alignment - ((head.length + kv.length + infoBlock.length) % alignment)) % alignment;
  return Buffer.concat([head, kv, infoBlock, Buffer.alloc(pad), ...tensors.map((t) => t.data)]);
}

/**
 * Writes a .gguf under models/converted/ and returns its path.
 *
 * The file lives inside the models tree on purpose: safeModelPath() only
 * serves files from there, and a fixture outside it would not exercise the
 * same path checks a real upload does.
 */
function writeGgufFile(dir, name, options) {
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, name);
  fs.writeFileSync(file, buildGgufBytes(options));
  return file;
}

module.exports = {
  buildGgufBytes, writeGgufFile, referenceMatrix, q8_0Pack, q8_0Values, toHalf,
  float32Buffer, GGML_F32, GGML_Q8_0,
};