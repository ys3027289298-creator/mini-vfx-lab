CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: texalias

texalias: src/texalias.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: texalias
	mkdir -p results
	./texalias

clean:
	rm -f texalias
	rm -rf results/edge_error.csv
