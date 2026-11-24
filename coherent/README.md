# coherent

低频位置相干扰动实验。运行：

```bash
make -C coherent
```

脚本会生成：

- `../artifacts/coherent_verification.json`：三档半径、误差、速度方向标准差与全部推导参数；
- `../artifacts/coherent_trajectories.csv`：抽样粒子轨迹；
- `../artifacts/amplitude_tradeoff.png`：扩散半径与方向保持裕度曲线；
- `../artifacts/coherent_three_runs.png`：三档轨迹和无扰动抛物线对照；
- `../artifacts/sample_scaling_laws.png`：独立样本的 √m 与相干场缩放规律。

验收口径：群体质心周围的 RMS 半径；方向指标用末速度方向的圆标准差（本场景角度不跨越 ±180°，线性标准差数值相同）。
