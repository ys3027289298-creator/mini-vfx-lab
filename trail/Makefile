CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: trail

trail: src/trail.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: trail
	./trail

clean:
	rm -f trail
