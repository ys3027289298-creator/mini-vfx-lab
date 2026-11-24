CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: depth

depth: src/depth.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: depth
	./depth

csv: depth
	./depth csv > sweep.csv

json: depth
	./depth json > depth_report.json

plot: csv
	python3 tools/plot_curves.py sweep.csv depth_curves

report: json csv
	cp depth_report.json ../artifacts/depth_report.json
	cp sweep.csv ../artifacts/depth_sweep.csv

runs: depth
	mkdir -p ../artifacts/depth_runs
	./depth run shallow  > ../artifacts/depth_runs/shallow.json
	./depth run standard > ../artifacts/depth_runs/standard.json
	./depth run deep     > ../artifacts/depth_runs/deep.json

clean:
	rm -f depth sweep.csv depth_report.json
	rm -f depth_curves_light.png depth_curves_dark.png
	rm -f ../artifacts/depth_report.json ../artifacts/depth_sweep.csv
	rm -rf ../artifacts/depth_runs

.PHONY: all test csv json plot report runs clean
