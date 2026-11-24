CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
ART ?= ../artifacts

all: pool

pool: src/pool.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: pool
	./pool test

data: pool
	./pool curves $(ART)/pool_curves.csv
	./pool json $(ART)/pool_summary.json

chart: data
	python3 tools/plot_curves.py $(ART)/pool_curves.csv \
		$(ART)/pool_curves.png $(ART)/pool_curves.svg

clean:
	rm -f pool

.PHONY: all test data chart clean
