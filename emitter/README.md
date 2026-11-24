# emitter

`make test` 跑自检（含编译期卡口负样本与连续 20 帧颜色残留检测）。

## 槽位完整性

粒子字段的唯一清单是 [`src/particle_slots.h`](src/particle_slots.h) 中的
`PARTICLE_FIELDS`（X 宏）。判据（Slot Completeness Criterion）：

1. **存储完备**：`Particle` 结构由清单展开生成；绕过清单私加字段会被
   `-Wmissing-field-initializers`（头文件内 pragma 升级为 error）和逐字段
   `offsetof`/`sizeof` 静态断言在编译期拦下，错误直接点出未登记字段名。
2. **重置完备**：每个字段在行尾注释中标明重置步骤（`INIT` / `SPAWN` /
   `DEATH` / `TICK`）。复用唯一入口 `particle_spawn_reset()` 做整槽覆写。
3. **运行时无残留**：自检连续截 20 帧，每帧比较新生粒子颜色与同帧内上一颗
   已死粒子的临终颜色，任一对待即失败（`color_residue_20_frames`）。

新增字段只需在 `PARTICLE_FIELDS` 加一行 `X(...)` 并写清重置步骤；结构、
重置初值、描述符表、编译卡口会自动带上它。若只改 struct 不登记，编译失败；
若重置路径退回逐字段赋值而漏掉颜色，20 帧检测会抓到 `newborn == dead`，
同一份自检中的阳性对照 `residue_check_catches_legacy_bug` 用旧写法标本
证明该检测能抓出全部 20/20 对残留。

`make test-compile-gate` 单独验证“清单外字段必须编译失败”的负样本。
gcc 与 clang 均通过（`make test CC=clang`）。
