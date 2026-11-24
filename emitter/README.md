# emitter

`make test` 跑自检；`make lab` 跑三档关闭时机、曲线选档和故障反例；
`make curves` 输出完整的停机曲线 CSV。

## 上界

在关闭时刻 `t_off`，设存量粒子 `i` 的年龄、寿命和剩余寿命分别为
`a_i`、`τ_i`、`r_i = τ_i - a_i`。若所有寿命满足 `τ_i ≤ Lmax`，则

```text
r_i ≤ τ_i ≤ Lmax
T_clear = max_i(r_i) ≤ Lmax
```

存量的粒子·秒总量另有上界 `Σ r_i ≤ N_off · Lmax`，但“画面上粒子数清零”
的墙钟时长上界是 `Lmax`，不是 `N_off · Lmax`。

若实测 `T_clear` 明显大于 `Lmax`，多出的时间只能来自关闭后出生的粒子。
对于仍拥有完整寿命 `Lmax` 的最晚偷渡粒子，偷发射窗口约为
`Δ ≈ T_clear - Lmax`。更一般地，关闭后的发射源积分必须满足

```text
S_post = Σ burst_post + floor(rate_acc_off + ∫rate(t)dt)
T_clear ≤ Lmax + sup{t > t_off : 该时刻仍有粒子出生}
```

要让右侧不超过 `Lmax`，必须让 `sup` 为空、`Δ = 0`，也就是关闭后发射源
恒为零。关闭时必须同时置零所有发射源：

```c
e->active = 0;        /* 发射总闸：rate 与 burst 都被 gate */
e->rate = 0.0f;       /* 连续发射速率 */
e->rate_acc = 0.0f;   /* 不足 1 个粒子的累积零头 */
e->burst_pending = 0; /* 已排队的爆发数；当帧 burst 实参也应传 0 */
```

只清 `rate_acc` 而不清总闸和速率，调用方继续传入非零 `rate` 时仍会发射。

## 关闭策略曲线

`em_shutdown(e, h)` 先停止发射，再把剩余寿命超过 `h` 的粒子压到 `h`：

- `h = 0`：立即清空，`D = 0`，但一帧消失比例 `A = 1`；
- `h = Lmax`：不压缩寿命，自然消亡，`D = Lmax`，单帧消失比例最低；
- 中间 `h`：加速尾段消亡，但在统一截止帧形成批量消失。

诊断程序在发射器副本上扫描 `h = 0..Lmax`，得到两条曲线：

```text
D(h) = 清零步数 × dt
A(h) = 关闭过程中最大单帧消失粒子数 / N_off
```

当前策略是在 `A(h) ≤ 0.25`（任何一帧最多消失四分之一存量）的候选中选
`D(h)` 最小的一档。三档关闭时机的实测结果：

| 时机 | `t_off` | `N_off` | `Lmax` | 选定 `h*` | 清零时长 | 关闭后发射数 | 结论 |
|---|---:|---:|---:|---:|---:|---:|---|
| early | 0.50s | 10 | 2.00s | 1.95s | 1.95s | 0 | PASS |
| middle | 1.20s | 24 | 2.00s | 1.75s | 1.75s | 0 | PASS |
| late | 3.00s | 40 | 2.00s | 1.55s | 1.55s | 0 | PASS |

越早关闭，粒子年龄越接近，轻微压缩就会让大批粒子在同一帧消失，因此
`h*` 更接近自然寿命；稳态时年龄分布均匀，可以更激进地缩短尾段。

## 偷渡发射编号

每个粒子有单调 `id` 和出生时刻 `born`。若清零时长超过 `Lmax`，诊断会在
首次越界时收集仍存活且 `born > t_off` 的编号。

内置反例在 middle 档只清 `rate_acc`、未关 `active`，并继续发射 2.50s：

```text
observed_clear = 4.50s, upper_bound = 2.00s, post_emissions = 50
alive IDs at first bound violation = 26,27,...,65
complete post-off spawn ID log      = 25,26,...,74
```

自检把该反例作为“必须检测到编号”的负向用例；正常三档则要求
`clear_time ≤ Lmax` 且关闭后发射数为 0。

固定步长实验使用 `dt = 0.05s`，`Lmax` 为其整数倍。任意步长下离散采样可能
有至多一帧的判定误差；实际引擎应在规划 `h` 时按真实 `dt` 仿真，或把截止
点向下取到帧边界。
