CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-function

all: run_experiment

run_experiment: run_experiment.c ../src/trail.c
	$(CC) $(CFLAGS) run_experiment.c -o $@ -lm

run: run_experiment
	./run_experiment

clean:
	rm -f run_experiment coef_sweep.csv
