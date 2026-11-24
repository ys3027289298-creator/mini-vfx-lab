CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
ART ?= ../artifacts

all: trail

trail: src/trail.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: trail
	./trail

# record the dozen-plus tip coordinates over one full slash
points: trail
	@mkdir -p $(ART)
	./trail points > $(ART)/tip_points.csv

# dump geometry, render diagnostic/overlay figures, run containment gate
viz: trail points
	@mkdir -p $(ART)
	./trail dump > /tmp/trail_dump.json 2>/dev/null
	python3 tools/render.py /tmp/trail_dump.json $(ART)

# full-swing overlay animation
gif: trail
	@mkdir -p $(ART)
	./trail dump > /tmp/trail_dump.json 2>/dev/null
	python3 tools/make_gif.py /tmp/trail_dump.json $(ART)/overlay_swing.gif

clean:
	rm -f trail
