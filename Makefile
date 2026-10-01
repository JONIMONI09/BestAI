CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -Iinclude
LDFLAGS ?= -lm

.PHONY: all test clean

all: hydra-run

hydra-run: src/hydra_engine.c src/main.c include/hydra_model.h
	$(CC) $(CFLAGS) -o hydra-run src/hydra_engine.c src/main.c $(LDFLAGS)

test: hydra-test

hydra-test: src/hydra_engine.c tests/test_engine.c include/hydra_model.h
	$(CC) $(CFLAGS) -o hydra-test src/hydra_engine.c tests/test_engine.c $(LDFLAGS)

test-run: hydra-test
	./hydra-test

clean:
	rm -f hydra-run hydra-test *.hydra
