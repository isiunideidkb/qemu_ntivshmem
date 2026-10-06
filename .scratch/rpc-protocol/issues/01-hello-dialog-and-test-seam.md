# 01: Hello DIALOG（system 路径）与测试缝

**What to build:** 一条可独立运行的验证路径，以及它覆盖的 Hello 行为。测试把伪造的 host 侧 Hello 消息写进一块 Address Space 缓冲区，调用 client 的 Hello 入口，断言 client 写回去的字节——全程不涉及 host 实现。client 侧的行为是：等 host 把 Gate 抬成 1，校验 hello 消息与公布的地址空间长度，回写自己的 Capability Mask（拒绝时置 error bit），然后落下 Gate。

这张票同时建立整个工作的测试缝：Address Space 缓冲区，加上一个可注入的 waiter 用来确定性地翻转 Gate。

**Blocked by:** None (can start immediately)

**Status:** done — 提交 fc52c10；测试 `.scratch/rpc-protocol/tests/01-hello-system.c`，25 项断言，3 种编译配置全通过

- [x] 测试缝可用：能在缓冲区里伪造 host 侧字节、断言 client 写回的字节，且不引用任何 host 实现
- [x] 可注入的 waiter 能确定性地把 Gate 翻成 1，测试不依赖真实自旋、真实 CPU 停等或计时
- [x] Gate 尚未为 1 时，client 不写入任何字节
- [x] hello 消息不为 1 时，client 置 Capability Mask 的 error bit 并落下 Gate
- [x] 公布的 Address Space 长度与 client 自身长度不符时，同样置 error bit
- [x] 校验通过时，client 回写自己的 Capability Mask 并落下 Gate
- [x] 未注入 waiter 时 system 路径明确失败，而不是静默空转
- [x] 一张票内即可演示：跑测试，观察缓冲区内容的变化

## 实现期间发现并已修复

1. **Capability Mask 位错位（严重）。** 原先照抄 `rpc_host.h` 的八进制字面量 `00000010`（= 8）等，导致 `is_phys` 去置 bit3、`is_phys && is_umonitor` 置 bit0+bit3，与本文件自己定义的枚举矛盾。已改为按枚举拼装（bit0=UMONITOR、bit1=PhysAddr）。
2. **测试是 tautological 的。** 原先断言 `space[1] == hh.cap_mask`，等于拿代码验代码，所以抓不到上面那条。已改为钉死字面量 `0x01 / 0x02 / 0x03 / 0x00`，真理源是 draft 的位定义。
3. **漏了 draft 要求的 host 能力掩码校验。** draft 明说 client 要 "check the accuracy of capability mask"。已补：host 置了 error bit（bit2）或任何保留位（bit3-7）即拒绝。
4. **多加了 draft 没有的 2 的幂约束。** `rpc_client_handshake_init` 与 `rpc_client_dispatch_init` 原先要求 Address Space 长度是 2 的幂，而 draft 只要求 `split_number × bytes_per_split == length`（只有 `bytes_per_split` 须为 2 的幂）。spec 的 Further Notes 第 5 条本就把这条列为 host 的偏差，client 不该复制。已移除，测试改用非 2 的幂长度 12288（= 3 × 4096）钉住该行为。

## 由 review 提出、但属于其它票的发现

- discover 未校验 draft 的收尾不变量（各节点长度之和 == 地址空间长度）→ 票 04
- `rpc_client_run_dispatch_in_threads` 名字说线程却不建线程，且其 runner 永不返回，`cur_node = cur_node->next` 是死代码 → 票 06
- `split_number >= 1` 这条断言作用在 `unsigned short` 上，握手已经拒过 `< 1`，所以返回 -1 不可达 → 票 03
- umonitor 与 system 两套实现逐段重复，只差等待那一句；抽一个 `wait_gate(addr)` 缝可消掉全部四对（Duplicated Code）→ 建议单独开票
