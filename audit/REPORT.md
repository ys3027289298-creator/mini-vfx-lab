# 量纲一致性审计报告：粒子下落慢 10 倍

## 1. 现象与判据

现象：`g = 9.8`（m/s²）的视觉效果等效于 `0.98`，自由落体一秒位移约为预期的
1/100（0.049 m vs 4.9 m），即链路中时间单位存在 10 倍偏差。

**量纲一致性判据（自动判定，不靠肉眼）**：60 fps 下自由落体 1 秒（60 帧），
零初速度、`g=9.8 m/s²`，解析解为

    d* = ½ g t² = 4.9 m

定义相对误差

    ε = |d − 4.9| / 4.9

判据：**ε ≤ 0.0102**，等价于位移落在 **[4.85, 4.95] m**。
同一判据施加于链路中每一个可独立测量的环节。

## 2. 参数 → 位移全链路时间单位表

| # | 步骤 | 输入时间单位 | 输出/参与量纲 | 代码位置 | 独立探针 |
|---|------|--------------|---------------|----------|----------|
| 0a | 参数 `g=9.8` | —（声明量纲） | m/s² | `sim` 命令行，`field.c` 中以 `g*dt` 消费 | 声明项，无 dt 边界 |
| 0b | 参数 `drag` | —（声明量纲） | 1/s，`expf(-drag*dt)` | `field/src/field.c` | 声明项 |
| 1 | 帧时钟 ms→s | 毫秒 tick | **秒 dt**（60fps 时 0.016667 s） | `sim/src/sim.c: SIM_MS_TO_SEC` | `probe-clock`：60 帧累计须 = 1.0 s |
| 2 | emitter 更新 | dt（s） | age（s）；位移 `x += vx*dt`（m） | `emitter/src/emitter.c: em_update` | `probe-emitter`：dt=1 s 后 age=1 s；rate=10/s 发射 10 粒 |
| 3 | field 积分 | dt（s）；g（m/s²） | v（m/s）、x（m），`x += v*dt + ½a*dt²` | `field/src/field.c: step_verlet` | `probe-field`：60×(1/60 s) 须得 4.9 m |
| 4 | trail 老化 | dt（s） | 点 age（s） | `trail/src/trail.c: tr_age` | `probe-trail`：dt=0.5 s 后 age=0.5 s |

模块（2/3/4）的函数契约都要求 dt 为**秒**；唯一的单位边界在步骤 1：
平台时钟给的是毫秒，必须经 `SIM_MS_TO_SEC` 换算成秒。

## 3. 自动扫描方法（`audit/dim_audit.py`）

1. **逐环节契约扫描**：每个环节用 `sim probe-*` 子命令隔离测量，对照解析
   期望值算 ε，超过 0.0102 即 FAIL。
2. **端到端判据**：`sim fall 60 60 9.8 verlet` 跑完整条 emitter→field→trail
   链路，对 4.9 m 算 ε；审计进程退出码 0 当且仅当判据通过。
3. **误差归因实验 A（逐环节扰动）**：把单个环节的 dt 分别 ×0.1、×10
   （`SIM_DT_SCALE_*` 环境变量），测端到端 ε 的变化量——区分环节是真正在
   位移因果路径上，还是仅仅相邻。
4. **误差归因实验 B（时钟常数扫描）**：把 `SIM_MS_TO_SEC` 依次替换为
   1 / 1e-2 / 1e-3 / 1e-4 / 1e-6，按"消除的端到端误差"排序，定位贡献最大
   的常数。

### 修复前扫描结果（摘要）

    frame clock        FAIL relerr=9.000e-01   [sim/src/sim.c: SIM_MS_TO_SEC]
    emitter update     OK
    field integrator   OK   relerr=0.000e+00
    trail aging        OK
    端到端: d=0.049 m  relerr=9.900e-01
    时钟扫描: 候选 0.001 -> relerr=0.000e+00，消除误差 9.900e-01（最大贡献）

### 可疑点按误差贡献排序（修复前）

| 排名 | 误差贡献 | 可疑点 |
|------|----------|--------|
| 1 | 0.990 | `SIM_MS_TO_SEC` 应为 0.001（ms→s），实际为 0.0001，改后端到端判据立即恢复 |
| 2 | 0.900 | 帧时钟环节自身契约破坏：60 帧累计 0.1 s 而非 1.0 s（同一根因的环节视图） |
| — | 0 | emitter / field / trail 隔离测量全部健康，排除 |

结论：三个模块本身量纲正确，10 倍偏差全部来自帧时钟换算常数。

## 4. 修复内容

1. **根因修复** `sim/src/sim.c`：`SIM_MS_TO_SEC` 由 `0.0001f` 改为 `0.001f`
   （1 ms = 10⁻³ s）。修复后 60 帧累计恰好 1.0 s。
2. **积分器精度修复** `field/src/field.c`：原半隐式 Euler 在 60 Hz 下
   1 秒自由落体给 4.9817 m（ε=1.67%，**超出 [4.85,4.95] 验收带**，因为
   每帧速度先更新、位移用的是末速度，系统性多走一帧）。新增
   `step_verlet`（velocity-Verlet，常加速度下位移解析精确），原 `step`
   保留、原有自检全部不动。Verlet 结果 ε≈2.9e-7。阻力仍按指数衰减
   （半步 kick 对称拆分），涡流按两端加速度平均处理。
3. 新增 `sim/` 组合层（帧时钟 + 三模块串联 + 全部探针子命令）与
   `audit/dim_audit.py` 自动审计；根 Makefile 的 `make test` 一键跑全部
   模块自检、链路审计和验收测量。

## 5. 验收测量（命令原样）

    sim/sim fall 60 60 9.8 verlet

输出：

    fps=60
    frames=60
    g=9.8
    frame_period_ms=16.6667
    clock_dt_s=0.0166667
    sim_time_s=1
    y_m=-4.9
    displacement_m=4.9
    analytic_4p9_m=4.9
    relerr_vs_4p9=2.91941e-07
    integrator=verlet
    alive=1
    trail_points=32

位移 **4.9 m**，落在要求的 **4.85–4.95 m** 区间内（ε ≈ 2.9×10⁻⁷）。

一键复跑：`make test`（含三个模块原自检、全链路审计、上述验收命令）。
