# visibility

按局部背景亮度逐粒子决定发光亮度的对比度模块。解决的问题：粒子在强光背景上
被淹没、在暗背景上又过曝成白点，而背景亮度在画面各处不同——不能用一个全局
亮度抬升同时处理两端。

## 对比度推导

线性光、叠加式发光精灵，亮度归一化到 [0,1]：

- 显示像素亮度 `L_d = L_b + w·L_p`，`L_b` 为局部背景，`L_p` 为粒子亮度，
  `w` 为精灵衰减权重（3×3，中心 1.0 / 边 0.5 / 角 0.25）。
- Weber 对比度

      C(L_p, L_b) = (L_d - L_b) / L_b = L_p / L_b

  随 `L_b` 单调下降，这正是"同一粒子换个背景就看不见"的数学形式。
- 理想适应下的实验室可辨阈值 `C_JND = 0.02`（明视觉 Weber 分数）：

      可见  <=>  L_p >= c · L_b

- 固定粒子亮度时，保持可见的**背景亮度上界**（越过即低于人眼阈值）：

      L_b,max = L_p / c

- 反解粒子侧，得到每粒子的**最低亮度地板**：

      L_p,min(L_b) = c · L_b

实际渲染按测量到的局部背景 `L̂_b`（含 ±15% 1σ 估计误差）逐粒子求地板：

    L_p = gain · max(L_base, c · L̂_b)

暗背景上地板很低，暗余烬保持自身亮度不被抬升；只有落在亮背景上的粒子被补
地板。对照的 uniform 策略拿不到逐粒子背景，只能按亮档最差背景（0.95）设一个
全局地板，结果暗场景粒子被一起抬向削波。

### 两个对比度档位

- `C_JND = 0.02`：大视场、稳态、理想适应下的实验室阈值，驱动解析界；
- `C_TARGET = 0.55`：小而瞬态的运动粒子在非均匀眩光下的实用超阈值对比度
  （小目标 Weber 分数的经验区间在 0.5–1.0，远高于大视场 JND），驱动 sweep
  的可见率判据。

## 过曝判据与约束

有效高光余量取 `L_clip = 0.60`（tonemap 后超过该值显示饱和为白点）。
像素满足 `L_b + w·L_p >= L_clip` 计为过曝，过曝率 = 过曝精灵像素 / 全部精灵像素。

三档背景各 400 个确定性随机粒子（暗 0.02–0.10 / 中 0.25–0.55 / 亮 0.70–0.95），
发射体为 11% 高亮火花 + 89% 暗余烬。曝光 gain 在 [1, 6] 对数扫 49 点，两条硬约束：

- 亮档可见率 ≥ **0.70**
- 暗档过曝像素 ≤ 总数的 **0.02**

## 结果

`make test` 自检输出（节选）：

    chosen_gain=1.0775
    dark:   visibility=0.9900 overexposure=0.0125 boosted=0.0400
    mid:    visibility=0.8325 overexposure=0.2694 boosted=0.7250
    bright: visibility=0.7725 overexposure=1.0000 boosted=0.9150
    bright_visibility_met=1
    dark_overexposure_met=1
    not_all_brightened=1
    uniform_has_no_window=1
    uniform_min_dark_over=0.0386 at gain=1.0000 (bright vis there 1.0000)

- 自适应策略在 gain≈1.078 取得满足两条硬约束的最小增益：亮档可见率 77.3%
  （≥70%），暗档过曝 1.25%（≤2%），暗档仅 4% 的粒子需要补地板。
- uniform 策略不存在可行窗口：它在最低 gain 处亮档可见率已是 100%，但暗档
  过曝 3.86% 已超线，且随 gain 单调上升——"一起调亮"无法同时满足两端。

![constraint curves](constraint_curves_light.png)

左：亮档可见率随 gain；右：暗档过曝像素占比随 gain。虚线为两条硬约束，
点线为自适应策略的最小可行 gain。暗色背景版本见
`constraint_curves_dark.png`。

## 用法

    make test   # 自检（含对比度数学单测、单调性、两条硬约束、确定性）
    make csv    # 输出全 sweep 数据到 sweep.csv
    make plot   # 生成 constraint_curves_{light,dark}.png（需要 matplotlib）
    make clean
