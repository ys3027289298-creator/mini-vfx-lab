CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: splash

splash: src/splash.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: splash
	./splash

clean:
	rm -f splash
