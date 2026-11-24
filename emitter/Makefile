CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: emitter

emitter: src/emitter.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: emitter
	./emitter

clean:
	rm -f emitter
