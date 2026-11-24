CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: emitter

emitter: src/emitter.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: emitter
	./emitter

lab: emitter
	./emitter --lab

curves: emitter
	./emitter --curves

clean:
	rm -f emitter

.PHONY: all test lab curves clean
