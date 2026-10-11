//todo add MONITOR/MWAIT support & ioctl
//todo virtaddr convert userspace_virt_addr to phys_addr ioctl for userspace process with specifying length,NonPaged Required,and lock(or modify) this page to non paged locked
//todo optimize read BAR register operations,more defensive programming : check the first read of BAR2 is not zero,BAR3 is  zero,check MMIO enable bit




//todo monitor/mwait ioctl wrapper for systesm specific waiter

/* ============================================================================
 * cusdriver.c
 *
 * WDM driver, single file, C89/C99 compatible, WDK 5.1 (XP/2003) and WDK 7.1.
 *
 * Behaviour:
 *   1. On START_DEVICE: read PCI config, locate BAR2, probe size with
 *      0xFFFFFFFF, restore, then MmMapIoSpace(..., MmCached).
 *   2. On IRP_MJ_CREATE: build MDL over the mapped BAR2, lock it,
 *      map into user mode with MmMapLockedPagesSpecifyCache.
 *   3. On IRP_MJ_DEVICE_CONTROL (METHOD_BUFFERED): return user-mode base.
 *   4. On IRP_MJ_CLOSE: tear user mapping down.
 *   5. On STOP_DEVICE / REMOVE_DEVICE: unmap BAR2.
 *
 * PORTABILITY NOTES (READ BEFORE EDITING):
 *
 *   - This source is intentionally pure 7-bit ASCII.  A Chinese Windows
 *     install defaults the WDK compiler to code page 936, which rejects
 *     UTF-8 non-ASCII characters with C4819.  Do NOT paste non-ASCII here.
 *
 *   - No SAL macros (_In_, _Out_, _Inout_, _In_opt_, ...): they do not
 *     exist in the WDK 5.1 headers and cause the compiler to parse every
 *     parameter as an implicit int.
 *
 *   - All declarations live at the TOP of their enclosing block.  The
 *     WDK 5.1 toolchain is a strict C89 compiler and rejects mixed
 *     declarations (C2275 / C2059).  Do not introduce C99-style "declare
 *     in the middle" code.
 *
 *   - <stdarg.h> is a COMPILER-INTRINSIC header, not the CRT.  In MSVC it
 *     only defines va_start/va_arg/va_end as macros that expand to compiler
 *     builtins.  It pulls in zero external symbols and links nothing.  The
 *     WDK's own ntddk.h/excpt.h uses it internally.  This driver does not
 *     call into msvcrt / libcmt / ntdll at all.
 *
 *   - Log serialisation uses a KMUTEX, NOT a FAST_MUTEX.  ExAcquireFastMutex
 *     raises IRQL to APC_LEVEL, and ZwWriteFile requires PASSIVE_LEVEL; a
 *     FAST_MUTEX here would bugcheck.  KMUTEX does not change IRQL.
 * ==========================================================================*/

// 1. 必须处于 DDK 构建环境
#if !defined(_X86_) || !defined(_X86_)
    #error "FATAL: Not in an x86 DDK build environment. Run setenv.bat with 'x86' argument."
#endif

// 2. 绝对禁止 x64 / AMD64 环境混入
#if defined(_AMD64_) || defined(_WIN64) || defined(_M_X64) || defined(_M_AMD64)
    #error "FATAL: x64 environment detected. This driver is x86-ONLY."
#endif

// 3. 禁止编译器自带架构宏指向非 x86
#if defined(_M_IA64) || defined(_M_ARM) || defined(_M_ARM64)
    #error "FATAL: Non-x86 compiler target detected."
#endif
 
#include <ntddk.h>
#include <stdarg.h> /* compiler-intrinsic: va_list / va_start / va_arg / va_end */
#include <wdm.h>
/* ------------------------------------------------------------------------
 * Pool tag.  Only used if we ever call ExAllocatePoolWithTag.
 * ---------------------------------------------------------------------- */
#define CUS_POOL_TAG 'DsuC'
#ifndef METHOD_FROM_CTL_CODE
#define METHOD_FROM_CTL_CODE(ctl) ((ctl) & 0x3u)
#endif

#ifndef FUNCTION_FROM_CTL_CODE
#define FUNCTION_FROM_CTL_CODE(ctl) (((ctl) >> 2) & 0xFFFu)
#endif

#ifndef DEVICE_TYPE_FROM_CTL_CODE
#define DEVICE_TYPE_FROM_CTL_CODE(ctl) (((ctl) >> 16) & 0xFFFFu)
#endif

#ifndef ACCESS_FROM_CTL_CODE
#define ACCESS_FROM_CTL_CODE(ctl) (((ctl) >> 14) & 0x3u)
#endif

extern _Bool IsSupportMonitor();
extern _Bool MonitorInit();
extern void MonitorAddr(void* addr);
extern void WaitAddr();

/* ------------------------------------------------------------------------
 * Device extension
 * ---------------------------------------------------------------------- */
typedef struct _CUS_DEVICE_EXTENSION {
  PDEVICE_OBJECT Self; /* our FDO                                       */
  PDEVICE_OBJECT LowerDevice; /* next device down (usually the PDO) */
  PDEVICE_OBJECT Pdo; /* physical device object                        */
} CUS_DEVICE_EXTENSION, *PCUS_DEVICE_EXTENSION;

/* ========================================================================
 *  Static state
 * ======================================================================*/

static KMUTEX g_Mutex;
static volatile LONG g_IsOccupied = 0;
static volatile PVOID g_Bar2BaseAddr = NULL;
static volatile PVOID g_Bar2UserAddr = NULL;
static ULONG g_Bar2Length = 0;
static PMDL g_CusMdlPtr = NULL;

static KMUTEX g_LogMutex; /* KMUTEX, NOT FAST_MUTEX -- see header */
static BOOLEAN g_LogMutexInit = FALSE;

/* ========================================================================
 *  Forward declarations
 * ======================================================================*/
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
NTSTATUS CusAddDevice(PDRIVER_OBJECT DriverObject,
                      PDEVICE_OBJECT PhysicalDeviceObject);
VOID CusUnload(PDRIVER_OBJECT DriverObject);

static NTSTATUS CusCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusDeviceIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusSyncComplete(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context);

VOID CusLogInit(VOID);
VOID CusLogClose(VOID);
VOID CusLog(const char *fmt, ...);

/* ========================================================================
 *  Logging
 *
 *  Log file: \??\C:\CusDriverLog.txt.
 *  Opened once at DriverEntry with FILE_ALL_ACCESS and FILE_OPEN_IF:
 *    - FILE_ALL_ACCESS  -> highest possible access rights on the handle.
 *    - FILE_OPEN_IF     -> open an existing file (do NOT delete / truncate
 *                          it), or create it if it does not yet exist.
 *  Kept open for the driver's lifetime; CusLog() appends.
 * ======================================================================*/

/* ------------------------------------------------------------------------
 *  Tiny, self-contained, kernel-safe formatter.
 *  Specifiers: %x %X %u %d %p %s %c %%
 *
 *  Rationale: neither WDK 5.1 nor WDK 7.1 ships a kernel-safe vsnprintf,
 *  and we must not lean on the CRT.  All locals are declared at the top of
 *  the loop body to satisfy strict C89.
 * ---------------------------------------------------------------------- */
static ULONG CusFormatString(char *buf, ULONG bufSize, const char *fmt,
                             va_list ap) {
  ULONG pos = 0;
  const char *p = fmt;

  if (bufSize == 0) {
    return 0;
  }

  while (*p != '\0' && pos + 1 < bufSize) {
    /* --- C89: declare everything for this iteration here --- */
    const char *s;
    ULONG v;
    LONG sv;
    ULONG uv;
    PVOID ptr;
    ULONG_PTR ptrv;
    const char *hex;
    char tmp[32];
    int i;
    int c;

    if (*p != '%') {
      buf[pos++] = *p++;
      continue;
    }

    p++; /* skip '%' */

    if (*p == '%') {
      buf[pos++] = '%';
      p++;
    } else if (*p == 's') {
      s = va_arg(ap, const char *);
      if (s == NULL) {
        s = "(null)";
      }
      while (*s != '\0' && pos + 1 < bufSize) {
        buf[pos++] = *s++;
      }
      p++;
    } else if (*p == 'x' || *p == 'X') {
      v = va_arg(ap, ULONG);
      hex = (*p == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
      i = 0;

      if (v == 0) {
        tmp[i++] = '0';
      }
      while (v != 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = hex[v & 0xFu];
        v >>= 4;
      }
      while (i > 0 && pos + 1 < bufSize) {
        buf[pos++] = tmp[--i];
      }
      p++;
    } else if (*p == 'u') {
      v = va_arg(ap, ULONG);
      i = 0;

      if (v == 0) {
        tmp[i++] = '0';
      }
      while (v != 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
      }
      while (i > 0 && pos + 1 < bufSize) {
        buf[pos++] = tmp[--i];
      }
      p++;
    } else if (*p == 'd') {
      sv = va_arg(ap, LONG);
      i = 0;

      if (sv < 0) {
        if (pos + 1 < bufSize) {
          buf[pos++] = '-';
        }
        /* -LONG_MIN cannot be represented in LONG; wrap via unsigned. */
        uv = (ULONG)(-(sv + 1)) + 1u;
      } else {
        uv = (ULONG)sv;
      }

      if (uv == 0) {
        tmp[i++] = '0';
      }
      while (uv != 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (uv % 10u));
        uv /= 10u;
      }
      while (i > 0 && pos + 1 < bufSize) {
        buf[pos++] = tmp[--i];
      }
      p++;
    } else if (*p == 'p') {
      ptr = va_arg(ap, PVOID);
      ptrv = (ULONG_PTR)ptr;
      i = 0;

      if (ptrv == 0) {
        tmp[i++] = '0';
      }
      while (ptrv != 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = "0123456789abcdef"[ptrv & 0xFu];
        ptrv >>= 4;
      }
      if (pos + 1 < bufSize) {
        buf[pos++] = '0';
      }
      if (pos + 1 < bufSize) {
        buf[pos++] = 'x';
      }
      while (i > 0 && pos + 1 < bufSize) {
        buf[pos++] = tmp[--i];
      }
      p++;
    } else if (*p == 'c') {
      c = va_arg(ap, int);
      if (pos + 1 < bufSize) {
        buf[pos++] = (char)c;
      }
      p++;
    } else {
      /* Unknown specifier: emit it literally. */
      if (pos + 1 < bufSize) {
        buf[pos++] = '%';
      }
      if (*p != '\0' && pos + 1 < bufSize) {
        buf[pos++] = *p++;
      }
    }
  }

  buf[pos] = '\0';
  return pos;
}

/* ------------------------------------------------------------------------
 *  CusLogInit -- called ONCE from AddDevice.
 *
 *  Opens the log with FILE_OVERWRITE_IF: any existing log is deleted /
 *  truncated.  Writes the banner, then closes the handle immediately.
 *
 *  From this point until the driver is unloaded, the file is opened only
 *  with FILE_SHARE_READ, so the log can be read (Notepad, type, Emacs)
 *  but NOT written to or deleted by anyone else.
 * ---------------------------------------------------------------------- */
static VOID CusLogInit(VOID) {
  UNICODE_STRING path;
  OBJECT_ATTRIBUTES oa;
  IO_STATUS_BLOCK iosb;
  HANDLE hLog = NULL;
  NTSTATUS status;
  static const char banner[] = "=== CusDriver log ===\n";

  RtlInitUnicodeString(&path, L"\\??\\C:\\CusDriverLog.txt");

  InitializeObjectAttributes(
      &oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

  status = ZwCreateFile(&hLog, FILE_ALL_ACCESS | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ,   /* only readers may open    */
                        FILE_OVERWRITE_IF, /* delete/truncate old log  */
                        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

  if (NT_SUCCESS(status) && hLog != NULL) {
    ZwWriteFile(hLog, NULL, NULL, NULL, &iosb, (PVOID)banner,
                (ULONG)(sizeof(banner) - 1), NULL, NULL);
    ZwClose(hLog);
  }
}

/* ------------------------------------------------------------------------
 *  CusLog -- printf-style into the log file.
 *
 *  Per call:
 *    Open (FILE_OPEN_IF, append-only, FILE_SHARE_READ), write, close.
 *
 *  FILE_SHARE_READ means other processes may open for reading, but
 *  cannot open for writing or for delete -- so the log is protected
 *  between AddDevice and Unload.
 *
 *  Line ending is a bare LF (no CR) -- the log is consumed by Emacs.
 *
 *  A KMUTEX (not FAST_MUTEX) is used for serialisation because ZwCreateFile
 *  and ZwWriteFile require IRQL == PASSIVE_LEVEL.
 * ---------------------------------------------------------------------- */
static VOID CusLog(const char *fmt, ...) {
  char buf[1024];
  ULONG len;
  va_list ap;
  IO_STATUS_BLOCK iosb;
  UNICODE_STRING path;
  OBJECT_ATTRIBUTES oa;
  HANDLE hLog = NULL;
  NTSTATUS status;

  if (!g_LogMutexInit) {
    return;
  }

  va_start(ap, fmt);
  len = CusFormatString(buf, (ULONG)sizeof(buf), fmt, ap);
  va_end(ap);

  /* Bare LF, no CR. */
  if (len + 1 < (ULONG)sizeof(buf)) {
    buf[len++] = '\n';
  }

  RtlInitUnicodeString(&path, L"\\??\\C:\\CusDriverLog.txt");

  InitializeObjectAttributes(
      &oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

  KeWaitForSingleObject(&g_LogMutex, Executive, KernelMode, FALSE, NULL);

  status =
      ZwCreateFile(&hLog, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, /* readers only */
                   FILE_OPEN_IF, /* never truncate here      */
                   FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

  if (NT_SUCCESS(status) && hLog != NULL) {
    ZwWriteFile(hLog, NULL, NULL, NULL, &iosb, buf, len, NULL, NULL);
    ZwClose(hLog);
    hLog = NULL;
  }

  KeReleaseMutex(&g_LogMutex, FALSE);
}

/* ------------------------------------------------------------------------
 *  CusLogClose -- nothing to do.
 *
 *  CusLog() never holds the file handle between calls, so there is
 *  nothing to close at unload.  Kept as a stub so DriverUnload() stays
 *  unchanged.  Per spec the log is NOT deleted at unload.
 * ---------------------------------------------------------------------- */
static VOID CusLogClose(VOID) { /* intentionally empty */ }

/* ========================================================================
 *  IRP_MJ_POWER handler
 *
 *  MUST forward every power IRP down the stack.  If a WDM FDO completes
 *  IRP_MJ_POWER with an error instead of forwarding it, the lower device
 *  never sees IRP_MN_SET_POWER / IRP_MN_QUERY_POWER and the power manager
 *  waits forever -- which manifests as:
 *
 *      DRIVER_POWER_STATE_FAILURE (0x9F)
 *
 *  during shutdown, reboot, or standby.
 *
 *  We deliberately do not implement wake, so IRP_MN_WAIT_WAKE is just
 *  forwarded too; the bus driver below us is free to accept or reject.
 * ======================================================================*/
static NTSTATUS CusPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION devExt =
      (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

  /* Tell the power manager we are done with the current power IRP.
   * Required on all WDK 5.1 / WDK 7.1 targets. */
  PoStartNextPowerIrp(Irp);

  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(devExt->LowerDevice, Irp);
}

/* ========================================================================
 *  IRP_MJ_SYSTEM_CONTROL -- pass through to the lower device.
 *  Some stacks expect WMI to reach the PDO; failing here can wedge
 *  unrelated WMI queries during shutdown.
 * ======================================================================*/
static NTSTATUS CusSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION devExt =
      (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

  IoSkipCurrentIrpStackLocation(Irp);
  return IoCallDriver(devExt->LowerDevice, Irp);
}

/* ========================================================================
 *  PCI configuration-space helpers
 * ======================================================================*/

/* Completion routine: signal an event and ask the I/O manager to stop
 * processing the IRP so we can inspect it ourselves and free it.        */
static NTSTATUS CusSyncComplete(PDEVICE_OBJECT DevObj, PIRP Irp,
                                PVOID Context) {
  PKEVENT ev = (PKEVENT)Context;

  UNREFERENCED_PARAMETER(DevObj);
  UNREFERENCED_PARAMETER(Irp);

  KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ------------------------------------------------------------------------
 *  CusReadPciConfig / CusWritePciConfig
 *
 *  Build a raw IRP_MN_READ_CONFIG / IRP_MN_WRITE_CONFIG against the PDO.
 *  The completion routine returns STATUS_MORE_PROCESSING_REQUIRED so the
 *  I/O manager does not free the IRP behind our back; we free it with
 *  IoFreeIrp after we are done.
 * ---------------------------------------------------------------------- */
static NTSTATUS CusReadPciConfig(PDEVICE_OBJECT Pdo,
                                 PPCI_COMMON_CONFIG pciConfig) {
  PIRP irp;
  PIO_STACK_LOCATION stack;
  KEVENT event;
  NTSTATUS status;

  if (Pdo == NULL || pciConfig == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  irp = IoAllocateIrp((CCHAR)Pdo->StackSize, FALSE);
  if (irp == NULL) {
    CusLog("CusReadPciConfig: IoAllocateIrp failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  stack = IoGetNextIrpStackLocation(irp);
  stack->MajorFunction = IRP_MJ_PNP;
  stack->MinorFunction = IRP_MN_READ_CONFIG;
  stack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
  stack->Parameters.ReadWriteConfig.Offset = 0;
  stack->Parameters.ReadWriteConfig.Length = sizeof(PCI_COMMON_CONFIG);
  stack->Parameters.ReadWriteConfig.Buffer = (PVOID)pciConfig;

  KeInitializeEvent(&event, NotificationEvent, FALSE);
  IoSetCompletionRoutine(irp, CusSyncComplete, &event, TRUE, TRUE, TRUE);

  status = IoCallDriver(Pdo, irp);
  if (status == STATUS_PENDING) {
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
  }
  status = irp->IoStatus.Status;

  IoFreeIrp(irp);
  return status;
}

static NTSTATUS CusWritePciConfig(PDEVICE_OBJECT Pdo,
                                  PPCI_COMMON_CONFIG pciConfig) {
  PIRP irp;
  PIO_STACK_LOCATION stack;
  KEVENT event;
  NTSTATUS status;

  if (Pdo == NULL || pciConfig == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  irp = IoAllocateIrp((CCHAR)Pdo->StackSize, FALSE);
  if (irp == NULL) {
    CusLog("CusWritePciConfig: IoAllocateIrp failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  stack = IoGetNextIrpStackLocation(irp);
  stack->MajorFunction = IRP_MJ_PNP;
  stack->MinorFunction = IRP_MN_WRITE_CONFIG;
  stack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
  stack->Parameters.ReadWriteConfig.Offset = 0;
  stack->Parameters.ReadWriteConfig.Length = sizeof(PCI_COMMON_CONFIG);
  stack->Parameters.ReadWriteConfig.Buffer = (PVOID)pciConfig;

  KeInitializeEvent(&event, NotificationEvent, FALSE);
  IoSetCompletionRoutine(irp, CusSyncComplete, &event, TRUE, TRUE, TRUE);

  status = IoCallDriver(Pdo, irp);
  if (status == STATUS_PENDING) {
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
  }
  status = irp->IoStatus.Status;

  IoFreeIrp(irp);
  return status;
}

/* ========================================================================
 *  BAR2 discovery / mapping
 * ======================================================================*/

/* ------------------------------------------------------------------------
 *  CusReadAndMapBar2
 *
 *  Sequence:
 *    1) read  config -> save BAR2
 *    2) write config with BAR2 = 0xFFFFFFFF
 *    3) read  config -> probe value -> length = ~(probe & ~0xF) + 1
 *    4) write config with BAR2 = saved value (restore)
 *    5) MmMapIoSpace(savedBAR, length, MmCached)
 *
 *  BaseAddresses is a 6-element, 0-based array covering BAR0..BAR5, so
 *  BAR2 is at index 2 (NOT 3).
 *
 *  MmCached is intentional.  A production driver would normally use
 *  MmNonCached or MmWriteCombined for a PCI BAR; the spec asked for
 *  MmCached and we honor that.
 * ---------------------------------------------------------------------- */
static NTSTATUS CusReadAndMapBar2(PDEVICE_OBJECT Pdo) {
  PCI_COMMON_CONFIG pciConfig;
  NTSTATUS status;
  ULONG savedBar2;
  ULONG probeValue;
  ULONG length;
  PHYSICAL_ADDRESS windowsPhysAddr;
  PVOID mapped;

  if (Pdo == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  RtlZeroMemory(&pciConfig, sizeof(PCI_COMMON_CONFIG));

  /* ---- Step 1: read current config ---- */
  CusLog("CusReadAndMapBar2: reading PCI config");
  status = CusReadPciConfig(Pdo, &pciConfig);
  if (!NT_SUCCESS(status)) {
    CusLog("CusReadAndMapBar2: read config failed: %x", status);
    return status;
  }

  savedBar2 = pciConfig.u.type0.BaseAddresses[2]; /* BAR2 */
  CusLog("CusReadAndMapBar2: BAR2 phys addr = %x", savedBar2);

  /* ---- Step 2: write 0xFFFFFFFF to BAR2 ---- */
  pciConfig.u.type0.BaseAddresses[2] = 0xFFFFFFFFUL;
  CusLog("CusReadAndMapBar2: writing 0xFFFFFFFF to BAR2");
  status = CusWritePciConfig(Pdo, &pciConfig);
  if (!NT_SUCCESS(status)) {
    CusLog("CusReadAndMapBar2: write 0xFFFFFFFF failed: %x", status);
    return status;
  }

  /* ---- Step 3: read back the sizing value ---- */
  RtlZeroMemory(&pciConfig, sizeof(PCI_COMMON_CONFIG));
  CusLog("CusReadAndMapBar2: reading back BAR2 sizing value");
  status = CusReadPciConfig(Pdo, &pciConfig);
  if (!NT_SUCCESS(status)) {
    CusLog("CusReadAndMapBar2: read-back failed: %x", status);

    /* Best-effort restore before bailing out. */
    RtlZeroMemory(&pciConfig, sizeof(PCI_COMMON_CONFIG));
    (VOID) CusReadPciConfig(Pdo, &pciConfig);
    pciConfig.u.type0.BaseAddresses[2] = savedBar2;
    (VOID) CusWritePciConfig(Pdo, &pciConfig);
    return status;
  }

  probeValue = pciConfig.u.type0.BaseAddresses[2];
  CusLog("CusReadAndMapBar2: BAR2 sizing value = %x", probeValue);

  /* ---- Step 4: compute length, then restore ---- */
  /* Standard BAR-sizing formula: size = (~mask) + 1 where
   * mask = value & ~0xF.  Low 4 bits of a memory BAR are read-only
   * type info, so clear them before inverting. */
  length = (~(probeValue & 0xFFFFFFF0UL)) + 1UL;
  CusLog("CusReadAndMapBar2: BAR2 length = %x", length);

  if (length == 0 || length == 0xFFFFFFFFUL) {
    CusLog("CusReadAndMapBar2: invalid BAR2 length, aborting");
    pciConfig.u.type0.BaseAddresses[2] = savedBar2;
    (VOID) CusWritePciConfig(Pdo, &pciConfig);
    return STATUS_DEVICE_NOT_READY;
  }

  pciConfig.u.type0.BaseAddresses[2] = savedBar2;
  CusLog("CusReadAndMapBar2: restoring BAR2 to %x", savedBar2);
  status = CusWritePciConfig(Pdo, &pciConfig);
  if (!NT_SUCCESS(status)) {
    CusLog("CusReadAndMapBar2: restore failed: %x", status);
    return status;
  }

  /* ---- Step 5: map it ---- */
  RtlZeroMemory(&windowsPhysAddr, sizeof(windowsPhysAddr));
  windowsPhysAddr.u.LowPart = savedBar2 & 0xFFFFFFF0UL; /* clear type bits */
  windowsPhysAddr.u.HighPart = 0;                       /* 32-bit BAR      */

  CusLog("CusReadAndMapBar2: MmMapIoSpace(phys=%x, len=%x, MmCached)",
         windowsPhysAddr.u.LowPart, length);

  mapped = MmMapIoSpace(windowsPhysAddr, length, MmCached);
  if (mapped == NULL) {
    CusLog("CusReadAndMapBar2: MmMapIoSpace failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  g_Bar2BaseAddr = mapped;
  g_Bar2Length = length;
  CusLog("CusReadAndMapBar2: mapped BAR2 at %p (len=%x)", mapped, length);

  return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------
 *  CusUnmapBar2 -- reverse of MmMapIoSpace.  Idempotent.
 * ---------------------------------------------------------------------- */
static VOID CusUnmapBar2(VOID) {
  if (g_Bar2BaseAddr != NULL) {
    CusLog("CusUnmapBar2: MmUnmapIoSpace(%p, %x)", g_Bar2BaseAddr,
           g_Bar2Length);
    MmUnmapIoSpace(g_Bar2BaseAddr, g_Bar2Length);
    g_Bar2BaseAddr = NULL;
  }
  g_Bar2Length = 0;
}

/* ========================================================================
 *  IRP_MJ_CREATE handler
 * ======================================================================*/
static NTSTATUS CusCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PVOID tmpUserAddr = NULL;

  UNREFERENCED_PARAMETER(DeviceObject);

  CusLog("Create: enter");

  /* Serialise: everything from here to the matching release runs
   * single-threaded with respect to other Create/Close/IoCtl paths. */
  KeWaitForSingleObject(&g_Mutex, Executive, KernelMode, FALSE, NULL);

  /* Atomic test-and-set on g_IsOccupied. */
  if (InterlockedCompareExchange(&g_IsOccupied, 1, 0) != 0) {
    CusLog("Create: device already occupied");
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_DEVICE_BUSY;
  }

  if (g_Bar2BaseAddr == NULL) {
    CusLog("Create: the programmer is fucking asshole - base_addr is NULL");
    g_IsOccupied = 0;
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_STATE;
  }

  /* Build an MDL over the MmMapIoSpace'd region.  Passing NULL for the
   * IRP so this MDL is NOT tied to the create IRP. */
  g_CusMdlPtr = IoAllocateMdl((PVOID)g_Bar2BaseAddr, g_Bar2Length,
                              FALSE, /* SecondaryBuffer */
                              FALSE, /* ChargeQuota     */
                              NULL); /* Irp -- deliberately not associated */
  if (g_CusMdlPtr == NULL) {
    CusLog("Create: IoAllocateMdl failed");
    g_IsOccupied = 0;
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  /* WARNING / HACK:
   * MmBuildMdlForNonPagedPool is technically wrong here -- the region
   * described by the MDL lives in the system address range (from
   * MmMapIoSpace), not in the non-paged pool.  It also sets the
   * MDL_PAGES_LOCKED flag.  The subsequent MmProbeAndLockPages on an
   * already-built MDL is therefore ill-formed and may raise an
   * exception on some kernels.  The __try/__except below traps that
   * and turns it into a clean failure.  This ordering was requested
   * explicitly and is preserved faithfully. */
  MmBuildMdlForNonPagedPool(g_CusMdlPtr);

  __try {
    MmProbeAndLockPages(g_CusMdlPtr, KernelMode, IoModifyAccess);

    tmpUserAddr = MmMapLockedPagesSpecifyCache(g_CusMdlPtr, UserMode, MmCached,
                                               NULL, 0, NormalPagePriority);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    CusLog("Create: exception in MDL setup");
    if (g_CusMdlPtr != NULL) {
      IoFreeMdl(g_CusMdlPtr);
      g_CusMdlPtr = NULL;
    }
    g_IsOccupied = 0;
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }

  if (tmpUserAddr == NULL) {
    CusLog("Create: MmMapLockedPagesSpecifyCache failed");
    MmUnlockPages(g_CusMdlPtr);
    IoFreeMdl(g_CusMdlPtr);
    g_CusMdlPtr = NULL;
    g_IsOccupied = 0;
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }

  g_Bar2UserAddr = tmpUserAddr;
  CusLog("Create: fetched user addr = %p", tmpUserAddr);

  KeReleaseMutex(&g_Mutex, FALSE);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  IRP_MJ_CLOSE handler
 * ======================================================================*/
static NTSTATUS CusClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  CusLog("Close: enter");

  KeWaitForSingleObject(&g_Mutex, Executive, KernelMode, FALSE, NULL);

  if (g_Bar2BaseAddr == NULL || g_Bar2UserAddr == NULL || g_CusMdlPtr == NULL) {
    CusLog("Close: the programmer is fucking asshole - state inconsistent");
    /* Fall through; tear down whatever is present. */
  }

  if (g_CusMdlPtr != NULL) {
    if (g_Bar2UserAddr != NULL) {
      MmUnmapLockedPages((PVOID)g_Bar2UserAddr, g_CusMdlPtr);
      g_Bar2UserAddr = NULL;
    }
    MmUnlockPages(g_CusMdlPtr);
    IoFreeMdl(g_CusMdlPtr);
    g_CusMdlPtr = NULL;
  }

  g_IsOccupied = 0;

  KeReleaseMutex(&g_Mutex, FALSE);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  IRP_MJ_DEVICE_CONTROL handler (METHOD_BUFFERED)
 *
 *  Input ignored.  Output must be >= sizeof(PVOID); we write the
 *  user-mode BAR2 base address into the system buffer.
 * ======================================================================*/
static NTSTATUS CusDeviceIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PIO_STACK_LOCATION stack;
  ULONG code;
  ULONG inLen;
  ULONG outLen;
  PVOID buf;
  NTSTATUS status;
  ULONG method;

  UNREFERENCED_PARAMETER(DeviceObject);

  stack = IoGetCurrentIrpStackLocation(Irp);
  code = stack->Parameters.DeviceIoControl.IoControlCode;
  inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
  outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
  buf = Irp->AssociatedIrp.SystemBuffer;
  method = METHOD_FROM_CTL_CODE(code); // hack: method code

  CusLog("IoCtl: code=%x inLen=%u outLen=%u", code, inLen, outLen);

  UNREFERENCED_PARAMETER(inLen); /* input is ignored */

  if ((outLen < sizeof(PVOID)) || (method != 0)) {
    CusLog("IoCtl: invalid device request");
    status = STATUS_BUFFER_TOO_SMALL;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
  }

  if (g_CusMdlPtr == NULL || g_Bar2UserAddr == NULL) {
    CusLog("IoCtl: device not in initialized state");
    status = STATUS_INVALID_DEVICE_STATE;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
  }

  *(PVOID *)buf = (PVOID)g_Bar2UserAddr;
  Irp->IoStatus.Information = sizeof(PVOID);
  status = STATUS_SUCCESS;

  Irp->IoStatus.Status = status;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

/* ========================================================================
 *  IRP_MJ_PNP handler
 * ======================================================================*/
static NTSTATUS CusPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION devExt;
  PIO_STACK_LOCATION stack;
  NTSTATUS status = STATUS_SUCCESS;

  devExt = (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
  stack = IoGetCurrentIrpStackLocation(Irp);

  switch (stack->MinorFunction) {

  case IRP_MN_START_DEVICE: {
    KEVENT event;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, CusSyncComplete, &event, TRUE, TRUE, TRUE);

    status = IoCallDriver(devExt->LowerDevice, Irp);
    if (status == STATUS_PENDING) {
      KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = Irp->IoStatus.Status;

    if (NT_SUCCESS(status)) {
      CusLog("PnP: IRP_MN_START_DEVICE");
      status = CusReadAndMapBar2(devExt->Pdo);
      if (!NT_SUCCESS(status)) {
        CusLog("PnP: BAR2 setup failed: %x", status);
        /* Fail START so PnP tears the device down. */
      }
    } else {
      CusLog("PnP: lower driver START_DEVICE failed: %x", status);
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
  }

  case IRP_MN_STOP_DEVICE: {
    CusLog("PnP: IRP_MN_STOP_DEVICE");
    CusUnmapBar2();

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(devExt->LowerDevice, Irp);
  }

  case IRP_MN_REMOVE_DEVICE: {
    UNICODE_STRING symLink;

    CusLog("PnP: IRP_MN_REMOVE_DEVICE");

    CusUnmapBar2();

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(devExt->LowerDevice, Irp);

    RtlInitUnicodeString(&symLink, L"\\DosDevices\\CusDriver");
    IoDeleteSymbolicLink(&symLink);

    IoDetachDevice(devExt->LowerDevice);
    IoDeleteDevice(DeviceObject);
    return status;
  }
  /* ------------------------------------------------------------------
   * IRP_MN_SURPRISE_REMOVAL -- device is gone; unmap the BAR before
   * forwarding so we cannot touch disappearing MMIO afterwards.
   * CusUnmapBar2() is idempotent, so it is safe if REMOVE also runs.
   * ----------------------------------------------------------------*/
  case IRP_MN_SURPRISE_REMOVAL: {
    CusLog("PnP: IRP_MN_SURPRISE_REMOVAL");
    CusUnmapBar2();

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(devExt->LowerDevice, Irp);
  }

  default:
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(devExt->LowerDevice, Irp);
  }
}

/* ========================================================================
 *  Catch-all for unsupported major functions
 * ======================================================================*/
static NTSTATUS CusUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_INVALID_DEVICE_REQUEST;
}

/* ========================================================================
 *  AddDevice
 * ======================================================================*/
NTSTATUS
CusAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
  NTSTATUS status;
  PDEVICE_OBJECT fdo = NULL;
  PCUS_DEVICE_EXTENSION devExt;
  UNICODE_STRING deviceName;
  UNICODE_STRING symLinkName;

  KeInitializeMutex(&g_LogMutex, 0);
  g_LogMutexInit = TRUE;

  CusLogInit();
  CusLog("===========================================");
  CusLog("AddDevice: enter");

  RtlInitUnicodeString(&deviceName, L"\\Device\\CusDriver");

  status = IoCreateDevice(DriverObject, sizeof(CUS_DEVICE_EXTENSION),
                          &deviceName, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
  if (!NT_SUCCESS(status)) {
    CusLog("AddDevice: IoCreateDevice failed: %x", status);
    return status;
  }

  devExt = (PCUS_DEVICE_EXTENSION)fdo->DeviceExtension;
  RtlZeroMemory(devExt, sizeof(CUS_DEVICE_EXTENSION));
  devExt->Self = fdo;
  devExt->Pdo = PhysicalDeviceObject;

  devExt->LowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
  if (devExt->LowerDevice == NULL) {
    CusLog("AddDevice: IoAttachDeviceToDeviceStack failed");
    IoDeleteDevice(fdo);
    return STATUS_UNSUCCESSFUL;
  }

  RtlInitUnicodeString(&symLinkName, L"\\DosDevices\\CusDriver");
  status = IoCreateSymbolicLink(&symLinkName, &deviceName);
  if (!NT_SUCCESS(status)) {
    CusLog("AddDevice: IoCreateSymbolicLink failed: %x", status);
    IoDetachDevice(devExt->LowerDevice);
    IoDeleteDevice(fdo);
    return status;
  }

  fdo->Flags |= DO_POWER_PAGABLE;
  fdo->Flags &= ~DO_DEVICE_INITIALIZING;

  CusLog("AddDevice: success");
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  DriverUnload
 * ======================================================================*/
VOID CusUnload(PDRIVER_OBJECT DriverObject) {
  UNREFERENCED_PARAMETER(DriverObject);

  CusLog("Unload: enter");

  CusUnmapBar2();

  CusLogClose();
}

/* ========================================================================
 *  DriverEntry
 * ======================================================================*/
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
  ULONG i;

  UNREFERENCED_PARAMETER(RegistryPath);

  KeInitializeMutex(&g_Mutex, 0);

  for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
    DriverObject->MajorFunction[i] = CusUnsupported;
  }

  DriverObject->MajorFunction[IRP_MJ_CREATE] = CusCreate;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = CusClose;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CusDeviceIoControl;
  DriverObject->MajorFunction[IRP_MJ_PNP] = CusPnp;
  DriverObject->MajorFunction[IRP_MJ_POWER] = CusPower; /* NEW */
  DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] =
      CusSystemControl; /* NEW */

  DriverObject->DriverExtension->AddDevice = CusAddDevice;
  DriverObject->DriverUnload = CusUnload;

  return STATUS_SUCCESS;
}

/* ============================================================================
 *  End of cusdriver.c
 * ==========================================================================*/
