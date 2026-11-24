CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: plume

plume: src/plume.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: plume
	./plume

clean:
	rm -f plume
