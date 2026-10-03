#!/usr/bin/env python3
"""GGUF reader - Python standard library only.

A GGUF file is what llama.cpp, Ollama, LM Studio and friends ship. Hydra
reads .hydra. This module is the "recognise it" half of the bridge between
the two; tools/gguf_to_hydra.py is the "convert it" half.

Layout (little-endian throughout, see ggml/docs/gguf.md):

    "GGUF"            4 bytes
    version           uint32
    tensor_count      uint64
    metadata_kv_count uint64
    metadata          kv_count * (string key, uint32 type, value)
    tensor_infos      tensor_count * (string name, uint32 n_dims,
                                      uint64 dims[n_dims], uint32 type,
                                      uint64 offset)
    [pad to general.alignment]
    tensor data

Only the three float types are decoded: F32, F16 and BF16. Every other
ggml_type is a block-quantised layout (Q4_0, Q8_0, Q4_K, MXFP4, ...) whose
scales are interleaved with the values, and there is no honest way to
recover dense floats from it with arithmetic alone. Those tensors are
reported by name and refused, not silently misread - see
docs/GGUF-IMPORT.md.

Why no numpy: the engine, the console and the CI all work on a bare Python
3. A converter that needs a wheel the rest of the project does not have is
a converter nobody runs.
"""

import mmap
import os
import struct
import sys
from array import array

GGUF_MAGIC = 0x46554747  # b"GGUF"
GGUF_VERSION_MIN = 2
GGUF_VERSION_MAX = 3

# gguf_metadata_value_type
(KV_UINT8, KV_INT8, KV_UINT16, KV_INT16, KV_UINT32, KV_INT32, KV_FLOAT32,
 KV_BOOL, KV_STRING, KV_ARRAY, KV_UINT64, KV_INT64, KV_FLOAT64) = range(13)

_KV_SCALAR = {
    KV_UINT8: ('<B', 1),
    KV_INT8: ('<b', 1),
    KV_UINT16: ('<H', 2),
    KV_INT16: ('<h', 2),
    KV_UINT32: ('<I', 4),
    KV_INT32: ('<i', 4),
    KV_FLOAT32: ('<f', 4),
    KV_BOOL: ('<?', 1),      # one byte, nonzero means true
    KV_UINT64: ('<Q', 8),
    KV_INT64: ('<q', 8),
    KV_FLOAT64: ('<d', 8),
}

# ggml_type. Only the first three are dense floats.
T_F32, T_F16, T_BF16 = 0, 1, 30

#: Dense float types this reader can decode, mapped to their byte width.
DENSE_FLOAT_TYPES = {T_F32: 4, T_F16: 2, T_BF16: 2}

#: The quantised layouts worth naming in an error message. A file full of
#: these is the normal case for a 4-bit download, so saying so plainly is
#: more useful than a table of forty numbers.
QUANTISED_TYPES = {
    2: 'Q4_0', 3: 'Q4_1', 6: 'Q5_0', 7: 'Q5_1', 8: 'Q8_0', 9: 'Q8_1',
    10: 'Q2_K', 11: 'Q3_K', 12: 'Q4_K', 13: 'Q5_K', 14: 'Q6_K', 15: 'Q8_K',
    16: 'IQ2_XXS', 17: 'IQ2_XS', 18: 'IQ3_XXS', 19: 'IQ1_S', 20: 'IQ4_NL',
    21: 'IQ3_S', 22: 'IQ2_S', 23: 'IQ4_XS', 24: 'I8', 25: 'I16', 26: 'I32',
    27: 'I64', 28: 'F64', 29: 'IQ1_M', 31: 'TQ1_0', 32: 'TQ2_0',
    34: 'MXFP4',
}

_LITTLE = sys.byteorder == 'little'

_HALF_TABLE = None


def _half_table():
    """Every IEEE binary16 bit pattern, decoded once.

    There are only 65536 of them, so the table is exact by construction -
    struct.iter_unpack does the subnormals, the infinities and the NaNs.
    Building it costs ~9 ms; decoding through it is a C-level map() over
    the raw halves instead of a per-element generator, which measured
    1.27x faster on a 4 M-value F16 tensor.
    """
    global _HALF_TABLE
    if _HALF_TABLE is None:
        raw = array('H', range(65536))
        if not _LITTLE:
            raw.byteswap()
        _HALF_TABLE = array('f', (t[0] for t in struct.iter_unpack('<e', raw.tobytes())))
    return _HALF_TABLE


class GgufError(Exception):
    """A GGUF file that cannot be trusted. The message is user-facing."""


def type_name(t):
    """Human name for a ggml_type number."""
    if t in DENSE_FLOAT_TYPES:
        return {T_F32: 'F32', T_F16: 'F16', T_BF16: 'BF16'}[t]
    return QUANTISED_TYPES.get(t, 'ggml_type#%d' % t)


class Reader:
    """A bounded little-endian cursor over the file bytes."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        end = self.pos + n
        if end > len(self.data):
            raise GgufError(
                'file ends at %d bytes but %d were needed - it is truncated '
                'or not a GGUF' % (len(self.data), end))
        chunk = self.data[self.pos:end]
        self.pos = end
        return chunk

    def u8(self):
        return self.take(1)[0]

    def scalar(self, fmt, size):
        return struct.unpack(fmt, self.take(size))[0]

    def u32(self):
        return self.scalar('<I', 4)

    def u64(self):
        return self.scalar('<Q', 8)

    def string(self):
        n = self.u64()
        if n > len(self.data):
            raise GgufError('string length %d exceeds the file size' % n)
        return self.take(n).decode('utf-8', 'replace')

    def align_to(self, alignment):
        rem = self.pos % alignment
        if rem:
            self.take(alignment - rem)


def _read_value(r, vtype):
    """One metadata value, as a plain Python object."""
    if vtype == KV_STRING:
        return r.string()
    if vtype == KV_ARRAY:
        elem = r.u32()
        count = r.u64()
        # A hostile file can claim a billion elements in a 12-byte header.
        if count * 4 > len(r.data):
            raise GgufError(
                'metadata array claims %d elements, which cannot fit in %d bytes'
                % (count, len(r.data)))
        return [_read_value(r, elem) for _ in range(count)]
    if vtype not in _KV_SCALAR:
        raise GgufError('unknown metadata value type %d' % vtype)
    fmt, size = _KV_SCALAR[vtype]
    return r.scalar(fmt, size)


class Tensor:
    """One tensor descriptor plus the geometry the converter needs."""

    __slots__ = ('name', 'dims', 'ggml_type', 'offset', 'size_bytes', 'data_start')

    def __init__(self, name, dims, ggml_type, offset, data_start, size_bytes):
        self.name = name
        self.dims = dims
        self.ggml_type = ggml_type
        self.offset = offset
        self.data_start = data_start
        self.size_bytes = size_bytes

    @property
    def n_dims(self):
        return len(self.dims)

    @property
    def n_elements(self):
        n = 1
        for d in self.dims:
            n *= d
        return n

    @property
    def type_name(self):
        return type_name(self.ggml_type)

    @property
    def is_dense_float(self):
        return self.ggml_type in DENSE_FLOAT_TYPES

    def shape(self):
        """(rows, cols) for a 1-D or 2-D tensor; None for anything else."""
        if self.n_dims == 1:
            return (1, self.dims[0])
        if self.n_dims == 2:
            return (self.dims[0], self.dims[1])
        return None

    def as_dict(self):
        return {
            'name': self.name,
            'dims': list(self.dims),
            'type': self.type_name,
            'dense_float': self.is_dense_float,
            'elements': self.n_elements,
            'bytes': self.size_bytes,
        }


class GgufFile:
    """A parsed GGUF: header, metadata and tensor directory."""

    def __init__(self, data, path=None):
        self.data = data
        self.path = path
        self.metadata = {}
        self.tensors = []
        self._mapping = None
        self._parse()

    @classmethod
    def open(cls, path):
        """Map a GGUF instead of reading it into RAM.

        A 4 GB F16 download becomes 4 GB of address space, not 4 GB of
        heap: the converter then reads it one row block at a time. Call
        close() when done, or use it as a context manager.
        """
        fh = open(path, 'rb')
        try:
            size = os.fstat(fh.fileno()).st_size
            if size == 0:
                raise GgufError('file is empty')
            data = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        finally:
            fh.close()
        self = cls(data, path)
        self._mapping = data
        return self

    def close(self):
        if self._mapping is not None:
            self._mapping.close()
            self._mapping = None
            self.data = b''

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()
        return False

    # -- parsing -------------------------------------------------------

    def _parse(self):
        r = Reader(self.data)
        magic = r.u32()
        if magic != GGUF_MAGIC:
            raise GgufError(
                'not a GGUF file (magic is 0x%08x, expected 0x%08x "GGUF")'
                % (magic, GGUF_MAGIC))
        self.version = r.u32()
        if not (GGUF_VERSION_MIN <= self.version <= GGUF_VERSION_MAX):
            raise GgufError(
                'GGUF version %d is not supported (this reader handles %d..%d)'
                % (self.version, GGUF_VERSION_MIN, GGUF_VERSION_MAX))
        n_tensors = r.u64()
        n_kv = r.u64()
        if n_tensors * 48 > len(self.data) or n_kv * 12 > len(self.data):
            raise GgufError(
                'header claims %d tensors and %d metadata entries, which '
                'cannot fit in %d bytes' % (n_tensors, n_kv, len(self.data)))

        for _ in range(n_kv):
            key = r.string()
            self.metadata[key] = _read_value(r, r.u32())

        self.alignment = int(self.metadata.get('general.alignment', 32) or 32)
        if self.alignment <= 0 or (self.alignment & (self.alignment - 1)):
            raise GgufError(
                'general.alignment is %d, which is not a power of two'
                % self.alignment)

        for _ in range(n_tensors):
            name = r.string()
            n_dims = r.u32()
            if n_dims > 4:
                raise GgufError('tensor %r claims %d dimensions' % (name, n_dims))
            dims = [r.u64() for _ in range(n_dims)]
            self.tensors.append(Tensor(name, dims, r.u32(), r.u64(), 0, 0))

        # The data section starts at the next `alignment` boundary.
        r.align_to(self.alignment)
        data_start = r.pos
        for t in self.tensors:
            t.data_start = data_start + t.offset
            width = DENSE_FLOAT_TYPES.get(t.ggml_type)
            if width is not None:
                t.size_bytes = t.n_elements * width
            else:
                # The byte size of a block-quantised tensor depends on its
                # geometry; the block sizes are not derivable here, so the
                # end of the file is the only bound we can state honestly.
                t.size_bytes = None
            if t.size_bytes is not None and t.data_start + t.size_bytes > len(self.data):
                raise GgufError(
                    'tensor %r runs past the end of the file (%d + %d > %d)'
                    % (t.name, t.data_start, t.size_bytes, len(self.data)))

    # -- convenience ---------------------------------------------------

    def find(self, name):
        for t in self.tensors:
            if t.name == name:
                return t
        return None

    def dense_candidates(self):
        """Tensors the converter can actually read, in file order."""
        return [t for t in self.tensors
                if t.is_dense_float and t.shape() is not None]

    def quantised_names(self):
        return [t.name for t in self.tensors
                if not t.is_dense_float and t.shape() is not None]

    def tokens(self):
        """The vocabulary as a list of strings, or [] when absent."""
        for key in ('tokenizer.ggml.tokens', 'tokenizer.ggml.model'):
            v = self.metadata.get(key)
            if isinstance(v, list) and v and all(isinstance(x, str) for x in v):
                return v
        return []

    def architecture(self):
        for key in ('general.architecture', 'general.type'):
            v = self.metadata.get(key)
            if isinstance(v, str):
                return v
        return None

    def summary(self):
        """A JSON-serialisable report, for --json and for the console."""
        return {
            'magic': 'GGUF',
            'version': self.version,
            'alignment': self.alignment,
            'architecture': self.architecture(),
            'tensorCount': len(self.tensors),
            'metadataKeys': sorted(self.metadata.keys()),
            'tokens': len(self.tokens()),
            'denseFloatTensors': [t.name for t in self.dense_candidates()],
            'quantisedTensors': self.quantised_names(),
            'bytes': len(self.data),
        }

    # -- decoding ------------------------------------------------------

    def decode_rows(self, tensor, first_row, n_rows):
        """Decode `n_rows` rows of a dense float tensor.

        Row-major, so rows of a 2-D tensor are contiguous and one row block
        is one byte range. This is what keeps the converter's memory at
        O(block) instead of O(tensor): a 32000x4096 F32 embedding is 524 MB
        if decoded whole, and ~8 MB decoded 512 rows at a time.
        """
        if not tensor.is_dense_float:
            raise GgufError(
                'tensor %r is %s - a block-quantised layout, not dense floats. '
                'Re-export the model in F16 or F32 (llama.cpp: '
                '--convert-f16) and import it again.'
                % (tensor.name, tensor.type_name))
        shape = tensor.shape()
        if shape is None:
            raise GgufError('tensor %r has %d dimensions; only 1-D and 2-D '
                            'tensors can be read' % (tensor.name, tensor.n_dims))
        rows = shape[0]
        if first_row < 0 or n_rows < 0 or first_row + n_rows > rows:
            raise GgufError('rows %d..%d are outside tensor %r (%d rows)'
                            % (first_row, first_row + n_rows, tensor.name, rows))
        width = DENSE_FLOAT_TYPES[tensor.ggml_type]
        cols = shape[1]
        n = n_rows * cols
        start = tensor.data_start + first_row * cols * width
        raw = self.data[start:start + n * width]
        return _decode_raw(tensor.ggml_type, raw, n)

    def decode(self, tensor):
        """Decode a whole dense float tensor. Convenience, not for 4 GB
        files - prefer decode_rows() when the tensor is large."""
        shape = tensor.shape()
        if shape is None:
            raise GgufError('tensor %r has %d dimensions; only 1-D and 2-D '
                            'tensors can be read' % (tensor.name, tensor.n_dims))
        return self.decode_rows(tensor, 0, shape[0])


def _decode_raw(gtype, raw, n):
    if gtype == T_F32:
        out = array('f')
        out.frombytes(raw)
        if not _LITTLE:
            out.byteswap()
        return out
    if gtype == T_F16:
        halves = array('H')
        halves.frombytes(raw)
        if not _LITTLE:
            halves.byteswap()
        return array('f', map(_half_table().__getitem__, halves))
    return _decode_bf16(raw, n)


def _decode_bf16(raw, n):
    """BF16 is the top 16 bits of an F32, so widen it with bit ops.

    Done with slice assignment rather than a Python loop: a 4096x4096
    tensor is 16.7 M values, and a list comprehension over those takes
    seconds where this takes milliseconds.
    """
    halves = array('H')
    halves.frombytes(raw)
    if not _LITTLE:
        halves.byteswap()
    wide = bytearray(4 * n)
    view = memoryview(wide)
    low = halves.tobytes()
    view[0::4] = bytes(n)
    view[1::4] = bytes(n)
    view[2::4] = low[0::2]
    view[3::4] = low[1::2]
    view.release()
    del view
    out = array('f')
    out.frombytes(wide)
    if not _LITTLE:
        out.byteswap()
    return out


def load(path):
    """Map a GGUF from disk. Close it (or use `with`) when done."""
    return GgufFile.open(path)
