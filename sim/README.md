# sim

帧循环组合层：帧时钟（毫秒 tick）→ `SIM_MS_TO_SEC` 换算成秒 →
emitter → field → trail。三个模块以 `#include` 方式编入同一翻译单元，
标识符用宏重命名避免冲突。

子命令（`./sim <cmd>`）：

| 命令 | 作用 |
|------|------|
| `fall <fps> <frames> <g> euler\|verlet` | 端到端自由落体，输出位移、累计仿真时间、对照 4.9 m 的相对误差 |
| `probe-clock <fps> <frames>` | 环节 1 契约：N 帧累计仿真时间须 = N/fps 秒 |
| `probe-emitter` | 环节 2 契约：dt=1 s 后 age=1 s、rate=10/s 发射 10 粒 |
| `probe-field euler\|verlet` | 环节 3 契约：60×(1/60 s)、g=9.8 须得 4.9 m |
| `probe-trail` | 环节 4 契约：老化 0.5 s 后点 age=0.5 s |

环境变量（审计/归因用，正常运行不需要）：

- `SIM_MS_TO_SEC`：运行时覆盖毫秒→秒换算常数（编译期默认 `0.001`）。
- `SIM_DT_SCALE_EMITTER` / `SIM_DT_SCALE_FIELD` / `SIM_DT_SCALE_TRAIL`：
  单独缩放某一环节收到的 dt，用于定位误差贡献环节。

`make` 构建；`make test` 跑 60 fps / 1 秒自由落体验收测量。
