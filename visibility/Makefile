CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: visibility

visibility: src/visibility.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: visibility
	./visibility

csv: visibility
	./visibility csv > sweep.csv

plot: csv
	python3 tools/plot_curves.py sweep.csv constraint_curves

clean:
	rm -f visibility sweep.csv constraint_curves_light.png constraint_curves_dark.png
