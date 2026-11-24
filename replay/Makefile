CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: replay

replay: src/replay.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: replay
	bash verify.sh

clean:
	rm -f replay work/*.save work/*.log
