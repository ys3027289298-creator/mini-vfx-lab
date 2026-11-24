CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: substep_bench

substep_bench: src/substep_bench.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: substep_bench
	./substep_bench test

clean:
	rm -f substep_bench
