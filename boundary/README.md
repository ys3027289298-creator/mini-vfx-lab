# boundary

粒子越过发射范围后的边界处理。`make test` 跑单元自测；`src/sim.c` 是
emitter（速率发射/寿命）+ field（阻力/重力积分）+ boundary 的端到端模拟，
四种模式共用同一套参数与 RNG 种子，只有边界行为不同。

## 问题复现（NONE：边界处理为空）

模拟 250 个粒子、运行 10s（寿命 12s，保证只有边界能让粒子消失）：

| 指标 | NONE（现状） |
|---|---|
| 存活粒子 | 250 |
| **存活但已越界** | **201（80.4%）** |
| 最远坐标 | \|c\| = 86.7（发射范围仅 ±4） |

粒子永不消亡，在屏幕上拖成一串小点一直飘到天边，与反馈现象一致。
截图：`../artifacts/boundary/shot_none.png`，越界数随时间单调增长见
`../artifacts/boundary/out_of_box_timeseries.png`。

## 三种边界行为

| 模式 | 行为 | 存活 | 越界存活（每帧） | 边界销毁 | 反弹 |
|---|---|---|---|---|---|
| `BD_KILL` | 越过任一边立即销毁 | 49 | **0** | 201 | 0 |
| `BD_REFLECT` | 墙体反弹（restitution=0.7），位置钳回墙内 | 250 | **0** | 0 | 703 |
| `BD_FADE` | 内侧 1 单位边带内 alpha 随位置线性淡出，硬边处销毁 | 49 | **0** | 201 | 0 |

截图（同参数、同种子、同一时刻 t=10s）：

- `../artifacts/boundary/shot_kill.png`
- `../artifacts/boundary/shot_reflect.png`
- `../artifacts/boundary/shot_fade.png`
- 四宫格对比：`../artifacts/boundary/comparison.png`

## 选型：BD_FADE（默认策略）

三种都满足「越界存活粒子归零」，差异在视觉与资源行为：

1. **KILL**：实现最简单、槽位释放最及时，但粒子在墙上瞬间消失（pop），
   高速粒子流在边界处看起来像被齐刷刷切断。
2. **REFLECT**：视觉上粒子被关在盒内，但**永远不释放槽位**——参考运行中
   250 个粒子全部存活、累计反弹 703 次，重力使它们最终沉降堆积在底墙；
   对持续发射的特效，容量（emitter 的 MAXP）会被慢慢占满并触发丢弃。
3. **FADE**：保留硬清理保证（越过硬边一定销毁，槽位一样释放），同时粒子
   在边带内逐渐透明，退出过程连续；alpha 是**位置的确定性函数**
   `alpha = clamp(到最近墙的距离 / margin)`，不是每帧随机或状态翻转，
   因此结构上不可能闪烁。

结论：默认采用 **`BD_FADE`**。需要硬碰撞语义时用 `BD_REFLECT`，追求最低
成本或粒子本就该短命时用 `BD_KILL`；三者都在 API 中保留，可按场景切换。

## 无闪烁的保证

闪烁的根源通常是 alpha 由逐帧状态驱动（来回翻转或出生帧跳变）。本实现：

- alpha 每帧由当前位置重新计算，同一位置必得同一 alpha；
- 位置每帧最大移动 ≈ 速度 × dt = 4 × 0.02 = 0.08，故相邻帧 alpha 跳变
  上界 ≈ 0.08/margin。实测 500 帧最大跳变 **0.103**（含斜向运动分量），
  验证脚本以 0.11 为上限逐帧断言；
- KILL/REFLECT 的 alpha 恒为 1.0，跳变恒为 0；
- 反射模式另设静止阈值 `rest_eps`：低速触墙直接置零而非反弹，避免粒子
  在墙面抖动（单元测试 `reflect_rests` 覆盖）。

## 验收结果

`tools/verify.py` 对全部 500 帧日志做 21 项断言，结果写入
`../artifacts/boundary/verification.txt`，当前 **ALL_CHECKS_PASSED=1**，
关键项：

- 三种策略每一帧「存活但越界」数均为 0，且任何存活坐标不超过 ±4；
- NONE 的越界数单调增长至 201（bug 基线，防止回归时误判）；
- NONE 与 REFLECT、KILL 与 FADE 的逐帧存活数分别完全一致，证明四种运行
  除边界外输入完全相同，对比有效；
- FADE 末帧所有粒子 alpha 与位置公式逐一吻合，边带内确有淡出中的粒子。

## 复现

```sh
make test                 # 单元自测
make run-sim              # 四种模式端到端模拟，CSV 输出到 ../artifacts/boundary/data
python3 tools/render.py   # 生成截图与对比图
python3 tools/verify.py   # 500 帧断言验证
```
