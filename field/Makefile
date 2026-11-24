CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: field vortex_lab

field: src/field.c
	$(CC) $(CFLAGS) $< -o $@ -lm

vortex_lab: src/vortex_lab.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: field
	./field

vortex-check: vortex_lab
	./vortex_lab --check

vortex-data: vortex_lab
	./vortex_lab ../artifacts

vortex-plot: vortex-data
	python3 plot_vortex.py

clean:
	rm -f field vortex_lab

.PHONY: all test clean vortex-check vortex-data vortex-plot
