# Guest-side architecture (as built)

Findings from reading `guest/i386nt/driver/*` and `common/rpc_client.h`. Line refs are to `cusdriver.c` unless stated.

## What the driver is

Pure **WDM FDO** — no WDF/KMDF. Built against WDK 5.1 (XP/2003) and WDK 7.1, **strict C89**, **x86-only** (enforced by `#error` at 49-61). Targets 32-bit Windows XP-class guests.

Entry points: `DriverEntry` 1033, `CusAddDevice` 965, plus `CusCreate`, `CusClose`, `CusDeviceIoControl`, `CusPnp`, `CusPower`, `CusSystemControl`, `CusUnsupported`. Creates `\Device\CusDriver` with symlink `\DosDevices\CusDriver` (1001). INF matches `PCI\VEN_1AF4&DEV_1110` — ivshmem.

## How BAR2 is found and mapped

PCI config is read through hand-built `IRP_MJ_PNP` / `IRP_MN_READ_CONFIG` on the PDO (462-536). BAR2 = `BaseAddresses[2]` (582), size probed by writing `0xFFFFFFFF` (586) and computing `~(probe & 0xFFFFFFF0) + 1` (616), then `MmMapIoSpace(phys, length, MmCached)` (642).

**The length is dynamic** — `g_Bar2Length` is a runtime ULONG (108), established at `IRP_MN_START_DEVICE` (891). There is no compile-time size constant anywhere.

## What exists for sharing

`MmMapIoSpace` for BAR2, and a per-open **MDL** path: `IoAllocateMdl` (704) → `MmBuildMdlForNonPagedPool` (727) → `MmProbeAndLockPages(KernelMode, IoModifyAccess)` (730) → `MmMapLockedPagesSpecifyCache(UserMode, MmCached)` (732), torn down at 787-795.

No continuous memory, no DMA, no common buffer, no `ExAllocatePoolWithTag` despite `CUS_POOL_TAG` being defined (69).

There is exactly **one** IOCTL handler and it is code-agnostic: `CusDeviceIoControl` (813-860) ignores the function and device type, requires `METHOD_BUFFERED` and `outLen >= sizeof(PVOID)`, and writes the user-mode BAR2 base into the system buffer (853). No `CTL_CODE` is defined anywhere.

**There is no protocol structure in the driver.** The only shared region is the mapped BAR2, handed to user mode as `g_Bar2UserAddr`. Access is single-client, enforced by `g_IsOccupied` via `InterlockedCompareExchange` (683).

## Synchronisation constraints

No spinlocks, no spin-wait, no `KeStallExecutionProcessor`. Two KMUTEXes (`g_Mutex` 104, `g_LogMutex` 111). KEVENTs are per-call stack locals for synchronous IRP waits, `KeWaitForSingleObject(Executive, KernelMode, FALSE, NULL)` at 492, 530, 885 — all PASSIVE paths. The completion routine only sets the event (450).

**The driver can block; it never busy-waits.**

## `cusdriver_2.c` — and the wait-primitive mismatch

A 105-line **GCC/MinGW C23** file providing CPU `MONITOR`/`MWAIT` for the WDM driver, compiled separately and linked as `cusdriver_2.obj` (`SOURCES:5`, README):

- `IsSupportMonitor` — **CPUID.1:ECX bit3** (45)
- `MonitorInit` — MSR 0x1A0 bit19 enable, CPUID.5 monitor line size (55-92)
- `MonitorAddr` (93), `WaitAddr` (98) — `__builtin_ia32_monitor`/`mwait` under `gnu::target("sse3")`

`cusdriver.c` declares matching externs (86-89) and **never calls them**.

**This is a different instruction set from the host's.** `MONITOR`/`MWAIT` is CPUID.1:ECX bit3 (SSE3-era). `UMONITOR`/`UMWAIT`/`TPAUSE` is the WAITPKG extension at CPUID.7:ECX bit5, Ice Lake (2019) and later. The host's `rpc_host.h` and `umonitor_check.h` use the WAITPKG builtins; the guest uses the older pair. They cannot both be "leaf7".

Link defect: `cusdriver_2.c` declares `extern void CusLog(...)` (43) while `cusdriver.c` defines `CusLog` as **`static`** (343). Those calls cannot link as written.

## Why polling is forced

The project is built on `ivshmem-plain`, which has **no interrupt or doorbell**. (`ivshmem-doorbell` is the variant that does.) The guest therefore cannot be woken — it must poll the Gate. That is the reason a park primitive like `MONITOR`/`MWAIT` is in the picture at all, and it is a constraint the draft never states explicitly.

## Build

`SOURCES`: TARGETNAME=cusdriver, TARGETTYPE=DRIVER, SOURCES=cusdriver.c + cusdriver_2.obj, TARGETLIBS ntoskrnl/wdm/ntstrsafe, DRIVERTYPE=WDM. INF: NTx86 only, Class=System, demand-start service (`StartType=3`), DriverVer 09/21/2026. README: build under WDK after MinGW-compiling `cusdriver_2.c`; device enumerates as "PCI standard RAM controller".

## `common/rpc_client.h`

Six lines of comment, no declarations:

```
coding style C23 + GCC(MinGW)
clang-format --style=LLVM
common rpc protocol for host
use nullptr instead of NULL
```

Note it says **"for host"** — the file appears to have been copied from the host side and never adapted. It asserts C23 + GCC(MinGW), which is the same convention `cusdriver_2.c` follows and which the C89 WDK driver cannot itself satisfy.
