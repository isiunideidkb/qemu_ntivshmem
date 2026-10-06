# Audit: `common/.draft` vs `common/rpc_host.h`

Findings from reading the implementation against the design draft. Line references are to `common/rpc_host.h` unless stated otherwise. Every item below is an **open decision** — not a bug report.

## 1. Synchronisation as implemented

- **Hello**: host fills `host_handshake_t` (hello_message=1, cap_mask, length), publishes with a single `__atomic_store(..., RELEASE)` (403, 528), then spins until the low byte reads 0 — umonitor path 406-410, unix path 529-533 via `system_specific_waiter`. Guest error is cap_mask bit2 (416-419, 537-538).
- **Dispatch**: host stores the whole struct with `flag_lowaddr=1` (450, 558) and spins on low byte==0 (451-455, 561-564). Error codes 0x0/0x1/0x2 map to 1/0/-1 (460-472).
- **Function stage**: writes function code at `base+1`, writes flag=1 at `base`, waits for flag==0, then invokes the handler — or, for the huge-SHM type, writes a length at `base+2` and returns (479-509 waitpkg, 587-625 system).

## 2. Layout as implemented

| Structure | Size | Fields |
|---|---|---|
| `host_handshake_t` | 6 B packed | hello_message@0, cap_mask@1, `uint length`@2 |
| `host_dispatch_handshake_t` | 8 B packed | flag_lowaddr@0, flag_highaddr@1, `ushort split_number`@2, `uint bytes_per_split`@4 |
| Function head | 2 B | flag@+0, function code@+1 |

`dispatch_type`: SHM=0x0, HUGE_SHM=0x1, FUNC=0x2.
Guard conditions in code: lengths must be >0 and a power of two; `bytes_per_split >= 8`.

## 3. Contradictions between draft and code

| # | `common/.draft` says | Code does | Where |
|---|---|---|---|
| 1 | "都需要做完操作后 最后 才会设置Flag" (Gate written last) | Sets the Gate inside one whole-struct store | 403, 450, 528, 558 |
| 2 | Host "will clean the whole Addr Space! filled with zero" | Zeroes only the 6 B / 8 B header | 421-422, 458-459, 535-536, 566-567 |
| 3 | Length range [32MB, 4GB] | No such check; only >0 | 359, 221 |
| 4 | `bytes_per_split` power of 2 and ≥ 2MB | Asserts `>= 8` | 224 |
| 5 | FuncNumber 0x0 = huge-page SHM, 0x1 = SHM | SHM=0x0, HUGE_SHM=0x1 — **swapped** | 175-179 |
| 6 | SHM/HugeSHM flags are constants, "DONOT READ&WRITE 0x1" | Writes flag=1 and waits for 0; or returns ignoring it | 489, 503, 601, 622 |
| 7 | `<function body> + <function head> == <bytes per split>` | Requires equality only for SHM; allows `len % bps == 0` for HUGE/FUNC | 315, 321 |
| 8 | Host waits on "the 2byte short for Hello" | Watches only the 1-byte low address | 409, 531 |
| 9 | Max split number 65535 (explicit check) | No check, implicit via `ushort` | 238 |
| 10 | (no requirement) | Handshake length must also be a power of two | 238 |

## 4. Backend matrix (host side)

- `ENABLE_UNIX` — the only working system path, but every routine reads a `system_specific_waiter` that no file in this repo ever assigns. Until a caller sets it, the unix host routines assert-exit (522, 555, 606).
- `ENABLE_LINUX` — sets only `__USE_LINUX__`, while all system routines are gated on `__USE_UNIX__`. Dead: returns -1/-3 or does nothing.
- `ENABLE_WINDOWS` — immediately `#error "do not compile on windows SDK env"` (511-513). Unreachable by construction.
- `ENABLE_WAITPKG` — genuinely implemented (394, 438, 479).
- Auto-select guard (88) omits `ENABLE_UNIX`, so `ENABLE_UNIX` also triggers auto-detect.

`README` states the host RPC is "GNU/Linux only", which the three-backend switch does not reflect.

## 5. Guest / client side

- `common/rpc_client.h` is a 6-line comment. There is no client implementation of the protocol.
- `guest/i386nt/driver/cusdriver.c` implements BAR2 MMIO plus MDL/IOCTL mapping only. It declares `IsSupportMonitor`, `MonitorInit`, `MonitorAddr`, `WaitAddr` (86-89) and never calls them. No protocol structs present.
- `cusdriver_2.c` is the MONITOR/MWAIT machinery alone.
- The guest CPUID probe in `cusdriver_2.c` uses leaf 1 / ECX bit 3 (MONITOR), while the host probe in `umonitor_check.h` uses leaf 7 / ECX bit 5 (WAITPKG). The two ends disagree about what they are detecting, and the draft labels cap-mask bit0 "UMONITOR extension".

## 6. Defects in `host_patch/preload_lib.c`

- Invalid `#ifundef` directive (13, 17).
- Passes `end` (NULL) instead of `&end` to `strtoull` (38, 47).
- Line 60 uses an undeclared `n`, with `n & (n-1) == 0` parsing as `n & ((n-1) == 0)`.

## 7. Open TODOs in the code

`rpc_host.h` items 4 (robustness) and 5 (identifier errors, UB), plus inline markers at 28, 209, 299, 387, 515, 586, 651; `cusdriver.c` 1, 3, 6; `cusdriver_2.c` 94, 99, 104-105.

## 8. Build findings (verified with GCC 16.2.1, `-std=c23`)

**`rpc_host.h` does not compile at all.** Line 620:

```c
__atomic_store_n(((node->base_addr) + 2), node->length, __ATOMIC_RELEASE);
```

`__atomic_store_n` is a generic macro requiring a *typed* pointer; `node->base_addr + 2` is `void *`. GCC rejects it:

```
error: operand type 'void *' is incompatible with argument 1 of '__atomic_store_n'
```

This is unconditional — it fails under `ENABLE_UNIX`, under `ENABLE_LINUX`, and with no flags at all, because `host_internal_dispatch_runner_system` is outside every platform guard. **As it stands the host header cannot be compiled by anyone**, so it also cannot be tested against. Line 501 writes the same field correctly with `node->length` through a cast-free path, which is why the umonitor twin does not trip it.

**The `-mwaitpkg` requirement is intentional, not a defect.** `rpc_host.h:79` probes `__has_builtin(__builtin_ia32_umwait)` before `<immintrin.h>` is included at line 116. Without the intrinsic header in scope those builtins are invisible, so `ENABLE_WAITPKG` fails its own check and raises `#error "making sure you specified -mwaitpkg option"` (83). That is the intended gate: `-mwaitpkg` is what defines `__WAITPKG__` and enables the routines. The client header reaches the same requirement deliberately, checking `__WAITPKG__` alongside `__has_builtin` so the failure message is accurate.

**`common/umonitor_check.h` has no include guard** and defines `is_support_umonitor()` with external linkage. A translation unit that includes both `rpc_host.h` and `rpc_client.h` therefore gets a duplicate definition. Integrating the two ends in one binary needs that guard added — on the host side, since only `rpc_client.h` is in scope here.

