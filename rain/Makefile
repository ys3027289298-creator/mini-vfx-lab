CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

TIERS ?= 20 40 80

all: rain

rain: src/rain.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: rain
	./rain --check
	@for d in $(TIERS); do ./rain --tier $$d || exit 1; done

curves: rain
	./rain --curves > ../artifacts/rain_curves.csv
	./rain --tiers-csv > ../artifacts/rain_tiers.csv

report: curves
	python3 tools/plot_report.py \
	  ../artifacts/rain_curves.csv \
	  ../artifacts/rain_tiers.csv \
	  ../artifacts/rain_report.png

clean:
	rm -f rain

.PHONY: all test curves report clean
