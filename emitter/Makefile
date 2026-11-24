CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: emitter bench audit_query

emitter: src/emitter.c src/emitter.h
	$(CC) $(CFLAGS) src/emitter.c -o $@ -lm

bench: src/bench.c src/emitter.c src/emitter.h
	$(CC) $(CFLAGS) -DEMITTER_NO_MAIN src/bench.c src/emitter.c -o $@ -lm

audit_query: src/audit_query.c src/emitter.c src/emitter.h
	$(CC) $(CFLAGS) -DEMITTER_NO_MAIN src/audit_query.c src/emitter.c -o $@ -lm

test: emitter bench audit_query
	./emitter
	./bench
	./audit_query

clean:
	rm -f emitter bench audit_query
