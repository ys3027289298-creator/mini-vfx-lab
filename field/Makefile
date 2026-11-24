CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: field

field: src/field.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: field
	./field

clean:
	rm -f field
