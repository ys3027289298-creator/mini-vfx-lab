CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: pause

pause: src/pause.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: pause
	./pause

analyze:
	python3 tools/analyze.py

clean:
	rm -f pause

distclean: clean
	rm -rf recordings/*.csv recordings/metrics.json recordings/analysis_report.txt

.PHONY: all test analyze clean distclean
