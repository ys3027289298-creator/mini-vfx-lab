# Whole-chain dimensional audit + module self-tests.
CC ?= gcc

.PHONY: all test audit clean

all:
	$(MAKE) -C field
	$(MAKE) -C emitter
	$(MAKE) -C trail
	$(MAKE) -C sim

test: all
	$(MAKE) -C field test
	$(MAKE) -C emitter test
	$(MAKE) -C trail test
	python3 -I audit/dim_audit.py sim/sim
	@echo "--- acceptance: 60fps free fall, 1 second ---"
	sim/sim fall 60 60 9.8 verlet

audit: all
	python3 -I audit/dim_audit.py sim/sim

clean:
	$(MAKE) -C field clean
	$(MAKE) -C emitter clean
	$(MAKE) -C trail clean
	$(MAKE) -C sim clean
