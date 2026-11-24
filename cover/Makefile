CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all test csv report clean

all: cover

cover: src/cover.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: cover
	./cover

csv: cover
	mkdir -p ../artifacts/cover
	./cover csv > ../artifacts/cover/cover_sweep.csv
	./cover frames-csv > ../artifacts/cover/cover_frames.csv

report: cover
	mkdir -p ../artifacts/cover
	./cover > ../artifacts/cover/cover_report.txt
	$(MAKE) csv

clean:
	rm -f cover
