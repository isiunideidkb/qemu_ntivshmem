# 02: Hello 的 waitpkg 路径与自动路由

**What to build:** Hello DIALOG 的第二条完整路径——用 UMONITOR/UMWAIT 停等在 Gate 上，以及选择它的自动路由。路由先看编译期开关，再看运行 CPU 是否具备该能力，两者都满足才走 waitpkg；否则走 system 路径。waitpkg 变体自身也会重复检查一次并在不支持时回退。

**Blocked by:** 01

**Status:** done — 两轮 code-review 后修正完毕

- [x] 存在 waitpkg 变体，用 UMONITOR/UMWAIT 停等 Gate，行为与 system 变体对外一致
- [x] waitpkg 变体在运行 CPU 不支持时自行回退到 system 变体，绝不空转
- [x] 路由入口先判编译期开关、再判运行时 CPU 能力，二者皆满足才走 waitpkg
- [x] 编译期开关打开但运行时无该能力时，最终行为与 system 路径完全一致
- [x] 支持该能力的 CPU 上有测试覆盖
- [x] 不支持的 CPU 上该测试跳过而非失败
- [x] 重复询问同一次 CPU 能力是可接受的，不因此引入缓存或状态
