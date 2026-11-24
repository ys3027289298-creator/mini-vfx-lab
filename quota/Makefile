CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: quota

quota: src/quota.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: quota
	./quota

clean:
	rm -f quota
