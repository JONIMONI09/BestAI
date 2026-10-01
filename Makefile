CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -Iinclude
LDFLAGS ?= -lm
NODE ?= node

.PHONY: all test ui clean

all: hydra-run

hydra-run: src/hydra_engine.c src/main.c include/hydra_model.h
	$(CC) $(CFLAGS) -o hydra-run src/hydra_engine.c src/main.c $(LDFLAGS)

test: hydra-test

hydra-test: src/hydra_engine.c tests/test_engine.c include/hydra_model.h
	$(CC) $(CFLAGS) -o hydra-test src/hydra_engine.c tests/test_engine.c $(LDFLAGS)

test-run: hydra-test
	./hydra-test

# Weboberflaeche: Demo-Modell bauen und Server starten (Port via PORT, Default 8787)
ui: hydra-run
	mkdir -p models
	python3 tools/make_dummy_model.py models/demo.hydra
	$(NODE) server.js

clean:
	rm -f hydra-run hydra-test *.hydra
