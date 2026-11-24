# replay

参数/状态归类与三档存档回放实验。`make test` 跑实验并校验正式三档全部 100% 一致（不达标退出非零），`make curves` 生成曲线图。

```sh
make test      # 输出三档体积/一致率、分叉量名、64 组边界扫描
make curves    # 生成 ../artifacts/replay_curves.png
```

`src/replay_lab.c` 以 unity-build 直接编译 `../emitter`、`../field`、`../trail` 的真实源码（仅重命名自测 `main/check`），回放跑的是同一套更新函数。完整清单、唯一性判据与结论见 [`../artifacts/REPLAY_REPORT.md`](../artifacts/REPLAY_REPORT.md)。

三档（24 片段 × 81 比对帧，实测全部 24/24）：

| 档 | 内容 | B/片段 |
|---|---|---:|
| `S_origin` | 参数 + field 初始条件 + 从规范零点起的完整输入流 | 607 |
| `M_compact` | 参数 + 紧凑切点状态 `mask=0x3D`（`rate_acc`/计数/粒子槽/轨迹/场粒子） | 1133 |
| `L_full` | `MAXP/MAXT` 定长全叶子快照 | 2541 |

另含两个预期分叉的朴素探针（仅参数、参数+标量隐状态），由字段级 diff 自动报出导致分叉的量名。
