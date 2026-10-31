/* hydra_moe.h - the sparse-MoE inference engine on top of the
 * .hydra2 container (FORMAT-V2.md, architecture "moe-transformer-v1",
 * round 12 design, 2026-10-27). Expert layouts:
 *   0 = flat F32 (the contract of round 12 stage 4)
 *   1 = TERNARY2 + per-group F32 scales (stage 5): each expert blob is
 *       [gate payload][gate scales][up payload][up scales]
 *       [down payload][down scales], one block per sub-matrix, where a
 *       (rows x cols) block is the exact TERNARY2 payload (n/2 bytes,
 *       element 2j in the LOW 2 bits of byte j, 2j+1 in bits 2-3,
 *       00=0, 01=+1, 10=-1, 11=0) followed by rows*ceil(cols/group)
 *       F32 scales in row-major [row][group] order (group 64 for type
 *       0x0040, 128 for 0x0041; the last group may be partial). The
 *       engine dequantizes a selected expert to F32 (value = code *
 *       scale, exact in F32) and runs the IDENTICAL F32 expert path -
 *       there is deliberately no separate ternary mat-vec kernel, so
 *       the downstream arithmetic is provably the F32 path's.
 *
 * This module is ADDITIVE: it opens a .hydra2 file through the
 * container reader (mmap trunk + pread fallback), reads the trunk
 * tensors (attention / FFN / embed / norms / vocab), and streams the
 * experts per token. It does not touch the v1-v4 engine paths; the
 * pinned legacy token streams stay bit-identical.
 *
 * MODEL (expert_layout 0 = F32 flat, the contract of this round):
 *
 *   h = embed[token]                                  (dim floats)
 *   for each layer l:
 *     a = RMSNorm(h, attn_norm[l])
 *     q = attn_q[l] * a ; k = attn_k[l] * a ; v = attn_v[l] * a
 *     GQA: q has n_heads heads of head_dim, k/v have n_kv_heads;
 *          head i of q attends to head (i % n_kv_heads) of k/v.
 *     RoPE on q and k at position pos (theta = v2.rope_theta,
 *          default 10000.0).
 *     kv cache per layer: sink (v2.kv_sink, default 4) + ring
 *          (v2.kv_ring, default 128); the CURRENT token is always
 *          attended, the append happens after the attention.
 *     c = causal scaled dot-product attention, head by head
 *          (scale 1/sqrt(head_dim))
 *     h = h + attn_o[l] * concat(c_heads)
 *     b = RMSNorm(h, ffn_norm[l])
 *     router logits r = router[l] * b            (experts_per_layer)
 *     top_k experts (tie -> lower id, the container rule); softmax
 *          weights over the SELECTED logits only
 *     h = h + sum_j w_j * SwiGLU_E(expert_j, b)   (streamed, one
 *          pread per expert)
 *   out = RMSNorm(h, final_norm)
 *   logits = lm_head * out   (or embed^T * out when v2.tied_embedding)
 *
 * The forward function is STATELESS: pos and the KV caches are
 * caller-managed (the same contract as hydra_transformer_forward_step
 * takes for v4), and sampling is a separate function over the REAL
 * per-vocabulary logit vector - which this engine produces and the
 * v1-v4 engines structurally cannot. The v1 sampler
 * (hydra_sample_token) is untouched.
 *
 * Errors follow the container's loud-refusal discipline (5.2): every
 * refusal names the file, the tensor, the key, and the offending
 * value on stderr and returns a negative code.
 */
#ifndef HYDRA_MOE_H
#define HYDRA_MOE_H

#include <stdint.h>
#include "hydra_model.h" /* hydra_kv_cache_t */
#include "hydra_v2_container.h" /* hydra2_container_t */

/* Resolved shape + inference parameters (FORMAT-V2.md 4.4 / R12
 * design). The v2.* keys are ENFORCED by this module, not by the
 * container, which stays structural. */
typedef struct hydra_moe_config {
    uint32_t dim;            /* v2.dim                 */
    uint32_t vocab_size;     /* v2.vocab_size          */
    uint32_t n_heads;        /* v2.n_heads             */
    uint32_t n_kv_heads;     /* v2.n_kv_heads (GQA; 1..n_heads) */
    uint32_t head_dim;       /* v2.head_dim; dim == n_heads*head_dim */
    uint32_t inter;          /* v2.inter (FFN width)   */
    uint32_t layers;         /* v2.layers == container layers       */
    uint32_t experts_per_layer; /* container v2.experts_per_layer   */
    uint32_t top_k;          /* container v2.top_k               */
    uint32_t expert_layout;  /* v2.expert_layout: 0 = F32 flat, 1 = ternary+scales */
    uint32_t ternary_group;  /* layout 1: 64 (0x0040) / 128 (0x0041), else 0 */
    double   rope_theta;     /* v2.rope_theta, default 10000.0    */
    uint32_t kv_sink;        /* v2.kv_sink, default 4             */
    uint32_t kv_ring;        /* v2.kv_ring, default 128           */
    int      tied_embedding; /* v2.tied_embedding (1 = use embed) */
    uint32_t bos_token;      /* v2.bos_token, default 0           */
} hydra_moe_config_t;

/* One resolved trunk tensor: name + F32 buffer + element count. */
typedef struct hydra_moe_tensor {
    const char *name;      /* the required name (for the refusal msg) */
    float *data;           /* n_elements floats, owned by the handle  */
    uint64_t n_elements;
} hydra_moe_tensor_t;

/* Per-layer tensor group (all F32, layout 0). */
typedef struct hydra_moe_layer {
    hydra_moe_tensor_t attn_q, attn_k, attn_v, attn_o;
    hydra_moe_tensor_t attn_norm, ffn_norm;
    hydra_moe_tensor_t router;
    hydra_moe_tensor_t ffn_gate, ffn_up, ffn_down;
} hydra_moe_layer_t;

/* One BPE merge rule in priority order (rule 0 applies first). */
typedef struct hydra_moe_merge {
    uint32_t left;      /* token id                              */
    uint32_t right;     /* token id                              */
    uint32_t target;    /* the id of string(left)+string(right)  */
} hydra_moe_merge_t;

/* The engine handle (public, the repository pattern). */
struct hydra_moe {
    hydra2_container_t container;
    int container_open;
    hydra_moe_config_t cfg;
    hydra_moe_tensor_t embed;
    hydra_moe_tensor_t final_norm;
    hydra_moe_tensor_t lm_head;
    int has_lm_head;
    hydra_moe_layer_t *layers;   /* cfg.layers entries */
    uint64_t expert_f32;         /* 3 * inter * dim per expert */
    size_t expert_bytes;
    uint8_t *expert_scratch;
    float *expert_f32buf;        /* layout 1: dequantized expert (F32) */
    /* Vocabulary (role 8, FORMAT-V2.md 3.2.3) - present when the file
     * carries a 'vocab' tensor; inference works without it. */
    int has_vocab;
    uint8_t *vocab_blob;         /* all NUL-terminated strings (owned) */
    uint32_t *vocab_off;         /* byte offset of token i in the blob */
    uint32_t *vocab_len;         /* string length of token i (no NUL)  */
    int32_t  byte_map[256];      /* single-byte token id, -1 = none    */
    hydra_moe_merge_t *merges;   /* merge_count entries, priority order*/
    uint32_t merge_count;
};
typedef struct hydra_moe hydra_moe_t;



/* Open a .hydra2 file for MoE inference.
 *
 * Validates the container (the reader does that), the architecture
 * ("moe-transformer-v1"), the required v2.* metadata, the existence
 * of EVERY tensor the forward pass needs (named below), their types
 * (F32 for layout 0) and element counts, and the expert geometry
 * (each expert = 3 * inter * dim elements, [gate | up | down]; the
 * layout-1 ternary blob layout is in the file header).
 *
 * Required trunk tensor names:
 *   "embed"                      vocab_size*dim, role EMBEDDING
 *   "attn_q.L", "attn_k.L", "attn_v.L", "attn_o.L"   (dim*dim, role ATTN)
 *   "attn_norm.L", "ffn_norm.L"  (dim, role NORM)
 *   "router.L"                   (experts_per_layer*dim, role ROUTER)
 *   "ffn_gate.L", "ffn_up.L"     (inter*dim, role TRUNK)
 *   "ffn_down.L"                 (dim*inter, role TRUNK)
 *   "final_norm"                 (dim, role NORM)
 *   "lm_head"                    (vocab_size*dim, role LM_HEAD) -
 *                                optional when tied_embedding
 *
 * Returns 0 on success; negative on refusal:
 *   -1  invalid argument
 *   -2  container refused the file (see its stderr)
 *   -3  wrong architecture
 *   -4  missing/invalid v2.* metadata
 *   -5  missing tensor
 *   -6  wrong tensor type or element count
 *   -7  bad shape relation (dim != n_heads*head_dim, n_kv_heads out of
 *       range, inter/dim not positive, ...)
 *   -8  expert geometry mismatch
 *   -9  out of memory.
 */
int hydra_moe_open(const char *path, hydra_moe_t *m);
void hydra_moe_close(hydra_moe_t *m);

/* Copy the resolved config. 0 on success, -1 on bad argument. */
int hydra_moe_config(const hydra_moe_t *m, hydra_moe_config_t *out);

/* Dequantize one ternary expert blob (layout 1, see the file header)
 * into 3*inter*dim F32 values with the SAME flat geometry as layout 0
 * (gate at [0, inter*dim), up at [inter*dim, 2*inter*dim), down at
 * [2*inter*dim, 3*inter*dim)). out must hold that many floats.
 *
 * Exported for the test parity suite (the same pattern as
 * hydra_tf_row_dot_scaled): the C test checks a hand-crafted blob,
 * including a partial last group and a sign-flipped scale, element by
 * element in exact F32. Returns 0 on success, -1 on a bad argument or
 * an odd rows*cols (the TERNARY2 payload cannot represent it).
 */
int hydra_moe_dequant_expert(const uint8_t *blob, float *out,
                             uint32_t inter, uint32_t dim, uint32_t group);

/* Dequantize a flat TERNARY2 trunk tensor payload (the paired
 * '<name>.scale' convention, FORMAT-V2.md 3.2.1) into n F32 values.
 * TERNARY2_S64 = group 64, TERNARY2_S128 = group 128; the last group
 * may be partial. element k holds code(k) * scales[k / group] (0, +1
 * or -1 times the scale). TERNARY2 packing: 2 elements per byte (the
 * even element in the low 2 bits, the odd one in bits 2-3), so n
 * elements occupy n/2 payload bytes. payload must NOT alias out: the
 * packed bytes are read at byte k/2 while the result is written at
 * byte 4k, so an aliased in-place call corrupts the payload from the
 * second element on (round-13, Qodo #3 - moe_get_tensor keeps the
 * packed bytes in a separate temporary buffer for this reason).
 * Exported for the test parity suite, same pattern as
 * hydra_moe_dequant_expert. */
int hydra_moe_dequant_flat(const uint8_t *payload, const float *scales,
                           uint64_t n, uint32_t group, float *out);

/* ------------------------------------------------------------------ */
/* vocabulary + BPE text codec (role 8 tensors, FORMAT-V2.md 3.2.3)   */
/* ------------------------------------------------------------------ */

/* 1 when the file carries a parsed 'vocab' tensor (the text codec is
 * available), 0 otherwise. Inference works in both cases. */
int hydra_moe_has_vocab(const hydra_moe_t *m);

/* Number of BPE merge rules (priority order), 0 when none. */
uint32_t hydra_moe_merge_count(const hydra_moe_t *m);

/* Copy the UTF-8 string of one token (NO trailing NUL) into buf.
 * Returns the byte count on success; -1 when there is no vocab, the
 * token id is out of range, or cap is smaller than the string. */
int hydra_moe_decode_token(const hydra_moe_t *m, uint32_t token,
                           char *buf, uint32_t cap);

/* Encode a UTF-8 string to token ids (strict byte-level BPE):
 *   1. the text must be valid UTF-8;
 *   2. every byte must have a single-byte token (byte map) - an
 *      unmapped byte is a refusal, never a guess;
 *   3. merge rules apply in priority order: the lowest-indexed rule
 *      whose (left, right) pair occurs adjacent is applied to ALL of
 *      its occurrences, then the scan restarts at rule 0 (each rule's
 *      target was validated against the vocabulary at open time);
 *   4. the result must fit max_tokens.
 * tokens_out must hold max_tokens uint32. Returns the token count on
 * success; -1 no vocab, -2 invalid UTF-8 at byte offset (printed on
 * stderr), -3 unmapped byte (offset on stderr), -4 output overflow,
 * -5 bad argument.
 */
int hydra_moe_encode_text(const hydra_moe_t *m, const char *text,
                          uint32_t *tokens_out, uint32_t max_tokens);

/* The real per-vocabulary logit vector for one input token.
 *
 * logits_out must hold cfg.vocab_size floats. pos is the RoPE
 * position (0 for the first token). kv_per_layer: NULL for
 * self-only attention (every token attends to itself only), or a
 * caller-owned array of `layers` hydra_kv_cache_t initialized with
 * the sink/ring sizes of the config (hydra_kv_cache_init); layer l
 * uses entry l (the same contract as the v4 forward step).
 *
 * Returns 0 on success; -1 invalid argument, -16 non-finite
 * intermediate, -17 KV append failed, -18 KV read failed.
 *
 * Uses ONE pread per selected expert per layer (top_k reads); the
 * expert bytes are decoded F32 and mixed with the softmax weights.
 */
int hydra_moe_forward(hydra_moe_t *m, uint32_t token_in,
                      uint32_t pos, float *logits_out,
                      hydra_kv_cache_t *kv_per_layer);

/* Sampler over a REAL logit vector (min-p + repetition penalty),
 * independent of the v1 sampler:
 *
 *   a) repetition penalty over the last `window` recent tokens:
 *      positive logits divided, negative multiplied (sign-preserving,
 *      repeats compound); the penalty walks the WHOLE window (round
 *      13, Qodo #21 - the old cap at `vocab` dropped history beyond
 *      one vocabulary span);
 *   b) numerically stable softmax with scale 1/temperature: the
 *      temperature scales the EXPONENT, exp((v-mx)/T) (round 13,
 *      Qodo #8 - the old exp(v-mx)/T renormalized the factor away,
 *      so every temperature sampled identically);
 *   c) min-p filter: drop P(c) < min_p * p_max, renormalize (p_max
 *      always survives);
 *   d) seeded xorshift64* CDF walk.
 *
 * recent_tokens must hold at least `window` entries (any order - the
 * penalty counts occurrences); pass NULL for window 0.
 * temperature <= 0 or min_p >= 1: GREEDY - argmax, no RNG consumed
 * (rng_state may be NULL on the greedy path; the stochastic path
 * REQUIRES a non-NULL rng_state, round 13, Qodo #19). rng_state is
 * in/out (the caller persists it for reproducibility, the same
 * pattern as the v1 sampler's engine-embedded seed).
 *
 * Returns 0 on success, -1 on bad arguments (logits NULL, vocab 0,
 * temperature < -1, min_p outside [0,1], penalty outside [0.5, 2.0],
 * or a NULL rng_state on the stochastic path).
 */
int hydra_moe_sample(const float *logits, uint32_t vocab,
                     uint32_t *token_out,
                     double temperature, double min_p,
                     double rep_penalty, uint32_t window,
                     const uint32_t *recent_tokens,
                     uint64_t *rng_state);

#endif /* HYDRA_MOE_H */
