CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: emitter

emitter: src/emitter.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: emitter
	./emitter

lab:
	python3 tools/degrade_lab.py

clean:
	rm -f emitter
