
# smoke — 十秒烟雾轨迹录制 + 力贡献诊断
#
# make          构建录制器
# make before   只录诊断前基线 -> ../artifacts/before_10s.csv
# make data     构建并跑完整分析（录制前/后两段 + 出图 + analysis.json）

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PY ?= python3

ART := ../artifacts

.PHONY: all before after data animate clean

all: record

record: src/record.c ../field/src/field.c
	$(CC) $(CFLAGS) src/record.c -o $@ -lm

before: record params_before.json
	$(PY) record_run.py params_before.json $(ART)/before_10s.csv

after: record params.json
	$(PY) record_run.py params.json $(ART)/after_10s.csv

data: record
	$(PY) analyze.py

animate:
	$(PY) animate.py

clean:
	rm -f record
