# collision substeps

地面碰撞的子步（连续碰撞检测的离散近似）实验。

```sh
make test
make
./substep_bench study --fps 15 --samples 2000000 \
  --verify-trials 10000000 --particles 500000 \
  --frames 120 --rounds 9
```

## 关系

一帧时长 `dt = 1/f`，最大速度 `v_max`，粒子半径 `r`。拆成 `N` 个等长子步后，最大子步位移为

```text
Δs_sub = v_max · dt / N
```

要求严格满足 `Δs_sub < r`，因此

```text
N > λ,        λ = v_max · dt / r = v_max / (f · r)
N_zero = floor(λ) + 1
```

若允许 `Δs_sub ≤ r`，整数下界可写成 `ceil(λ)`；当 `λ` 恰为整数时，它不满足题目要求的“小于半径”，所以这里采用 `floor(λ)+1`。

## 目标穿透率

仓库实验使用可复现的均匀速度压力模型 `V ~ Uniform(0, v_max)`，并把粒子放在最不利的接触相位。穿透率为

```text
p_pen(N) = max(0, 1 - N/λ)
```

给定目标穿透率 `ε > 0`：

```text
N_ε = ceil(λ(1 - ε))
```

真实速度分布未知时，应使用速度分位数：

```text
N_ε = ceil(dt · v_{1-ε} / r)
```

其中 `P(V > v_{1-ε}) = ε`。有限样本中的零穿透不能证明真实概率为零；`ε=0` 必须使用上面的有界最大速度公式做确定性保证，再用大规模实验复核。

完整三档帧率结果、CSV 和图见 `../../artifacts/substep-study/results.md`。
