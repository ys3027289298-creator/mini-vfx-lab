CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: boundary

boundary: src/boundary.c
	$(CC) $(CFLAGS) src/boundary.c -o $@ -lm

sim: src/sim.c src/boundary.c
	$(CC) $(CFLAGS) src/sim.c -o $@ -lm

test: boundary
	./boundary

run-sim: sim
	mkdir -p ../artifacts/boundary/data
	for m in none kill reflect fade; do \
		./sim $$m ../artifacts/boundary/data/$$m; \
	done

clean:
	rm -f boundary sim
