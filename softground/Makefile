CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: softground

softground: src/softground.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: softground
	./softground

clean:
	rm -f softground
