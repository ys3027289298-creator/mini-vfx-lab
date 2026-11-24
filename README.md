# mini-vfx-lab

粒子与特效基础模块，C 实现，每目录独立构建可自测。

| 目录 | 目录内容 |
|---|---|
| `emitter/` | 速率与爆发发射、寿命过期、容量丢弃 |
| `field/` | 重力、线性阻力、涡流力场积分 |
| `trail/` | 按年龄排序的轨迹点、宽度衰减 |
| `explosion/` | 爆炸爆发场景：固定/均匀/指数三种寿命分布，断崖复现与平滑衰减验证 |

分析脚本在 `tools/analyze_explosion.py`（读取 `explosion` 产出的 CSV，
输出衰减曲线、寿命分布 KS 检验、分批快照图，见 `explosion/README.md`）。
