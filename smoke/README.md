# smoke — 烟雾力场诊断场景

用 `field` 模块的同一个积分器录制 10 秒粒子轨迹，量化重力 / 阻力 / 涡流三项对
每帧速度变化的贡献，定位「下拽、散得快」的大头参数并验证修复。完整报告见
[`../artifacts/report.md`](../artifacts/report.md)。

## 一键复现

```bash
make data       # 编译录制器 -> 录 before/after 两段 CSV -> 贡献分析 + PNG + analysis.json
make animate    # 可选：生成 artifacts/quiver_playback.mp4|.gif 逐帧箭头回放
```

依赖：`gcc`、`python3` + `numpy` + `matplotlib`；动画另需 `ffmpeg`。

## 文件

| 文件 | 作用 |
|---|---|
| `src/record.c` | 确定性轨迹录制器。`#include "../../field/src/field.c"` 直接复用原生 `step()`，额外拆出三项 Δv 写 CSV，并校验拆分与 `step()` 一致 |
| `record_run.py` | 读 params JSON，拼参数调用 `./record` |
| `analyze.py` | 录两段数据、算贡献占比/方向/扩散指标、出全部图和 `artifacts/analysis.json` |
| `animate.py` | 全部 600 帧的并排箭头回放（mp4/gif） |
| `params_before.json` | 别人随手填的基线：`g_y=-16, drag=0.5, swirl=0.45` |
| `params.json` | 当前参数；诊断后只把占大头的 `gravity_y` 改成 `+1.4`（向上净浮力） |

## 方法要点

- 坐标 `+y` 朝上；dt = 1/60 s，600 步 = 10 s；固定随机种子，结果可逐位复现。
- 贡献主口径：各项逐帧速度增量模长总和 `Σ|Δv_term|` 的占比；
  另给 RMS 占比与「逐帧最大单项」频率交叉验证。
- 箭头图统一 `angles='xy' + scale_units='xy' + 等纵横比`，方向不被坐标轴拉伸。
- 只允许改三个力场标量（竖直重力 `g_y`、阻力、涡流）中的一个；
  `analysis.json -> force_changes` 会自动记录实际变化的键。
