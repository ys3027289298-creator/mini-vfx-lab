CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: bench

bench: bench.c particles.h
	$(CC) $(CFLAGS) bench.c -o bench -lEGL -lGLESv2 -lm

run: bench
	./bench

clean:
	rm -f bench
