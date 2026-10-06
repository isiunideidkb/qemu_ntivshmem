# 03: Dispatch DIALOG 握手

**What to build:** 第二个 DIALOG。client 等 host 把 Gate 抬成 1，读取 host 公布的 `split_number` 与 `bytes_per_split`，校验它们是否可能，按协议的错误码回答，然后落下 Gate。拒绝时 client 不保留任何来自该次握手的布局信息。

**Blocked by:** 01

**Status:** done — 两轮 code-review 后修正完毕

- [x] client 等 Gate 抬成 1 之后才读取任何内容
- [x] `split_number` 小于 1 时回 0x1（非法函数数）
- [x] `bytes_per_split` 小于 8 时回 0x2（非法空间数）
- [x] 校验通过时回 0x0，并保留 `split_number` 与 `bytes_per_split` 供后续发现使用
- [x] 回完错误码之后落下 Gate，host 得以继续
- [x] 返回码与错误码的映射符合约定：1 成功、0 非法函数数、-1 非法空间数、-2 保留错误、-3 未编译进来
- [x] 与 Hello 共用同一条测试缝与同一个注入 waiter
