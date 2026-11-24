CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
OUT ?= ../artifacts/explosion_run

all: explosion

explosion: src/explosion.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: explosion
	./explosion

# CSV runs for every lifetime mode, one subdirectory each
run: explosion
	mkdir -p $(OUT)/fixed $(OUT)/uniform $(OUT)/exp
	./explosion fixed   $(OUT)/fixed
	./explosion uniform $(OUT)/uniform
	./explosion exp     $(OUT)/exp

clean:
	rm -f explosion
