/* ====================================================================
 * hydra_v2_container.h - the `.hydra2` sparse-MoE container (v2).
 *
 * Implements docs/FORMAT-V2.md: 128-byte header, metadata K/V block,
 * 64-byte tensor directory entries (resident trunk region), 48-byte
 * expert directory entries (streamed payloads), deterministic top-k
 * routing, and per-expert streaming reads (one pread + CRC-32 per
 * expert, verified at READ time, not at open - section 5.1.5).
 *
 * Scope of this module (what it is NOT): it is a container. It does not
 * run MoE inference, it does not implement an expert cache, and it does
 * not map the trunk. The trunk is readable through hydra2_read_tensor()
 * (the router weights live there); experts are never resident - they
 * are copied into a caller-owned buffer per read and never kept.
 *
 * Validation is both-ends always (R14/R28, FORMAT-V2.md section 5.1):
 * every offset is checked against its region base AND region end, every
 * count is overflow-checked in 64-bit before the multiply, and
 * file_size is checked against the real size on disk. Every refusal is
 * loud and specific: the message names the field, the value, and the
 * bound (section 5.2) - never a generic "invalid file".
 * ==================================================================== */
#ifndef HYDRA_V2_CONTAINER_H
#define HYDRA_V2_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Container identity (FORMAT-V2.md section 4.1). */
#define HYDRA2_MAGIC_STR      "HYDRA2\0\0"
#define HYDRA2_MAGIC_LEN      8u
#define HYDRA2_HEADER_BYTES   128u
#define HYDRA2_TENSOR_ENTRY_B 64u
#define HYDRA2_EXPERT_ENTRY_B 48u
/* Region offsets (metadata, directories, trunk, experts) must be
 * 16-byte aligned. FORMAT-V2.md says "aligned" without a value; this
 * is the documented implementation decision (recorded there too). */
#define HYDRA2_ALIGN          16u

/* Tensor element type ids (FORMAT-V2.md section 3.2). The first five
 * have decoders somewhere in this repository (GGUF import path, v1);
 * the last three are reserved and MUST be refused by name. */
enum {
    HYDRA2_TERNARY2 = 0x0000u, /* 2 elements/byte, v1 packing          */
    HYDRA2_F16      = 0x0010u, /* 2 bytes                              */
    HYDRA2_BF16     = 0x0011u, /* 2 bytes                              */
    HYDRA2_F32      = 0x0012u, /* 4 bytes                              */
    HYDRA2_Q8_0     = 0x0020u, /* 34/32 bytes (34-byte blocks), /32    */
    HYDRA2_Q4_0     = 0x0030u, /* reserved - refuse by name            */
    HYDRA2_Q4_K     = 0x0031u, /* reserved - refuse by name            */
    HYDRA2_MXFP4    = 0x0032u, /* reserved - refuse by name            */
    HYDRA2_TERNARY2_S64  = 0x0040u, /* ternary2 + per-64-group F32    */
    HYDRA2_TERNARY2_S128 = 0x0041u, /*     + per-128-group F32 scales */
    HYDRA2_VOCAB_UTF8   = 0x0050u,  /* NUL-terminated UTF-8 token     */
    HYDRA2_U16_PAIRS    = 0x0051u   /* strings, one per token id; 4B  */
                                    /* per merge rule (left u16,      */
                                    /* right u16), FORMAT-V2.md 3.2.3 */
};

/* Vocabulary types (2026-10-30, FORMAT-V2.md 3.2.3): role VOCAB (8)
 * tensors. 'vocab' carries the token strings: n_elements token strings
 * in token-id order, each 1..n bytes of UTF-8 followed by ONE NUL byte;
 * the structural rule is n_bytes >= 2 * n_elements (every token needs
 * at least its NUL). 'merges' carries the BPE rules in priority order
 * (rule 0 first): n_elements rules of 4 bytes each, little-endian
 * (left u16, right u16); a model without merges omits the tensor. The
 * container validates structure only; the MoE module parses and
 * validates the payload (NUL walk, UTF-8, merge-target range). */

/* Block-scale types (2026-10-27, FORMAT-V2.md 3.2): the payload is
 * TERNARY2 packing, EXACTLY as TERNARY2 (2 codes/byte); the scales
 * live in a PAIRED tensor named '<name>.scale' with type F32 and
 * n_elements = rows * ceil(cols / group) for a 2-D (rows x cols)
 * tensor (rows = 1 for a 1-D tensor). The inference semantics are
 * hydra_tf_row_dot_scaled(): per-group integer-exact partial dot,
 * then the documented f32 scale FMA. */

/* Tensor roles (FORMAT-V2.md section 4.2). 2026-10-27: the three MoE
 * inference roles 7/8/9 (FORMAT-V2.md 4.2, R12 design) — the reader
 * accepts them; OLD readers refuse files that use them, which is the
 * documented forward-compatibility direction. Role 6 is RESERVED
 * (never assigned) and is refused. */
enum {
    HYDRA2_ROLE_TRUNK         = 0u,
    HYDRA2_ROLE_ROUTER        = 1u,
    HYDRA2_ROLE_EMBEDDING     = 2u,
    HYDRA2_ROLE_NORM          = 3u,
    HYDRA2_ROLE_KV            = 4u,
    HYDRA2_ROLE_SHARED_EXPERT = 5u,
    /* 6 = reserved */
    HYDRA2_ROLE_ATTN          = 7u, /* attn_q/attn_k/attn_v/attn_o per layer */
    HYDRA2_ROLE_VOCAB         = 8u, /* 'vocab' (0x0050 UTF8), 'merges' (0x0051) */
    HYDRA2_ROLE_LM_HEAD       = 9u  /* optional; default = tied embedding */
};

/* Architecture names allowed in v2.architecture (section 3.1).
 * Anything else is refused, with the offending name echoed back. */
#define HYDRA2_ARCH_RECURRENT  "hydra-recurrent-v1"
#define HYDRA2_ARCH_MOE        "moe-transformer-v1"

/* Return codes. Every refusal is a distinct code so a caller (and a
 * test) can name which validation failed. 0 = success. */
enum {
    HYDRA2_OK               =  0,
    HYDRA2_E_ARG            = -1,  /* NULL out / bad caller argument   */
    HYDRA2_E_OPEN           = -2,  /* file could not be opened         */
    HYDRA2_E_SMALL          = -3,  /* file smaller than the header     */
    HYDRA2_E_MAGIC          = -4,  /* bad magic (v1 named explicitly)  */
    HYDRA2_E_VERSION        = -5,  /* version/header_bytes/reserved    */
    HYDRA2_E_FILESIZE       = -6,  /* file_size != real size on disk   */
    HYDRA2_E_HDR_CRC        = -7,  /* header_crc32 mismatch            */
    HYDRA2_E_META_OOB       = -8,  /* metadata region out of bounds    */
    HYDRA2_E_META_BAD       = -9,  /* metadata block content invalid   */
    HYDRA2_E_TENSOR_OOB     = -10, /* tensor dir region out of bounds  */
    HYDRA2_E_TENSOR_BAD     = -11, /* tensor entry field invalid       */
    HYDRA2_E_EXPERT_OOB     = -12, /* expert dir region out of bounds  */
    HYDRA2_E_EXPERT_BAD     = -13, /* expert entry field invalid       */
    HYDRA2_E_META_REQUIRED  = -14, /* required routing metadata missing
                                       or out of range (top_k etc.)   */
    HYDRA2_E_READ           = -15, /* pread failed / short read        */
    HYDRA2_E_CRC            = -16, /* expert CRC-32 mismatch at read   */
    HYDRA2_E_CAP            = -17, /* caller buffer too small          */
    HYDRA2_E_NOMEM          = -18  /* allocation failed (refusal names
                                      the byte count)                  */
};

typedef struct {
    char     name[32];   /* NUL-terminated ASCII (length in name_len) */
    uint16_t role;       /* HYDRA2_ROLE_*                            */
    uint16_t type_id;    /* HYDRA2_* element type                    */
    uint16_t n_dims;     /* 1..4                                     */
    uint16_t _reserved;  /* = 0                                      */
    uint8_t  name_len;   /* strlen of name (validated <= 31)         */
    uint64_t offset;     /* RELATIVE to trunk_offset                 */
    uint64_t n_elements; /* >= 1                                     */
    uint64_t n_bytes;    /* = n_elements * bytes_per_element(type)   */
} hydra2_tensor_entry_t;

typedef struct {
    uint32_t layer;        /* MoE layer index                        */
    uint32_t expert_id;    /* index within that layer                */
    uint64_t offset;       /* RELATIVE to experts_offset             */
    uint64_t length;       /* payload bytes, >= 1                    */
    uint64_t first_element;/* position in the flattened param stream */
    uint32_t type_id;      /* HYDRA2_* element type                  */
    uint32_t crc32;        /* CRC-32 (IEEE 0xEDB88320) of the bytes  */
    uint64_t _reserved;    /* = 0                                    */
} hydra2_expert_entry_t;

/* One parsed metadata key/value pair. Keys are ASCII, values are raw
 * bytes whose width is given by the v2.* semantics (FORMAT-V2.md
 * section 4.4; the reader checks the width of every required key).
 * Key and value bytes live in the container's flat meta blob
 * (meta_blob), at key_off/val_off - one allocation, no sub-pointers,
 * freed together on close. */
typedef struct {
    uint32_t key_off; /* offset of the key bytes in meta_blob */
    uint32_t key_len;
    uint32_t val_off; /* offset of the value bytes in meta_blob */
    uint32_t val_len;
} hydra2_meta_kv_t;

typedef struct {
    int fd;
    /* Validated header fields (FORMAT-V2.md section 4.1). */
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t header_bytes;
    uint64_t file_size;
    uint64_t metadata_offset;
    uint64_t metadata_bytes;
    uint64_t tensor_dir_offset;
    uint64_t tensor_dir_count;
    uint64_t tensor_dir_bytes;
    uint64_t expert_dir_offset;
    uint64_t expert_dir_count;
    uint64_t expert_dir_bytes;
    uint64_t trunk_offset;
    uint64_t trunk_bytes;
    uint64_t experts_offset;
    uint64_t experts_bytes;
    uint32_t header_crc32;
    /* Trunk mapping (FORMAT-V2.md 9.4, 2026-10-27): when the resident
     * trunk region could be mmap'ed, tensor reads serve from trunk_map
     * (offsets are trunk-relative, exactly as the tensor entries'
     * offset field is); trunk_backend names which path is live
     * ("mmap" or "pread" - the documented portable fallback). */
    const uint8_t *trunk_map;
    uint64_t trunk_map_len;      /* mapped bytes (page-aligned extent)  */
    uint64_t trunk_map_base_off; /* file offset the mapping starts at   */
    const char *trunk_backend;
    /* Parsed, validated directories (heap; owned by the container). */
    const hydra2_tensor_entry_t *tensors;
    const hydra2_expert_entry_t *experts;
    hydra2_meta_kv_t *meta;     /* index array (heap; owned) */
    const uint8_t *meta_blob;   /* all key+value bytes (heap; owned) */
    uint32_t meta_count;
    /* Routing config extracted from the metadata (section 4.4). */
    char     architecture[64];
    uint32_t layers;
    uint32_t moe_layers;
    uint32_t top_k;
    uint32_t experts_per_layer;
    uint32_t router_dtype;
    uint64_t expert_bytes_typ;
    uint32_t router_runs_ahead;
} hydra2_container_t;

/* Human-readable names for the element type and tensor role ids
 * (FORMAT-V2.md 3.2 / 4.2). Unknown ids map to "UNKNOWN" - the
 * reader's own refusals already name reserved types, these are for
 * reports (hydra-run --inspect). */
const char *hydra2_type_name(uint16_t type_id);
const char *hydra2_role_name(uint16_t role);

/* Open and validate a .hydra2 file. On success the header is
 * CRC-checked, all region bounds are checked at BOTH ends, both
 * directories are parsed and field-validated, the metadata K/V block
 * is parsed, and the required routing keys are extracted and range
 * checked (top_k missing or out of range is a refusal - guessing a
 * routing set would change which experts are read). Expert payloads
 * are NOT touched: their CRCs are verified when they are read.
 * Returns HYDRA2_OK or a HYDRA2_E_* refusal (with a stderr message
 * naming field/value/bound). */
int hydra2_open(hydra2_container_t *c, const char *path);

/* Release everything hydra2_open() allocated and close the fd. */
void hydra2_close(hydra2_container_t *c);

/* Lookup helpers. Return 0 and set *out on success, HYDRA2_E_ARG if
 * not found (the message names the searched name/layer/expert). */
int hydra2_find_tensor(const hydra2_container_t *c, const char *name,
                       const hydra2_tensor_entry_t **out);
int hydra2_find_expert(const hydra2_container_t *c, uint32_t layer,
                       uint32_t expert_id,
                       const hydra2_expert_entry_t **out);

/* Copy a trunk tensor into a caller buffer (offset is relative to
 * trunk_offset, both ends validated at open). cap >= n_bytes.
 * Returns HYDRA2_OK, HYDRA2_E_CAP, HYDRA2_E_READ. */
int hydra2_read_tensor(hydra2_container_t *c,
                       const hydra2_tensor_entry_t *t,
                       uint8_t *buf, uint64_t cap);

/* Stream ONE expert into a caller buffer: one contiguous pread of
 * exactly e->length bytes, then the stored CRC-32 is verified over
 * those bytes (section 5.1.5: verified at read time). cap >= length.
 * Returns HYDRA2_OK, HYDRA2_E_CAP, HYDRA2_E_READ, HYDRA2_E_CRC. */
int hydra2_read_expert(hydra2_container_t *c,
                       const hydra2_expert_entry_t *e,
                       uint8_t *buf, uint64_t cap);

/* Deterministic top-k routing over raw router logits (section 4.4
 * context; FORMAT-V2.md section 5.2 loudness applies to the
 * refusals here too).
 *
 *   - selects the top_k largest logits; TIES break to the LOWER
 *     expert id, so the result is a pure function of the input,
 *   - sel_idx[k] is filled in descending logit order (ties: ascending
 *     id), sel_w[k] = softmax over the SELECTED logits only
 *     (max-subtracted, double accumulation; weights sum to 1),
 *   - all logits must be finite (NaN/Inf is a refusal - a router
 *     that can produce NaN has a bug upstream, and routing it
 *     silently would paper over that bug).
 * Returns HYDRA2_OK or HYDRA2_E_ARG (message names the failing
 * constraint). top_k == n_experts is valid (selects all). */
int hydra2_route_top_k(const float *logits, uint32_t n_experts,
                       uint32_t top_k,
                       uint32_t *sel_idx, float *sel_w);

/* CRC-32, IEEE, reflected, polynomial 0xEDB88320 (the one the expert
 * directory stores). Public because the fixture tooling and the
 * tests compute it independently of the reader. Known answer:
 * crc32("123456789") == 0xCBF43926, crc32("") == 0x00000000. */
uint32_t hydra2_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* HYDRA_V2_CONTAINER_H */
