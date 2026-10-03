CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -Iinclude
LDFLAGS ?= -lm
NODE ?= node

.PHONY: all test test-run jni-batch-test large-model-test gguf-test gpu-bench ui clean

all: hydra-run

hydra-run: src/hydra_engine.c src/main.c include/hydra_model.h
	$(CC) $(CFLAGS) -o hydra-run src/hydra_engine.c src/main.c $(LDFLAGS)

test: hydra-test jni-batch-test large-model-test

hydra-test: src/hydra_engine.c tests/test_engine.c include/hydra_model.h
	$(CC) $(CFLAGS) -o hydra-test src/hydra_engine.c tests/test_engine.c $(LDFLAGS)

# The JNI batching policy (android/app/src/main/cpp/hydra_batch.h) is shared
# between the APK and this test, so the partial-final-batch rule is verified
# on the host. No device or emulator is needed or available for it.
jni-batch-test: tests/test_jni_batch.c android/app/src/main/cpp/hydra_batch.h
	$(CC) $(CFLAGS) -Iinclude -o jni-batch-test tests/test_jni_batch.c $(LDFLAGS)

# Multi-GiB models and 32-bit offset arithmetic. The big file is sparse, so
# this needs neither 5 GiB of disk nor 5 GiB of RAM - that is exactly the
# property being tested.
large-model-test: src/hydra_engine.c tests/test_large_model.c include/hydra_model.h
	$(CC) $(CFLAGS) -o large-model-test src/hydra_engine.c tests/test_large_model.c $(LDFLAGS)

# GGUF -> .hydra conversion. The gate builds a real GGUF, converts it, and
# cross-checks the packed bytes and the C engine's token stream against
# independent reference implementations. It needs ./hydra-run for the
# engine half and says so when it is missing rather than passing quietly.
gguf-test: hydra-run tools/gguf_test.py tools/gguf_to_hydra.py tools/gguf_reader.py
	python3 tools/gguf_test.py --engine ./hydra-run

test-run: hydra-test jni-batch-test large-model-test gguf-test
	./hydra-test
	./jni-batch-test
	./large-model-test

# GPU feasibility benchmark, CPU reference only. The device build adds
# -DHYDRA_WITH_GLES and links -lEGL -lGLESv2; see tools/gpu_bench/README.md.
# This target exists so CI can prove the file still compiles and the CPU
# path still produces valid JSON on every push - the previous version could
# only be built with an NDK nobody ran in CI, so it silently rotted.
gpu-bench: gpu_bench_cpu
	./gpu_bench_cpu --dim 64 --layers 4 --steps 2000

gpu_bench_cpu: tools/gpu_bench/gpu_bench.c
	$(CC) -O2 -Wall -Wextra -Werror -o gpu_bench_cpu tools/gpu_bench/gpu_bench.c

# Weboberflaeche: Starter-Modell bauen und Server starten (Port via PORT, Default 8787)
ui: hydra-run
	mkdir -p models
	python3 tools/make_model.py models/starter.hydra
	$(NODE) server.js

clean:
	rm -f hydra-run hydra-test jni-batch-test large-model-test gpu_bench_cpu *.hydra .speed.gguf .speed.hydra
