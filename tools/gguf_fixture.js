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

function buildGgufBytes({
  rows = 64, cols = 32, tokens = ['a', 'b', '▁c', 'd'], alignment = ALIGNMENT,
} = {}) {
  const tensors = [
    { name: 'token_embd.weight', dims: [rows, cols], data: referenceMatrix(rows, cols, 7) },
    { name: 'blk.0.attn_q.weight', dims: [cols, cols], data: referenceMatrix(cols, cols, 99) },
  ];
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
    buf.writeUInt32LE(0, p); p += 4;          // ggml_type F32
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

module.exports = { buildGgufBytes, writeGgufFile, referenceMatrix };