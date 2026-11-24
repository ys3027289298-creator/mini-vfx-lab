CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: collision

collision: src/collision.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: collision
	./collision

clean:
	rm -f collision
