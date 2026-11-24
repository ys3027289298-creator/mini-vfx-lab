CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: emitter consistency

emitter: src/emitter.c
	$(CC) $(CFLAGS) $< -o $@ -lm

consistency: src/consistency.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: emitter consistency
	./emitter
	./consistency

clean:
	rm -f emitter consistency
