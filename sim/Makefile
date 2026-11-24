CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: sim

sim: src/sim.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: sim
	./sim fall 60 60 9.8 verlet

clean:
	rm -f sim
