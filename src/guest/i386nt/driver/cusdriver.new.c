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
 *   3. On IRP_MJ_DEVICE_CONTROL (METHOD_BUFFERED): dispatch by IOCTL.
 *   4. On IRP_MJ_CLEANUP: placeholder (no resources to release here).
 *   5. On IRP_MJ_CLOSE: tear user mapping down.
 *   6. On STOP_DEVICE / REMOVE_DEVICE: unmap BAR2.
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

// 1. Must be in an x86 DDK build environment.
#if !defined(_X86_) || !defined(_X86_)
#error \
    "FATAL: Not in an x86 DDK build environment. Run setenv.bat with 'x86' argument."
#endif

// 2. Absolutely no x64 / AMD64 environment.
#if defined(_AMD64_) || defined(_WIN64) || defined(_M_X64) || defined(_M_AMD64)
#error "FATAL: x64 environment detected. This driver is x86-ONLY."
#endif

// 3. No non-x86 compiler target.
#if defined(_M_IA64) || defined(_M_ARM) || defined(_M_ARM64)
#error "FATAL: Non-x86 compiler target detected."
#endif

#include <ntddk.h>
#include <stdarg.h> /* compiler-intrinsic: va_list / va_start / va_arg / va_end */
#include <wdm.h>

/* ------------------------------------------------------------------------
 * Pool tag.
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

#ifndef PCI_ENABLE_MEMORY_SPACE
#define PCI_ENABLE_MEMORY_SPACE 0x0002
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* ------------------------------------------------------------------------
 * Custom IOCTL codes.  All use METHOD_BUFFERED.
 * ---------------------------------------------------------------------- */
#define CUS_DEVICE_TYPE FILE_DEVICE_UNKNOWN

#define IOCTL_CUS_GET_BAR2_USER_ADDR \
    CTL_CODE(CUS_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CUS_PHYSICAL_ADDR_DIRECT \
    CTL_CODE(CUS_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CUS_MONITOR_WAIT_Z \
    CTL_CODE(CUS_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CUS_MONITOR_WAIT_O \
    CTL_CODE(CUS_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ------------------------------------------------------------------------
 * External monitor routines.
 * ---------------------------------------------------------------------- */
static _Bool IsSupportMonitor = 0;
extern _Bool __cdecl MonitorInit();
extern _Bool __cdecl CorrectCache();
extern void __cdecl MonitorAddrAndWaitUntilZ();
extern void __cdecl MonitorAddrAndWaitUntilO();

/* ------------------------------------------------------------------------
 * Device extension
 * ---------------------------------------------------------------------- */
typedef struct _CUS_DEVICE_EXTENSION {
  PDEVICE_OBJECT Self;        /* our FDO */
  PDEVICE_OBJECT LowerDevice; /* next device down (usually the PDO) */
  PDEVICE_OBJECT Pdo;         /* physical device object */
} CUS_DEVICE_EXTENSION, *PCUS_DEVICE_EXTENSION;

/* ========================================================================
 * Static state
 * ======================================================================*/

static KMUTEX g_Mutex;
static volatile LONG g_IsOccupied = 0;
static volatile PVOID g_Bar2BaseAddr = NULL;
static volatile PVOID g_Bar2UserAddr = NULL;
static ULONG g_Bar2Length = 0;
static PMDL g_CusMdlPtr = NULL;

static KMUTEX g_LogMutex;
static BOOLEAN g_LogMutexInit = FALSE;

/* ========================================================================
 * Forward declarations
 * ======================================================================*/
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
NTSTATUS CusAddDevice(PDRIVER_OBJECT DriverObject,
                      PDEVICE_OBJECT PhysicalDeviceObject);
VOID CusUnload(PDRIVER_OBJECT DriverObject);

static NTSTATUS CusCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusDeviceIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS CusSyncComplete(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context);

VOID CusLogInit(VOID);
VOID CusLogClose(VOID);
VOID CusLog(const char* fmt, ...);

/* ========================================================================
 * Logging
 * ======================================================================*/

static ULONG CusFormatString(char* buf, ULONG bufSize, const char* fmt,
                             va_list ap) {
  ULONG pos = 0;
  const char* p = fmt;

  if (bufSize == 0) {
    return 0;
  }

  while (*p != '\0' && pos + 1 < bufSize) {
    const char* s;
    ULONG v;
    LONG sv;
    ULONG uv;
    PVOID ptr;
    ULONG_PTR ptrv;
    const char* hex;
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
      s = va_arg(ap, const char*);
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
                        FILE_SHARE_READ,
                        FILE_OVERWRITE_IF,
                        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

  if (NT_SUCCESS(status) && hLog != NULL) {
    ZwWriteFile(hLog, NULL, NULL, NULL, &iosb, (PVOID)banner,
                (ULONG)(sizeof(banner) - 1), NULL, NULL);
    ZwClose(hLog);
  }
}

static VOID CusLog(const char* fmt, ...) {
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

  if (len + 1 < (ULONG)sizeof(buf)) {
    buf[len++] = '\n';
  }

  RtlInitUnicodeString(&path, L"\\??\\C:\\CusDriverLog.txt");

  InitializeObjectAttributes(
      &oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

  KeWaitForSingleObject(&g_LogMutex, Executive, KernelMode, FALSE, NULL);

  status =
      ZwCreateFile(&hLog, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                   FILE_OPEN_IF,
                   FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

  if (NT_SUCCESS(status) && hLog != NULL) {
    ZwWriteFile(hLog, NULL, NULL, NULL, &iosb, buf, len, NULL, NULL);
    ZwClose(hLog);
    hLog = NULL;
  }

  KeReleaseMutex(&g_LogMutex, FALSE);
}

static VOID CusLogClose(VOID) { /* intentionally empty */ }

/* ========================================================================
 * IRP_MJ_POWER handler
 * ======================================================================*/
static NTSTATUS CusPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION devExt =
      (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(devExt->LowerDevice, Irp);
}

/* ========================================================================
 * IRP_MJ_SYSTEM_CONTROL -- pass through to the lower device.
 * ======================================================================*/
static NTSTATUS CusSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION devExt =
      (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

  IoSkipCurrentIrpStackLocation(Irp);
  return IoCallDriver(devExt->LowerDevice, Irp);
}

/* ========================================================================
 * PCI configuration-space helpers
 * ======================================================================*/

static NTSTATUS CusSyncComplete(PDEVICE_OBJECT DevObj, PIRP Irp,
                                PVOID Context) {
  PKEVENT ev = (PKEVENT)Context;

  UNREFERENCED_PARAMETER(DevObj);
  UNREFERENCED_PARAMETER(Irp);

  KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

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
 * BAR2 discovery / mapping
 * ======================================================================*/

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

  /* Check MMIO enable bit */
  if ((pciConfig.Command & PCI_ENABLE_MEMORY_SPACE) == 0) {
    CusLog("CusReadAndMapBar2: memory space disabled, enabling");
    pciConfig.Command |= PCI_ENABLE_MEMORY_SPACE;
    status = CusWritePciConfig(Pdo, &pciConfig);
    if (!NT_SUCCESS(status)) {
      CusLog("CusReadAndMapBar2: failed to enable memory space: %x", status);
      return status;
    }
    RtlZeroMemory(&pciConfig, sizeof(PCI_COMMON_CONFIG));
    status = CusReadPciConfig(Pdo, &pciConfig);
    if (!NT_SUCCESS(status)) {
      CusLog("CusReadAndMapBar2: re-read config failed: %x", status);
      return status;
    }
    if ((pciConfig.Command & PCI_ENABLE_MEMORY_SPACE) == 0) {
      CusLog("CusReadAndMapBar2: memory space still disabled");
      return STATUS_DEVICE_NOT_READY;
    }
  }

  /* Check BAR2 non-zero and BAR3 zero */
  if (pciConfig.u.type0.BaseAddresses[2] == 0) {
    CusLog("CusReadAndMapBar2: BAR2 is zero");
    return STATUS_DEVICE_NOT_READY;
  }
  if (pciConfig.u.type0.BaseAddresses[3] != 0) {
    CusLog("CusReadAndMapBar2: BAR3 is not zero");
    return STATUS_DEVICE_NOT_READY;
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

    RtlZeroMemory(&pciConfig, sizeof(PCI_COMMON_CONFIG));
    (VOID) CusReadPciConfig(Pdo, &pciConfig);
    pciConfig.u.type0.BaseAddresses[2] = savedBar2;
    (VOID) CusWritePciConfig(Pdo, &pciConfig);
    return status;
  }

  probeValue = pciConfig.u.type0.BaseAddresses[2];
  CusLog("CusReadAndMapBar2: BAR2 sizing value = %x", probeValue);

  /* ---- Step 4: compute length, then restore ---- */
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
  windowsPhysAddr.u.LowPart = savedBar2 & 0xFFFFFFF0UL;
  windowsPhysAddr.u.HighPart = 0;

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
 * IRP_MJ_CREATE handler
 * ======================================================================*/
static NTSTATUS CusCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PVOID tmpUserAddr = NULL;

  UNREFERENCED_PARAMETER(DeviceObject);

  CusLog("Create: enter");

  KeWaitForSingleObject(&g_Mutex, Executive, KernelMode, FALSE, NULL);

  if (InterlockedCompareExchange(&g_IsOccupied, 1, 0) != 0) {
    CusLog("Create: device already occupied");
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_DEVICE_BUSY;
  }

  if (g_Bar2BaseAddr == NULL) {
    CusLog("Create: BAR2 base address is NULL");
    g_IsOccupied = 0;
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_STATE;
  }

  g_CusMdlPtr = IoAllocateMdl((PVOID)g_Bar2BaseAddr, g_Bar2Length,
                              FALSE, /* SecondaryBuffer */
                              FALSE, /* ChargeQuota */
                              NULL); /* Irp */
  if (g_CusMdlPtr == NULL) {
    CusLog("Create: IoAllocateMdl failed");
    g_IsOccupied = 0;
    KeReleaseMutex(&g_Mutex, FALSE);
    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  MmBuildMdlForNonPagedPool(g_CusMdlPtr);

  __try {
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
 * IRP_MJ_CLEANUP handler
 * ======================================================================*/
static NTSTATUS CusCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  CusLog("Cleanup: enter");

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

/* ========================================================================
 * IRP_MJ_CLOSE handler
 * ======================================================================*/
static NTSTATUS CusClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  CusLog("Close: enter");

  KeWaitForSingleObject(&g_Mutex, Executive, KernelMode, FALSE, NULL);

  if (g_Bar2BaseAddr == NULL || g_Bar2UserAddr == NULL || g_CusMdlPtr == NULL) {
    CusLog("Close: state inconsistent");
  }

  if (g_CusMdlPtr != NULL) {
    if (g_Bar2UserAddr != NULL) {
      MmUnmapLockedPages((PVOID)g_Bar2UserAddr, g_CusMdlPtr);
      g_Bar2UserAddr = NULL;
    }
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
 * IRP_MJ_DEVICE_CONTROL handler (METHOD_BUFFERED)
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
  method = METHOD_FROM_CTL_CODE(code);

  CusLog("IoCtl: code=%x inLen=%u outLen=%u", code, inLen, outLen);

  if (method != METHOD_BUFFERED) {
    CusLog("IoCtl: unsupported method");
    status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
  }

  if (g_Bar2BaseAddr == NULL || g_Bar2UserAddr == NULL || g_CusMdlPtr == NULL) {
    CusLog("IoCtl: device not in initialized state");
    status = STATUS_INVALID_DEVICE_STATE;
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
  }

  status = STATUS_SUCCESS;

  switch (code) {
    case IOCTL_CUS_GET_BAR2_USER_ADDR: {
      if (outLen < sizeof(PVOID)) {
        CusLog("IoCtl: GET_BAR2_USER_ADDR buffer too small");
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      *(PVOID*)buf = (PVOID)g_Bar2UserAddr;
      Irp->IoStatus.Information = sizeof(PVOID);
      break;
    }

    case IOCTL_CUS_PHYSICAL_ADDR_DIRECT: {
      PVOID userAddr;
      PMDL mdl;
      PVOID systemAddr = NULL;

      if (inLen < sizeof(PVOID) || outLen < sizeof(PVOID)) {
        CusLog("IoCtl: PHYSICAL_ADDR_DIRECT buffer too small");
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      userAddr = *(PVOID*)buf;
      CusLog("IoCtl: PHYSICAL_ADDR_DIRECT userAddr=%p", userAddr);

      mdl = IoAllocateMdl(userAddr, PAGE_SIZE, FALSE, FALSE, NULL);
      if (mdl != NULL) {
        __try {
          MmProbeAndLockPages(mdl, UserMode, IoModifyAccess);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          CusLog("IoCtl: PHYSICAL_ADDR_DIRECT exception in MmProbeAndLockPages");
          IoFreeMdl(mdl);
          mdl = NULL;
        }

        if (mdl != NULL) {
          systemAddr = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
          if (systemAddr == NULL) {
            CusLog("IoCtl: PHYSICAL_ADDR_DIRECT MmGetSystemAddressForMdlSafe failed");
          }
          /* Per protocol, do NOT unlock or free MDL on success/failure */
        }
      } else {
        CusLog("IoCtl: PHYSICAL_ADDR_DIRECT IoAllocateMdl failed");
      }

      *(PVOID*)buf = systemAddr;
      Irp->IoStatus.Information = sizeof(PVOID);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_CUS_MONITOR_WAIT_Z: {
      ULONG offset;
      ULONG statuscode;
      PVOID monitorAddr;

      if (inLen < 2 * sizeof(ULONG) || outLen < sizeof(ULONG)) {
        CusLog("IoCtl: MONITOR_WAIT_Z buffer too small");
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      offset = *(ULONG*)buf;
      statuscode = *((ULONG*)buf + 1);
      monitorAddr = (PVOID)((ULONG_PTR)g_Bar2BaseAddr - offset);
      CusLog("IoCtl: MONITOR_WAIT_Z offset=%x statuscode=%x addr=%p",
             offset, statuscode, monitorAddr);

      MonitorAddrAndWaitUntilZ(monitorAddr, statuscode);

      statuscode = IsSupportMonitor ? 1 : 0;
      *(ULONG*)buf = statuscode;
      Irp->IoStatus.Information = sizeof(ULONG);
      break;
    }

    case IOCTL_CUS_MONITOR_WAIT_O: {
      ULONG offset;
      ULONG statuscode;
      PVOID monitorAddr;

      if (inLen < 2 * sizeof(ULONG) || outLen < sizeof(ULONG)) {
        CusLog("IoCtl: MONITOR_WAIT_O buffer too small");
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }

      offset = *(ULONG*)buf;
      statuscode = *((ULONG*)buf + 1);
      monitorAddr = (PVOID)((ULONG_PTR)g_Bar2BaseAddr - offset);
      CusLog("IoCtl: MONITOR_WAIT_O offset=%x statuscode=%x addr=%p",
             offset, statuscode, monitorAddr);

      MonitorAddrAndWaitUntilO(monitorAddr, statuscode);

      statuscode = IsSupportMonitor ? 1 : 0;
      *(ULONG*)buf = statuscode;
      Irp->IoStatus.Information = sizeof(ULONG);
      break;
    }

    default:
      CusLog("IoCtl: unknown code %x", code);
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

  Irp->IoStatus.Status = status;
  if (status != STATUS_SUCCESS) {
    Irp->IoStatus.Information = 0;
  }
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

/* ========================================================================
 * IRP_MJ_PNP handler
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
 * Catch-all for unsupported major functions
 * ======================================================================*/
static NTSTATUS CusUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_INVALID_DEVICE_REQUEST;
}

/* ========================================================================
 * AddDevice
 * ======================================================================*/
NTSTATUS
CusAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
  NTSTATUS status;
  PDEVICE_OBJECT fdo = NULL;
  PCUS_DEVICE_EXTENSION devExt;
  UNICODE_STRING deviceName;
  UNICODE_STRING symLinkName;
  _Bool cacheResult;

  KeInitializeMutex(&g_LogMutex, 0);
  g_LogMutexInit = TRUE;

  CusLogInit();
  CusLog("===========================================");
  CusLog("AddDevice: enter");

  IsSupportMonitor = MonitorInit();
  CusLog("AddDevice: IsSupportMonitor = %d", IsSupportMonitor);
  cacheResult = CorrectCache();
  CusLog("AddDevice: CorrectCache = %d", cacheResult);

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
 * DriverUnload
 * ======================================================================*/
VOID CusUnload(PDRIVER_OBJECT DriverObject) {
  UNREFERENCED_PARAMETER(DriverObject);

  CusLog("Unload: enter");

  CusUnmapBar2();

  CusLogClose();
}

/* ========================================================================
 * DriverEntry
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
  DriverObject->MajorFunction[IRP_MJ_CLEANUP] = CusCleanup;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CusDeviceIoControl;
  DriverObject->MajorFunction[IRP_MJ_PNP] = CusPnp;
  DriverObject->MajorFunction[IRP_MJ_POWER] = CusPower;
  DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = CusSystemControl;

  DriverObject->DriverExtension->AddDevice = CusAddDevice;
  DriverObject->DriverUnload = CusUnload;

  return STATUS_SUCCESS;
}

/* ============================================================================
 * End of cusdriver.c
 * ==========================================================================*/