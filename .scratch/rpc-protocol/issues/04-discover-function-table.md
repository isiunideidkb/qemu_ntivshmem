# 04: 发现（discover）建表，HUGE_SHM 按 body 的 size 取跨度

**What to build:** 从 Address Space 里自行推导出全部 Function 槽并建成一张调用方拥有的表。client 不向 host 索取额外信息，只用手上已有的 `split_number` 与 `bytes_per_split`，逐 Split 走查，从每个槽的 head 读出它的类型。HUGE_SHM 槽的跨度取自它 body 里的 size 字段。

这张票修掉一个已知缺陷：当前实现逐 Split 各记一个节点、从不读取 size 字段，因此一个跨多个 Split 的 HUGE_SHM 会被错误地当成多个独立槽。

**Blocked by:** 03

**Status:** done — 两轮 code-review 后修正完毕

- [x] 仅凭 `split_number` 与 `bytes_per_split` 推导出每个槽的偏移，不向 host 索取额外信息
- [x] 逐 Split 走查，从每个槽的 head 读出其类型
- [x] HUGE_SHM 槽按 body 中 +2 处的 size 字段跳过其跨度
- [x] SHM 槽的跨度恰为一个 Bytes per Split
- [x] `split_number × bytes_per_split` 与公布的地址空间长度不符时，返回"布局不一致"
- [x] 一个 HUGE_SHM 占据多个 Split 的布局能被正确跨过——这是本缺陷的回归测试
- [x] 表可遍历，节点按发现顺序链接
- [x] 释放表只释放节点，绝不释放调用方的缓冲区
- [x] host 尚未写入 size 字段时行为可预期，不越界读

## 契约变更（作者 review 后）

作者 review 指出：未定义情形应 close-session，而不是直接返回（更不能是裸返回）；且**返回 ERROR 与 LOG 是两种并列的告警方式，不该同时使用**。据此（见 ADR-0004）：

| 情形 | 旧行为 | 新行为 |
|---|---|---|
| `split_number` 为 0 | 返回 -1 | **assert 中止**——握手已拒绝过，到达此处即调用方忽略了裁决 |
| `bytes_per_split` < 8 | 返回 0 | **assert 中止**，同上 |
| `split_number × bytes_per_split` ≠ length | 返回 0 | **assert 中止**——这是调用方的算术，不是对端的数据 |
| HUGE_SHM 未公布可用跨度 / 槽越界 / 不铺满 | 返回 0 + LOG | **仍返回 0**（对端数据不可用，是调用方该知道的真实失败），但**不再 LOG** |

所以 `discover` 现在只有两个上报结果：`1` 与 `0`。`-1` 已取消。

## 另需注意

等待 Gate 的逻辑也一并修正（见票 02）：**先判条件再等待**。按协议时序 gate 通常在 client 到达前就已被 host 抬起，此时直接返回；旧实现无条件 `_umonitor/_umwait`，会睡在一个已经发生过的事件上而卡死。
