# collision

地面碰撞响应：法向按恢复系数 `e` 反转、切向按摩擦系数 `mu` 衰减，并以
`sleep_speed` 截断弹跳级数（理想模型在 `e > 0` 时弹跳次数无穷，见
`analysis/ANALYSIS.md` 的发散性证明）。

`make test` 跑自检。闭式推导、参数选择与三档入射角数值验证见 `analysis/`。
