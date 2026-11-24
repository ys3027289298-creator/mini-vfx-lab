#!/usr/bin/env bash
# 端到端验证：
#   1) v1 缺陷版复现：同档两次回放（不同种子）必须分叉
#   2) v2 修复版：录制一次，回放十次，十份帧日志逐帧完全一致，
#      且每份都与原始录制的检查点后帧逐帧一致（含全部槽位与状态头）
#   3) 池空洞：v1 压缩破坏布局，v2 精确还原
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p work
rm -f work/*.save work/*.log

PY="python3 -I tools/compare_frames.py"

echo "########## 构建 ##########"
make >/dev/null

echo
echo "########## 1) v1 缺陷复现：同档回放两次，应分叉 ##########"
./replay record v1 work/v1.save work/v1_record.log >/dev/null
./replay play   v1 work/v1.save work/v1_a.log 111 >/dev/null
./replay play   v1 work/v1.save work/v1_b.log 222 >/dev/null
if $PY work/v1_a.log work/v1_b.log > work/v1_diff.txt; then
    echo "FAIL: v1 两次回放竟然一致，缺陷未复现"; exit 1
fi
head -3 work/v1_diff.txt
echo "OK: v1 两次回放按预期分叉"

echo
echo "########## 2) 池空洞：v1 压缩 vs v2 全量 ##########"
./replay hole-test work > work/hole.txt
tail -1 work/hole.txt
grep -q 'v1_broken=1 v2_exact=1' work/hole.txt || { echo "FAIL: 空洞还原不符预期"; exit 1; }
echo "OK: v1 破坏槽位布局，v2 精确还原"

echo
echo "########## 3) v2 修复版：录制 + 十次回放 ##########"
./replay record v2 work/v2.save work/v2_record.log >/dev/null
echo "--- v2 存档检查点段落 ---"
sed -n '1,6p' work/v2.save

for i in $(seq 1 10); do
    ./replay play v2 work/v2.save "work/v2_run_$i.log" >/dev/null
done

fail=0
echo
echo "--- 十次回放两两/对照基准的逐帧一致性 ---"
# 以第一次回放为基准，其余九次与之逐帧比对
for i in $(seq 2 10); do
    if ! $PY "work/v2_run_1.log" "work/v2_run_$i.log" > "work/cmp_$i.txt"; then
        echo "FAIL: 回放 1 与回放 $i 不一致"; cat "work/cmp_$i.txt"; fail=1
    fi
done

# 每次回放还必须与原始录制的检查点后帧逐帧一致
for i in $(seq 1 10); do
    if ! $PY work/v2_record.log "work/v2_run_$i.log" > "work/rec_cmp_$i.txt"; then
        echo "FAIL: 回放 $i 与原始录制不一致"; cat "work/rec_cmp_$i.txt"; fail=1
    fi
done

if [ "$fail" -ne 0 ]; then exit 1; fi

frame_count=$(grep -c '^frame ' work/v2_run_1.log)
echo "OK: 十次回放两两逐帧一致，且全部与原始录制吻合（每份 $frame_count 帧）"
echo
echo "########## 全部通过 ##########"
