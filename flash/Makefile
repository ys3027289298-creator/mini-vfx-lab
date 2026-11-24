CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: flash

flash: src/flash.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: flash
	./flash

curves: flash
	./flash --curves > curves.csv
	./flash --trace > trace.csv

plot: curves
	python3 plot_flash.py

clean:
	rm -f flash curves.csv trace.csv flash_tuning.png
