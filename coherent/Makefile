PYTHON ?= python3
OUT ?= ../artifacts

.PHONY: all data test clean

all test data:
	$(PYTHON) coherent_lab.py --out $(OUT)

clean:
	rm -f $(OUT)/coherent_verification.json \
	      $(OUT)/coherent_trajectories.csv \
	      $(OUT)/amplitude_tradeoff.png \
	      $(OUT)/coherent_three_runs.png \
	      $(OUT)/sample_scaling_laws.png
