# emitter

`make test` 跑自检。

除速率/爆发发射、寿命过期与容量丢弃外，粒子还带 `size` 字段：尺寸服从右偏的
Gamma 分布 \(S\sim\Gamma(k,\theta)\)，按目标平均尺寸反解 `theta = mean/k`。
偏斜度由“覆盖面积占比 \(C(k)\)”与“尺寸方差 \(1/k\)”两条曲线的平衡点选为
`k = 2`；三档平均尺寸各跑一次并按实际方差 `< 0.20` 判定是否退化成均匀。

推导、曲线与实测方差见 [SIZE_DISTRIBUTION.md](SIZE_DISTRIBUTION.md)。
