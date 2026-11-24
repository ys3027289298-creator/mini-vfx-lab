CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-function

ROOT_ART := $(CURDIR)/../artifacts

all: replay_lab

replay_lab: src/replay_lab.c ../emitter/src/emitter.c ../field/src/field.c ../trail/src/trail.c
	$(CC) $(CFLAGS) src/replay_lab.c -o $@ -lm

test: replay_lab
	mkdir -p $(ROOT_ART)
	./replay_lab $(ROOT_ART)

curves: test
	python3 tools/make_curves.py $(ROOT_ART)

clean:
	rm -f replay_lab

.PHONY: all test curves clean
