CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
LDFLAGS ?= -lm

all: cascade

cascade: src/cascade.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

test: cascade
	./cascade selftest
	./cascade verify
	./cascade bench

analyse: cascade
	./cascade analyse

clean:
	rm -f cascade

.PHONY: all test analyse clean
