# blend

正常 source-over 与叠加 additive 混合律、相对亮度判别，以及重叠中心过曝上限。

`make test` 跑自检。

要点（实验与图见 `../artifacts/blend-lab/REPORT.md`）：

- 正常：`Y = b(1-a) + s·a`，相对增亮 `ΔY = a(s-b)`，随背景 `b` 升高单调下降。
- 叠加：HDR 累加 `H = b + s·a`，未钳位时 `ΔY = s·a` 与背景无关；8-bit 显示为 `min(s·a, 255-b)`。
- 多颗重叠中心峰值上限 `OVERLAP_CEIL = 235`（BT.601/709 基准白，距白点 255 留 20 码余量），保证不出现 `Y==255` 死白圆斑。
