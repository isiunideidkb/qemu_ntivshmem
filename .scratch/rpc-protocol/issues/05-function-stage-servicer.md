# 05: Function 阶段的服务者（三类槽）

**What to build:** 第三个 DIALOG 的服务者。client 等某个槽的 Gate 被 host 抬成 1，从该槽的 head 判出它属于哪一类，据此行事：FUNC 调用调用方注册的处理器，SHM 与 HUGE_SHM 不需要处理器、由调用方在 Gate 落下后自行读取共享区。办完落下 Gate，把 host 释放回来。

**Blocked by:** 04

**Status:** done — 两轮 code-review 后修正完毕

- [x] 等某个槽的 Gate 抬成 1 之后才动作
- [x] 按 head 判定槽的类型：FUNC 调用注册的处理器
- [x] SHM 与 HUGE_SHM 不调用任何处理器
- [x] 处理完毕落下 Gate，host 得以进行下一步
- [x] 遇到 HUGE_SHM 时读出并记录它的 size
- [x] FUNC 槽没有注册处理器时明确失败，而不是静默跳过
- [x] 三类槽各有测试覆盖
- [x] 服务顺序是顺序的，不引入线程
