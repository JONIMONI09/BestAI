CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -Iinclude
LDFLAGS ?= -lm
NODE ?= node

# M3: AVX2 detection. -mavx2 is added only when (a) the host is x86-64
# and (b) the compiler accepts the flag. A -mavx2 build still runs on
# CPUs without AVX2: src/hydra_simd.h dispatches at runtime, and a plain
# build contains no AVX2 instruction at all. Cross-compilation (the
# ARM CI leg) never matches (a), so it is unaffected.
HYDRA_ARCH := $(shell uname -m 2>/dev/null)
ifeq ($(HYDRA_ARCH),x86_64)
AVX2_OK := $(shell printf 'int main(void){return 0;}\n' | $(CC) -mavx2 -x c - -o $(CURDIR)/.avx2_probe 2>/dev/null && echo yes; rm -f $(CURDIR)/.avx2_probe)
endif
ifneq (,$(strip $(AVX2_OK)))
CFLAGS += -mavx2
endif

.PHONY: all test test-run jni-batch-test large-model-test hydra2-test hydra-moe-test gguf-test gguf-conv-test gpu-bench ui clean

all: hydra-run

# --inspect links the .hydra2 container reader into hydra-run: the
# report is the reader's own verdict (FORMAT-V2.md implementation
# status - inspection was the missing consumer).
hydra-run: src/hydra_engine.c src/main.c src/hydra_v2_container.c src/hydra_moe.c include/hydra_model.h include/hydra_v2_container.h include/hydra_moe.h src/hydra_simd.h src/hydra_neon.h
	$(CC) $(CFLAGS) -o hydra-run src/hydra_engine.c src/main.c src/hydra_v2_container.c src/hydra_moe.c $(LDFLAGS)

test: hydra-test jni-batch-test large-model-test hydra2-test hydra-moe-test

hydra-test: src/hydra_engine.c tests/test_engine.c include/hydra_model.h src/hydra_simd.h src/hydra_neon.h
	$(CC) $(CFLAGS) -o hydra-test src/hydra_engine.c tests/test_engine.c $(LDFLAGS)

# The JNI batching policy (android/app/src/main/cpp/hydra_batch.h) and the
# sampler-configuration validation (android/app/src/main/cpp/hydra_sample_cfg.h)
# are shared between the APK and this test, so the partial-final-batch rule and
# the accept/reject ranges are verified on the host. No device or emulator is
# needed or available for them.
# The transformer (v3/v4) section drives the real engine through the
# JNI contract, so the engine is linked in (cf. hydra2-test).
jni-batch-test: tests/test_jni_batch.c android/app/src/main/cpp/hydra_batch.h android/app/src/main/cpp/hydra_sample_cfg.h src/hydra_engine.c include/hydra_model.h
	$(CC) $(CFLAGS) -Iinclude -o jni-batch-test tests/test_jni_batch.c src/hydra_engine.c $(LDFLAGS)

# Multi-GiB models and 32-bit offset arithmetic. The big file is sparse, so
# this needs neither 5 GiB of disk nor 5 GiB of RAM - that is exactly the
# property being tested.
large-model-test: src/hydra_engine.c tests/test_large_model.c include/hydra_model.h src/hydra_simd.h src/hydra_neon.h
	$(CC) $(CFLAGS) -o large-model-test src/hydra_engine.c tests/test_large_model.c $(LDFLAGS)

# The .hydra2 sparse-MoE container (docs/FORMAT-V2.md): header + metadata +
# directory parsing with BOTH-ENDS validation (R14), deterministic top-k
# routing, and one-pread-per-expert streaming with CRC-32 verified at read
# time. It links the v1 engine too, because the test also verifies the
# mutual v1/v2 refusal (FORMAT-V2.md 7.2/7.3). The Python fixture generator
# is a build dependency: the cross-check below runs against a file it
# writes, so a stale generator cannot silently rot.
hydra2-test: src/hydra_v2_container.c src/hydra_engine.c tests/test_hydra2_container.c include/hydra_v2_container.h tools/make_model.py
	$(CC) $(CFLAGS) -o hydra2-test src/hydra_v2_container.c src/hydra_engine.c tests/test_hydra2_container.c $(LDFLAGS)

# Round 12: the sparse-MoE inference engine on the .hydra2 container
# (src/hydra_moe.c). Its fixtures are complete-contract files from
# make_model.py --moe-full (built by test-run below): the F32 layout
# (argv[1]) plus the ternary + block-scale layouts (argv[2..4],
# stage 5: S64, S128, and a dim-256 S64 file whose gate/up rows span
# four 64-element groups).
hydra-moe-test: src/hydra_moe.c src/hydra_v2_container.c src/hydra_engine.c tests/test_hydra_moe.c include/hydra_moe.h include/hydra_v2_container.h include/hydra_model.h tools/make_model.py
	$(CC) $(CFLAGS) -o hydra-moe-test src/hydra_moe.c src/hydra_v2_container.c src/hydra_engine.c tests/test_hydra_moe.c $(LDFLAGS)

# GGUF -> .hydra conversion. The gate builds a real GGUF, converts it, and
# cross-checks the packed bytes and the C engine's token stream against
# independent reference implementations. It needs ./hydra-run for the
# engine half and says so when it is missing rather than passing quietly.
gguf-test: hydra-run tools/gguf_test.py tools/gguf_to_hydra.py tools/gguf_reader.py
	python3 tools/gguf_test.py --engine ./hydra-run

# Round 12: the GGUF -> .hydra2 converter (M2.2/M3.1/M4). Converts the
# minimal llama GGUF into a complete-contract MoE .hydra2 and cross-checks
# the engine stream, the CRC-protected expert payloads, the exact
# embedding transpose, the exact masked/scaled dense FFN, the embedded
# vocabulary and the merge rules against independent re-reads, with loud
# refusals for non-GGUF / Q4_0 / non-llama / token-less / mismatched
# dimension inputs.
gguf-conv-test: hydra-run tools/gguf_to_hydra2.py tools/gguf_to_hydra2_test.py tools/gguf_reader.py tools/gguf_minimal_llama.py
	python3 tools/gguf_to_hydra2_test.py ./hydra-run

test-run: hydra-test jni-batch-test large-model-test gguf-test gguf-conv-test hydra2-test hydra-moe-test
	./hydra-test
	./jni-batch-test
	./large-model-test
	python3 tools/make_model.py $(CURDIR)/.hydra2-pyfix.hydra2 --v2
	./hydra2-test "$(CURDIR)/.hydra2-pyfix.hydra2"
	rm -f $(CURDIR)/.hydra2-pyfix.hydra2
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix.hydra2 --moe-full --layers 2
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-t64.hydra2 --moe-full --layers 2 --moe-ternary
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-t128.hydra2 --moe-full --layers 2 --moe-ternary 128
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-t64-lg.hydra2 --moe-full --layers 1 --experts 4 --top-k 2 --dim 256 --n-heads 8 --head-dim 32 --inter 64 --moe-ternary 64
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-nv.hydra2 --moe-full --layers 2 --moe-no-vocab
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-at64.hydra2 --moe-full --layers 2 --moe-attn-ternary
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-gqa.hydra2 --moe-full --layers 2 --n-heads 4 --head-dim 4 --n-kv-heads 2
	# Q4_0 (round 16): dim/inter must be multiples of 32; argv[9] is
	# the SAME shape + LCG draws in F32 (quantisation-visibility).
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-q4.hydra2 --moe-full --layers 2 --dim 64 --n-heads 2 --head-dim 32 --inter 64 --experts 4 --top-k 2 --moe-q4
	python3 tools/make_model.py $(CURDIR)/.hydra2-moefix-q4f32.hydra2 --moe-full --layers 2 --dim 64 --n-heads 2 --head-dim 32 --inter 64 --experts 4 --top-k 2
	./hydra-moe-test "$(CURDIR)/.hydra2-moefix.hydra2" "$(CURDIR)/.hydra2-moefix-t64.hydra2" "$(CURDIR)/.hydra2-moefix-t128.hydra2" "$(CURDIR)/.hydra2-moefix-t64-lg.hydra2" "$(CURDIR)/.hydra2-moefix-nv.hydra2" "$(CURDIR)/.hydra2-moefix-at64.hydra2" "$(CURDIR)/.hydra2-moefix-gqa.hydra2" "$(CURDIR)/.hydra2-moefix-q4.hydra2" "$(CURDIR)/.hydra2-moefix-q4f32.hydra2"
	rm -f $(CURDIR)/.hydra2-moefix.hydra2 $(CURDIR)/.hydra2-moefix-t64.hydra2 $(CURDIR)/.hydra2-moefix-t128.hydra2 $(CURDIR)/.hydra2-moefix-t64-lg.hydra2 $(CURDIR)/.hydra2-moefix-nv.hydra2 $(CURDIR)/.hydra2-moefix-at64.hydra2 $(CURDIR)/.hydra2-moefix-gqa.hydra2 $(CURDIR)/.hydra2-moefix-q4.hydra2 $(CURDIR)/.hydra2-moefix-q4f32.hydra2

# GPU feasibility benchmark, CPU reference only. The device build adds
# -DHYDRA_WITH_GLES and links -lEGL -lGLESv2; see tools/gpu_bench/README.md.
# This target exists so CI can prove the file still compiles and the CPU
# path still produces valid JSON on every push - the previous version could
# only be built with an NDK nobody ran in CI, so it silently rotted.
gpu-bench: gpu_bench_cpu
	./gpu_bench_cpu --dim 64 --layers 4 --steps 2000

gpu_bench_cpu: tools/gpu_bench/gpu_bench.c
	$(CC) -O2 -Wall -Wextra -Werror -o gpu_bench_cpu tools/gpu_bench/gpu_bench.c

# Web console: build the starter model and start the server (port via PORT, default 8787)
ui: hydra-run
	mkdir -p models
	python3 tools/make_model.py models/starter.hydra
	$(NODE) server.js

clean:
	rm -f hydra-run hydra-test jni-batch-test large-model-test hydra2-test hydra-moe-test gpu_bench_cpu *.hydra *.hydra2 .speed.gguf .speed.hydra
