CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: blend

blend: src/blend.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: blend
	./blend

clean:
	rm -f blend
