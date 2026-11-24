CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: shard

shard: src/shard.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: shard
	./shard

clean:
	rm -f shard
