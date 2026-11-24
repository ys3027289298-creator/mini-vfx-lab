# emitter

`make test` 跑自检。

## 暂停 / 恢复

`em_update(e, dt, rate, burst)` 保持原语义。暂停感知的循环使用

```c
em_update_ex(e, dt, rate, burst, pause_span, pay_cap);
```

- `dt` 只放本帧一个步长：恢复时先把时间戳重定基，**不要**把暂停间隔当 `dt`
  传进来，否则旧粒子会瞬移、`rate_acc` 也会一口累积出整段发射量。
- `pause_span` 是暂停的 wall 秒数，只记入 `debt` / `debt_frac` 发射欠账，
  不进入任何位置或寿命积分。
- `pay_cap` 限制每帧最多偿还几个欠账粒子，避免恢复时一次性倾泻；池满时按
  既有 `dropped` 计数显式核销。

完整的复现、跳变测量与验收见 [`../pause/`](../pause/README.md)。
