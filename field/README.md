# field

`make test` 跑原有力场自检。

涡流半径/切向速度标定实验：

```sh
make vortex-data   # 生成 ../artifacts/vortex_*.csv/json
make vortex-plot   # 额外生成 ../artifacts/vortex_curves.png
```

实验模型、公式推导和三档直径的粒子编号结果见
`../artifacts/vortex_report.md`。
