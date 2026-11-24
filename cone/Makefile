CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: cone

cone: src/cone.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: cone
	./cone

clean:
	rm -f cone
