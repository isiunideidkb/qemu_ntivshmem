/* ============================================================================
 * cusdriver.c
 *
 * WDM driver, single file, C89/C99 compatible, WDK 5.1 (XP/2003) and WDK 7.1.
 *
 * Behaviour:
 *   1. On START_DEVICE: read PCI config, verify MMIO decode enable and BAR
 *      sanity (BAR2 != 0, BAR3 == 0), size BAR2 with dword-only config
 *      accesses, restore, then MmMapIoSpace(..., MmCached).
 *   2. On IRP_MJ_CREATE: build MDL over the mapped BAR2 with
 *      MmBuildMdlForNonPagedPool (no probe/lock: the mapping is already
 *      nonpaged), map into user mode with MmMapLockedPagesSpecifyCache.
 *   3. On IRP_MJ_DEVICE_CONTROL: fixed METHOD_BUFFERED IOCTL set, see the
 *      IOCTL definitions below.  Unknown codes are rejected.
 *   4. On IRP_MJ_CLEANUP: tear the user mapping down in the closing
 *      process context.
 *   5. On IRP_MJ_CLOSE: fallback teardown, reset occupancy.
 *   6. On STOP_DEVICE / REMOVE_DEVICE / SURPRISE_REMOVAL: unmap BAR2.
 *
 * Locking model:
 *   g_mutex is acquired and released WITHIN each dispatch (same thread),
 *   never held across IRPs -- KeReleaseMutex must run on the owning thread.
 *   Single-opener is enforced by the atomic g_is_occupied flag.  Each IOCTL
 *   code additionally owns one dedicated KMUTEX for its own duration.
 *
 * Style:
 *   Identifiers custom to this project follow the POSIX / lowercase_underscore
 *   convention (no Hungarian prefixes, no CamelCase).  Names that cross the
 *   compile or link boundary unchanged are deliberately left alone:
 *
 *     DriverEntry / CusAddDevice / CusUnload   -- WDM entry points
 *     CusLog                                   -- external symbol (monitor.c)
 *     monitor_init / correct_cache /
 *     monitor_addr_and_wait_until_z /
 *     monitor_addr_and_wait_until_o            -- monitor.c symbols
 *     IOCTL_CUS_* / CUS_MONITOR_REQUEST        -- shared user/kernel ABI
 *     NTSTATUS / PVOID / PMDL / PIRP / ...     -- DDK-provided types/macros
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

// 1. DDK build environment required
#if !defined(_M_IX86)
#error \
    "FATAL: Not in an x86 DDK build environment. Run setenv.bat with 'x86' argument."
#endif

// 2. x64 / AMD64 strictly forbidden
#if defined(_WIN64) || defined(_M_X64) || defined(_M_AMD64)
#error "FATAL: x64 environment detected. This driver is x86-ONLY."
#endif

#pragma gcc diagnostic push
#pragma clang diagnostic ignored "-Wignored-pragma-intrinsic"
#pragma clang diagnostic ignored "-Wmicrosoft-enum-forward-reference"
#pragma clang diagnostic ignored "-Wvisibility"
#pragma clang diagnostic ignored "-Wmicrosoft-anon-tag"
#pragma clang diagnostic ignored "-Wvla"
#pragma clang diagnostic ignored "-Wgnu-folding-constant"
#pragma clang diagnostic ignored "-Wpragma-pack"
#pragma clang diagnostic ignored "-Wnonportable-include-path"
#include <ntddk.h>
#include <stdarg.h> /* compiler-intrinsic: va_list / va_start / va_arg / va_end */
#include <wdm.h>
#pragma gcc diagnostic pop
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

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
  (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif

#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif

#ifndef PCI_ENABLE_MEMORY_SPACE
#define PCI_ENABLE_MEMORY_SPACE 0x0002u
#endif

/* ------------------------------------------------------------------------
 * Fixed IOCTL set.  ALL codes are METHOD_BUFFERED and synchronous.
 * Anything not listed here is rejected with STATUS_INVALID_DEVICE_REQUEST.
 *
 * IOCTL_CUS_GET_USER_BASE:
 *   Input : ignored.
 *   Output: PVOID (sizeof(PVOID) bytes) = user-mode BAR2 base from Create.
 *
 * IOCTL_CUS_PHYSICAL_ADDR:
 *   Input : PVOID (sizeof(PVOID) bytes) = user-mode virtual address.
 *   Output: the SAME slot is overwritten with the system address of the
 *           locked page span, or NULL on ANY failure.  The NTSTATUS is
 *           always STATUS_SUCCESS (errors are reported as NULL in the
 *           buffer) unless the system buffer cannot hold a pointer at all.
 *   A PAGE_SIZE span starting at the address is probed and locked with
 *   MmProbeAndLockPages(UserMode, IoModifyAccess).  On success the pages
 *   are NEVER unlocked and the MDL is deliberately retained: lifetime is
 *   a user-mode protocol responsibility (use heap memory you can retire).
 *
 * IOCTL_CUS_MONITOR_WAIT_Z / IOCTL_CUS_MONITOR_WAIT_O:
 *   Input : CUS_MONITOR_REQUEST, fixed 8 bytes:
 *           unsigned int Offset + unsigned int StatusCode slot.
 *   Output: unsigned int (fixed 4 bytes) at the START of the system buffer:
 *           0 = MONITOR/MWAIT unsupported (asm call skipped),
 *           1 = wait completed.  Written on EVERY completion.
 *   Monitored address = g_bar2_base_addr - Offset (per protocol).
 * ---------------------------------------------------------------------- */
#define IOCTL_CUS_GET_USER_BASE \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CUS_PHYSICAL_ADDR \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CUS_MONITOR_WAIT_Z \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CUS_MONITOR_WAIT_O \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _CUS_MONITOR_REQUEST {
  unsigned int Offset;     /* fixed 4 bytes, unsigned int */
  unsigned int StatusCode; /* fixed 4 bytes, unsigned int */
} CUS_MONITOR_REQUEST, *PCUS_MONITOR_REQUEST;

/* Compile-time guard: the wire layout must stay exactly 8 bytes. */
typedef char cus_monitor_request_size_check[
    (sizeof(CUS_MONITOR_REQUEST) == 8u) ? 1 : -1];

/* ------------------------------------------------------------------------
 * _Bool compatibility shim.
 * WDK 7.1 (VS2008, _MSC_VER 1500) and WDK 5.1 (VS2005, _MSC_VER 1400) C
 * compilers predate C99 _Bool (first added in VS2013, _MSC_VER 1800).
 * Typedef it so the declarations below compile UNCHANGED.  ABI-compatible:
 * _Bool and unsigned char both return in AL on x86 __cdecl.
 * ------------------------------------------------------------------------ */

/*
  !!Custom!!
*/
static _Bool g_is_support_monitor = 0;
extern _Bool __cdecl monitor_init(void);
extern void __cdecl correct_cache(void);
extern void __cdecl monitor_addr_and_wait_until_z(void*);
extern void __cdecl monitor_addr_and_wait_until_o(void*);

/* ------------------------------------------------------------------------
 * Device extension
 * ------------------------------------------------------------------------ */
typedef struct _CUS_DEVICE_EXTENSION {
  PDEVICE_OBJECT Self; /* our FDO                                       */
  PDEVICE_OBJECT LowerDevice; /* next device down (usually the PDO) */
  PDEVICE_OBJECT Pdo; /* physical device object                        */
} CUS_DEVICE_EXTENSION, *PCUS_DEVICE_EXTENSION;

/* ========================================================================
 *  Static state
 * ======================================================================*/

static KMUTEX g_mutex;

/* Warning: this variable is DEPRECATED, do not use in new code. */
static volatile LONG g_is_occupied = 0;

static volatile PVOID g_bar2_base_addr = NULL;
static volatile PVOID g_bar2_user_addr = NULL;
static ULONG g_bar2_length = 0;
static PMDL g_cus_mdl = NULL;

/* One dedicated KMUTEX per IOCTL code: each code serialises against
 * itself.  Acquired and released inside the same synchronous dispatch
 * call.  Never nested with g_mutex. */
static KMUTEX g_ioctl_user_base_mutex;
static KMUTEX g_ioctl_phys_addr_mutex;
static KMUTEX g_ioctl_mon_z_mutex;
static KMUTEX g_ioctl_mon_o_mutex;

static KMUTEX g_log_mutex; /* KMUTEX, NOT FAST_MUTEX -- see header */
static BOOLEAN g_log_mutex_init = FALSE;

/* ========================================================================
 *  Forward declarations
 * ======================================================================*/
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
NTSTATUS CusAddDevice(PDRIVER_OBJECT DriverObject,
                      PDEVICE_OBJECT PhysicalDeviceObject);
VOID CusUnload(PDRIVER_OBJECT DriverObject);

static NTSTATUS cus_create(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS cus_cleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS cus_close(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS cus_device_ioctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS cus_pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS cus_unsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS cus_sync_complete(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context);

VOID cus_log_init(VOID);
VOID cus_log_close(VOID);
VOID __cdecl cus_log(const char* fmt, ...) asm("_cus_log");

/* ========================================================================
 *  Logging
 *
 *  Log file: \??\C:\CusDriverLog.txt.
 *  Opened once at AddDevice with FILE_ALL_ACCESS and FILE_OVERWRITE_IF.
 *  From then on cus_log() opens with FILE_SHARE_READ only and appends.
 * ======================================================================*/

/* ------------------------------------------------------------------------
 *  Tiny, self-contained, kernel-safe formatter.
 *  Specifiers: %x %X %u %d %p %s %c %%
 *
 *  Rationale: neither WDK 5.1 nor WDK 7.1 ships a kernel-safe vsnprintf,
 *  and we must not lean on the CRT.  All locals are declared at the top of
 *  the loop body to satisfy strict C89.
 * ---------------------------------------------------------------------- */
static ULONG cus_format_string(char* buf, ULONG bufSize, const char* fmt,
                               va_list ap) {
  ULONG pos = 0;
  const char* p = fmt;

  if (bufSize == 0) {
    return 0;
  }

  while (*p != '\0' && pos + 1 < bufSize) {
    /* --- C89: declare everything for this iteration here --- */
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
 *  cus_log_init -- called ONCE from AddDevice.
 *
 *  Opens the log with FILE_OVERWRITE_IF: any existing log is deleted /
 *  truncated.  Writes the banner, then closes the handle immediately.
 *
 *  From this point until the driver is unloaded, the file is opened only
 *  with FILE_SHARE_READ, so the log can be read (Notepad, type, Emacs)
 *  but NOT written to or deleted by anyone else.
 * ---------------------------------------------------------------------- */
static VOID cus_log_init(VOID) {
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
 *  cus_log -- printf-style into the log file.
 *
 *  Per call: open (FILE_OPEN_IF, append-only, FILE_SHARE_READ), write,
 *  close.  Line ending is a bare LF (no CR) -- the log is consumed by
 *  Emacs.  A KMUTEX (not FAST_MUTEX) serialises because ZwCreateFile /
 *  ZwWriteFile require IRQL == PASSIVE_LEVEL.
 * ---------------------------------------------------------------------- */
VOID __cdecl cus_log(const char* fmt, ...) {
  char buf[1024];
  ULONG len;
  va_list ap;
  IO_STATUS_BLOCK iosb;
  UNICODE_STRING path;
  OBJECT_ATTRIBUTES oa;
  HANDLE hLog = NULL;
  NTSTATUS status;

  if (!g_log_mutex_init) {
    return;
  }

  va_start(ap, fmt);
  len = cus_format_string(buf, (ULONG)sizeof(buf), fmt, ap);
  va_end(ap);

  /* Bare LF, no CR. */
  if (len + 1 < (ULONG)sizeof(buf)) {
    buf[len++] = '\n';
  }

  RtlInitUnicodeString(&path, L"\\??\\C:\\CusDriverLog.txt");

  InitializeObjectAttributes(
      &oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

  KeWaitForSingleObject(&g_log_mutex, Executive, KernelMode, FALSE, NULL);

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

  KeReleaseMutex(&g_log_mutex, FALSE);
}

/* ------------------------------------------------------------------------
 *  cus_log_close -- nothing to do.
 *
 *  cus_log() never holds the file handle between calls, so there is
 *  nothing to close at unload.  Kept as a stub so DriverUnload() stays
 *  unchanged.  Per spec the log is NOT deleted at unload.
 * ---------------------------------------------------------------------- */
static VOID cus_log_close(VOID) { /* intentionally empty */ }

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
static NTSTATUS cus_power(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION dev_ext =
      (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

  /* Tell the power manager we are done with the current power IRP.
   * Required on all WDK 5.1 / WDK 7.1 targets. */
  PoStartNextPowerIrp(Irp);

  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(dev_ext->LowerDevice, Irp);
}

/* ========================================================================
 *  IRP_MJ_SYSTEM_CONTROL -- pass through to the lower device.
 *  Some stacks expect WMI to reach the PDO; failing here can wedge
 *  unrelated WMI queries during shutdown.
 * ======================================================================*/
static NTSTATUS cus_system_control(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION dev_ext =
      (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

  IoSkipCurrentIrpStackLocation(Irp);
  return IoCallDriver(dev_ext->LowerDevice, Irp);
}

/* ========================================================================
 *  PCI configuration-space helpers
 * ======================================================================*/

/* Completion routine: signal an event and ask the I/O manager to stop
 * processing the IRP so we can inspect it ourselves and free it.        */
static NTSTATUS cus_sync_complete(PDEVICE_OBJECT DevObj, PIRP Irp,
                                  PVOID Context) {
  PKEVENT ev = (PKEVENT)Context;

  UNREFERENCED_PARAMETER(DevObj);
  UNREFERENCED_PARAMETER(Irp);

  KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ------------------------------------------------------------------------
 *  cus_pci_config_access -- raw IRP_MN_READ_CONFIG / IRP_MN_WRITE_CONFIG
 *  against the PDO for an arbitrary (offset, length) region.
 *
 *  The completion routine returns STATUS_MORE_PROCESSING_REQUIRED so the
 *  I/O manager does not free the IRP behind our back; we free it with
 *  IoFreeIrp after we are done.
 *
 *  BAR sizing traffic uses this with length == sizeof(ULONG) so we never
 *  rewrite Command/Status or any other register by accident.
 * ---------------------------------------------------------------------- */
static NTSTATUS cus_pci_config_access(PDEVICE_OBJECT Pdo, UCHAR minor,
                                       ULONG offset, PVOID buffer, ULONG length) {
  PIRP irp;
  PIO_STACK_LOCATION stack;
  KEVENT event;
  NTSTATUS status;

  if (Pdo == NULL || buffer == NULL || length == 0) {
    return STATUS_INVALID_PARAMETER;
  }
  if (offset >= (ULONG)sizeof(PCI_COMMON_CONFIG) ||
      length > (ULONG)sizeof(PCI_COMMON_CONFIG) - offset) {
    return STATUS_INVALID_PARAMETER;
  }

  irp = IoAllocateIrp((CCHAR)Pdo->StackSize, FALSE);
  if (irp == NULL) {
    cus_log("cus_pci_config_access: IoAllocateIrp failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  stack = IoGetNextIrpStackLocation(irp);
  stack->MajorFunction = IRP_MJ_PNP;
  stack->MinorFunction = minor;
  stack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
  stack->Parameters.ReadWriteConfig.Offset = offset;
  stack->Parameters.ReadWriteConfig.Length = length;
  stack->Parameters.ReadWriteConfig.Buffer = buffer;

  KeInitializeEvent(&event, NotificationEvent, FALSE);
  IoSetCompletionRoutine(irp, cus_sync_complete, &event, TRUE, TRUE, TRUE);

  status = IoCallDriver(Pdo, irp);
  if (status == STATUS_PENDING) {
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
  }
  status = irp->IoStatus.Status;

  IoFreeIrp(irp);
  return status;
}

static NTSTATUS cus_read_pci_config(PDEVICE_OBJECT Pdo,
                                     PPCI_COMMON_CONFIG pci_config) {
  if (pci_config == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  return cus_pci_config_access(Pdo, IRP_MN_READ_CONFIG, 0, pci_config,
                                (ULONG)sizeof(PCI_COMMON_CONFIG));
}

/* ========================================================================
 *  BAR2 discovery / mapping
 * ======================================================================*/

/* Best-effort restore of the saved BAR2 dword.  Used on every failure
 * path after the sizing write may have landed. */
static VOID cus_restore_bar2(PDEVICE_OBJECT Pdo, ULONG saved_bar2) {
  NTSTATUS status;
  ULONG value;

  value = saved_bar2;
  status = cus_pci_config_access(
      Pdo, IRP_MN_WRITE_CONFIG,
      (ULONG)FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0.BaseAddresses[2]), &value,
      (ULONG)sizeof(ULONG));
  if (!NT_SUCCESS(status)) {
    cus_log("cus_restore_bar2: restore failed: %x", status);
  }
}

/* ------------------------------------------------------------------------
 *  cus_read_and_map_bar2
 *
 *  Sequence:
 *    1) read config; verify MMIO decode enable, type0 header,
 *       BAR2 != 0, BAR3 == 0, BAR2 is a memory BAR
 *    2) write 0xFFFFFFFF to the BAR2 dword only
 *    3) read the BAR2 dword back -> length = ~(probe & ~0xF) + 1
 *    4) restore the saved BAR2 dword
 *    5) MmMapIoSpace(savedBAR, length, MmCached)
 *
 *  BaseAddresses is a 6-element, 0-based array covering BAR0..BAR5, so
 *  BAR2 is at index 2 (NOT 3).
 *
 *  MmCached is intentional per spec.
 * ---------------------------------------------------------------------- */
static NTSTATUS cus_read_and_map_bar2(PDEVICE_OBJECT Pdo) {
  PCI_COMMON_CONFIG pci_config;
  NTSTATUS status;
  ULONG saved_bar2;
  ULONG saved_bar3;
  ULONG probe_value;
  ULONG length;
  ULONG bar2_offset;
  PHYSICAL_ADDRESS windows_phys_addr;
  PVOID mapped;

  if (Pdo == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  bar2_offset =
      (ULONG)FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0.BaseAddresses[2]);

  /* ---- Step 1: read current config, then gate everything ---- */
  RtlZeroMemory(&pci_config, sizeof(PCI_COMMON_CONFIG));
  cus_log("cus_read_and_map_bar2: reading PCI config");
  status = cus_read_pci_config(Pdo, &pci_config);
  if (!NT_SUCCESS(status)) {
    cus_log("cus_read_and_map_bar2: read config failed: %x", status);
    return status;
  }

  /* MMIO decode MUST be enabled before any BAR probing. */
  if ((pci_config.Command & PCI_ENABLE_MEMORY_SPACE) == 0) {
    cus_log("cus_read_and_map_bar2: MMIO decode disabled (Command=%x)",
            (ULONG)pci_config.Command);
    return STATUS_DEVICE_NOT_READY;
  }

  /* type0 layout is only valid for header type 0 (mask multifunction bit). */
  if ((pci_config.HeaderType & 0x7Fu) != 0x00u) {
    cus_log("cus_read_and_map_bar2: not a type0 header (HeaderType=%x)",
            (ULONG)pci_config.HeaderType);
    return STATUS_DEVICE_NOT_READY;
  }

  saved_bar2 = pci_config.u.type0.BaseAddresses[2]; /* BAR2 */
  saved_bar3 = pci_config.u.type0.BaseAddresses[3]; /* BAR3 */
  cus_log("cus_read_and_map_bar2: BAR2=%x BAR3=%x", saved_bar2, saved_bar3);

  if (saved_bar2 == 0) {
    cus_log("cus_read_and_map_bar2: BAR2 is zero (unassigned), aborting");
    return STATUS_DEVICE_NOT_READY;
  }
  if (saved_bar3 != 0) {
    cus_log("cus_read_and_map_bar2: BAR3 is not zero, aborting");
    return STATUS_DEVICE_NOT_READY;
  }
  if ((saved_bar2 & 0x1u) != 0) {
    cus_log("cus_read_and_map_bar2: BAR2 is an I/O BAR, aborting");
    return STATUS_DEVICE_NOT_READY;
  }

  /* ---- Step 2: write 0xFFFFFFFF to the BAR2 dword only ---- */
  probe_value = 0xFFFFFFFFUL;
  cus_log("cus_read_and_map_bar2: writing 0xFFFFFFFF to BAR2");
  status = cus_pci_config_access(Pdo, IRP_MN_WRITE_CONFIG, bar2_offset,
                                  &probe_value, (ULONG)sizeof(ULONG));
  if (!NT_SUCCESS(status)) {
    cus_log("cus_read_and_map_bar2: write 0xFFFFFFFF failed: %x", status);
    cus_restore_bar2(Pdo, saved_bar2);
    return status;
  }

  /* ---- Step 3: read back the sizing dword ---- */
  probe_value = 0;
  cus_log("cus_read_and_map_bar2: reading back BAR2 sizing value");
  status = cus_pci_config_access(Pdo, IRP_MN_READ_CONFIG, bar2_offset, &probe_value,
                                  (ULONG)sizeof(ULONG));
  if (!NT_SUCCESS(status)) {
    cus_log("cus_read_and_map_bar2: read-back failed: %x", status);
    cus_restore_bar2(Pdo, saved_bar2);
    return status;
  }
  cus_log("cus_read_and_map_bar2: BAR2 sizing value = %x", probe_value);

  /* ---- Step 4: compute length, validate, then restore ---- */
  /* Standard BAR-sizing formula: size = (~mask) + 1 where
   * mask = value & ~0xF.  Low 4 bits of a memory BAR are read-only
   * type info, so clear them before inverting. */
  length = (~(probe_value & 0xFFFFFFF0UL)) + 1UL;
  cus_log("cus_read_and_map_bar2: BAR2 length = %x", length);

  if (length == 0 || length == 0xFFFFFFFFUL ||
      (length & (length - 1UL)) != 0UL) {
    cus_log("cus_read_and_map_bar2: invalid BAR2 length, aborting");
    cus_restore_bar2(Pdo, saved_bar2);
    return STATUS_DEVICE_NOT_READY;
  }

  cus_log("cus_read_and_map_bar2: restoring BAR2 to %x", saved_bar2);
  status = cus_pci_config_access(Pdo, IRP_MN_WRITE_CONFIG, bar2_offset, &saved_bar2,
                                  (ULONG)sizeof(ULONG));
  if (!NT_SUCCESS(status)) {
    cus_log("cus_read_and_map_bar2: restore failed: %x", status);
    return status;
  }

  /* ---- Step 5: map it ---- */
  RtlZeroMemory(&windows_phys_addr, sizeof(windows_phys_addr));
  windows_phys_addr.u.LowPart = saved_bar2 & 0xFFFFFFF0UL; /* clear type bits */
  windows_phys_addr.u.HighPart = 0;                       /* 32-bit BAR      */

  cus_log("cus_read_and_map_bar2: MmMapIoSpace(phys=%x, len=%x, MmCached)",
          windows_phys_addr.u.LowPart, length);

  mapped = MmMapIoSpace(windows_phys_addr, length, MmCached);
  if (mapped == NULL) {
    cus_log("cus_read_and_map_bar2: MmMapIoSpace failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  g_bar2_base_addr = mapped;
  g_bar2_length = length;
  cus_log("cus_read_and_map_bar2: mapped BAR2 at %p (len=%x)", mapped, length);

  return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------
 *  cus_unmap_bar2 -- reverse of MmMapIoSpace.  Idempotent.
 * ---------------------------------------------------------------------- */
static VOID cus_unmap_bar2(VOID) {
  if (g_bar2_base_addr != NULL) {
    cus_log("cus_unmap_bar2: MmUnmapIoSpace(%p, %x)", g_bar2_base_addr,
            g_bar2_length);
    MmUnmapIoSpace(g_bar2_base_addr, g_bar2_length);
    g_bar2_base_addr = NULL;
  }
  g_bar2_length = 0;
}

/* ========================================================================
 *  IRP_MJ_CREATE handler
 *
 *  g_mutex is acquired and released on THIS thread within this dispatch.
 *  Single-opener is enforced by g_is_occupied, not by holding the mutex
 *  across IRPs (KeReleaseMutex must run on the owning thread).
 * ======================================================================*/
static NTSTATUS cus_create(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PVOID tmp_user_addr = NULL;

  UNREFERENCED_PARAMETER(DeviceObject);

  cus_log("Create: enter");

  KeWaitForSingleObject(&g_mutex, Executive, KernelMode, FALSE, NULL);

  /* Atomic test-and-set on g_is_occupied. */
  if (InterlockedCompareExchange(&g_is_occupied, 1, 0) != 0) {
    cus_log("Create: device already occupied");
    KeReleaseMutex(&g_mutex, FALSE);
    Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_DEVICE_BUSY;
  }

  if (g_bar2_base_addr == NULL || g_bar2_length == 0) {
    cus_log("Create: the programmer is fucking asshole - base_addr is NULL");
    g_is_occupied = 0;
    KeReleaseMutex(&g_mutex, FALSE);
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_STATE;
  }

  /* Build an MDL over the MmMapIoSpace'd region.  Passing NULL for the
   * IRP so this MDL is NOT tied to the create IRP. */
  g_cus_mdl = IoAllocateMdl((PVOID)g_bar2_base_addr, g_bar2_length,
                             FALSE, /* SecondaryBuffer */
                             FALSE, /* ChargeQuota     */
                             NULL); /* Irp -- deliberately not associated */
  if (g_cus_mdl == NULL) {
    cus_log("Create: IoAllocateMdl failed");
    g_is_occupied = 0;
    KeReleaseMutex(&g_mutex, FALSE);
    Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  /* The BAR2 mapping already lives in nonpaged system space: just fill
   * the MDL's PFN array from the kernel VA.  No MmProbeAndLockPages --
   * there is nothing to probe and nothing to lock, and therefore also
   * nothing to unlock later (IoFreeMdl is the whole teardown). */
  MmBuildMdlForNonPagedPool(g_cus_mdl);

  __try {
    tmp_user_addr = MmMapLockedPagesSpecifyCache(g_cus_mdl, UserMode, MmCached,
                                                 NULL, 0, NormalPagePriority);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cus_log("Create: exception while mapping BAR2 into user mode");
    IoFreeMdl(g_cus_mdl);
    g_cus_mdl = NULL;
    g_is_occupied = 0;
    KeReleaseMutex(&g_mutex, FALSE);
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }

  if (tmp_user_addr == NULL) {
    cus_log("Create: MmMapLockedPagesSpecifyCache failed");
    IoFreeMdl(g_cus_mdl);
    g_cus_mdl = NULL;
    g_is_occupied = 0;
    KeReleaseMutex(&g_mutex, FALSE);
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
  }

  g_bar2_user_addr = tmp_user_addr;
  cus_log("Create: fetched user addr = %p", tmp_user_addr);

  /* Release on the SAME thread that acquired.  Occupancy stays set until
   * cus_close; a second Create in between sees g_is_occupied and gets BUSY. */
  KeReleaseMutex(&g_mutex, FALSE);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  IRP_MJ_CLEANUP handler
 *
 *  Runs when the last handle reference goes away, in the context of the
 *  closing process -- the correct context for MmUnmapLockedPages on a
 *  UserMode mapping.  The user mapping is torn down here; occupancy
 *  reset stays in cus_close.
 * ======================================================================*/
static NTSTATUS cus_cleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  cus_log("Cleanup: enter");

  if (g_cus_mdl != NULL) {
    if (g_bar2_user_addr != NULL) {
      MmUnmapLockedPages((PVOID)g_bar2_user_addr, g_cus_mdl);
      g_bar2_user_addr = NULL;
    }
    /* No MmUnlockPages: the MDL was built with MmBuildMdlForNonPagedPool,
     * the pages were never probe-locked. */
    IoFreeMdl(g_cus_mdl);
    g_cus_mdl = NULL;
  }
  g_bar2_user_addr = NULL;

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  IRP_MJ_CLOSE handler
 * ======================================================================*/
static NTSTATUS cus_close(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  UNREFERENCED_PARAMETER(DeviceObject);

  cus_log("Close: enter");

  KeWaitForSingleObject(&g_mutex, Executive, KernelMode, FALSE, NULL);

  /* cus_cleanup normally tore the mapping down already.  Fallback
   * teardown for paths where CLEANUP did not run; idempotent. */
  if (g_cus_mdl != NULL) {
    if (g_bar2_user_addr != NULL) {
      MmUnmapLockedPages((PVOID)g_bar2_user_addr, g_cus_mdl);
      g_bar2_user_addr = NULL;
    }
    IoFreeMdl(g_cus_mdl);
    g_cus_mdl = NULL;
  }
  g_bar2_user_addr = NULL;

  g_is_occupied = 0;

  KeReleaseMutex(&g_mutex, FALSE);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  IRP_MJ_DEVICE_CONTROL helpers (all METHOD_BUFFERED, all synchronous)
 *
 *  Each helper owns one dedicated KMUTEX for its whole duration: only
 *  one caller per IOCTL code at a time.  g_mutex is NEVER touched here.
 * ======================================================================*/

static NTSTATUS cus_ioctl_get_user_base(PVOID buf, ULONG out_len, PULONG_PTR info) {
  PVOID user_addr;
  NTSTATUS status;

  KeWaitForSingleObject(&g_ioctl_user_base_mutex, Executive, KernelMode, FALSE,
                        NULL);

  user_addr = (PVOID)g_bar2_user_addr;

  if (out_len < (ULONG)sizeof(PVOID)) {
    status = STATUS_BUFFER_TOO_SMALL;
    *info = 0;
  } else if (g_cus_mdl == NULL || user_addr == NULL) {
    cus_log("IoCtl: device not in initialized state");
    status = STATUS_INVALID_DEVICE_STATE;
    *info = 0;
  } else {
    *(PVOID*)buf = user_addr;
    *info = (ULONG_PTR)sizeof(PVOID);
    status = STATUS_SUCCESS;
  }

  KeReleaseMutex(&g_ioctl_user_base_mutex, FALSE);
  return status;
}

/* ------------------------------------------------------------------------
 *  PhysicalAddr passthrough.  Must run in the requesting process context
 *  (a top-level synchronous DeviceIoControl does).  Protocol: the result
 *  (system address, or NULL on ANY failure) is always written back into
 *  the same pointer-sized slot; NTSTATUS stays SUCCESS.  On success the
 *  MDL is retained and the pages stay locked FOREVER -- unlock/lifetime
 *  is a user-mode protocol concern, not the kernel's.
 * ---------------------------------------------------------------------- */
static NTSTATUS cus_ioctl_phys_addr(PVOID buf, ULONG in_len, ULONG out_len,
                                     PULONG_PTR info) {
  PVOID user_va;
  PVOID sys_addr;
  PVOID result;
  PMDL pmdl;
  BOOLEAN locked;
  NTSTATUS status;

  KeWaitForSingleObject(&g_ioctl_phys_addr_mutex, Executive, KernelMode, FALSE,
                        NULL);

  result = NULL;
  status = STATUS_SUCCESS;
  *info = 0;

  /* System buffer is max(in_len, out_len); if it cannot hold a pointer at
   * all we cannot even report NULL without corrupting pool. */
  if (in_len < (ULONG)sizeof(PVOID) && out_len < (ULONG)sizeof(PVOID)) {
    status = STATUS_BUFFER_TOO_SMALL;
  } else {
    user_va = NULL;
    if (in_len >= (ULONG)sizeof(PVOID)) {
      user_va = *(PVOID*)buf;
    }

    if (user_va != NULL) {
      pmdl = IoAllocateMdl(user_va, PAGE_SIZE, FALSE, FALSE, NULL);
      if (pmdl != NULL) {
        locked = FALSE;
        __try {
          MmProbeAndLockPages(pmdl, UserMode, IoModifyAccess);
          locked = TRUE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          /* Probe/lock raised: no pages remain locked. */
          locked = FALSE;
        }

        if (locked) {
          sys_addr = NULL;
          __try {
            sys_addr = MmGetSystemAddressForMdlSafe(pmdl, NormalPagePriority);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
            sys_addr = NULL;
          }

          if (sys_addr != NULL) {
            result = sys_addr;
            /* SUCCESS: MDL kept, pages kept locked.  Never unlocked. */
          } else {
            /* Failure cleanup only; the success path never unlocks. */
            MmUnlockPages(pmdl);
            IoFreeMdl(pmdl);
          }
        } else {
          IoFreeMdl(pmdl);
        }
      }
    }

    *(PVOID*)buf = result;
    *info = (ULONG_PTR)sizeof(PVOID);
  }

  KeReleaseMutex(&g_ioctl_phys_addr_mutex, FALSE);
  return status;
}

/* ------------------------------------------------------------------------
 *  monitor_addr_and_wait_until_z / _o.
 *  Input : CUS_MONITOR_REQUEST (Offset + StatusCode slot, 8 bytes).
 *  Output: unsigned int at buffer start: 0 if g_is_support_monitor == 0
 *          (asm call skipped), 1 after the wait completes.  Always
 *          written, always synchronous.
 *  Address = g_bar2_base_addr - Offset, exactly per protocol.
 * ---------------------------------------------------------------------- */
static NTSTATUS cus_ioctl_monitor_wait(PVOID buf, ULONG in_len, ULONG out_len,
                                        PULONG_PTR info, PKMUTEX mutex,
                                        BOOLEAN until_one) {
  unsigned int offset;
  unsigned int result_code;
  ULONG_PTR base;
  ULONG_PTR target;
  NTSTATUS status;

  KeWaitForSingleObject(mutex, Executive, KernelMode, FALSE, NULL);

  base = (ULONG_PTR)g_bar2_base_addr; /* single volatile read */

  if (in_len < (ULONG)sizeof(CUS_MONITOR_REQUEST) ||
      out_len < (ULONG)sizeof(unsigned int)) {
    status = STATUS_BUFFER_TOO_SMALL;
    *info = 0;
  } else if (base == 0) {
    cus_log("IoCtl: monitor requested but BAR2 is not mapped");
    status = STATUS_INVALID_DEVICE_STATE;
    *info = 0;
  } else {
    offset = ((PCUS_MONITOR_REQUEST)buf)->Offset;

    if ((ULONG_PTR)offset > base) {
      /* base - offset would wrap around: reject defensively. */
      status = STATUS_INVALID_PARAMETER;
      *info = 0;
    } else {
      target = base - (ULONG_PTR)offset;

      if (g_is_support_monitor == 0) {
        result_code = 0u;
      } else {
        if (until_one) {
          monitor_addr_and_wait_until_o((PVOID)target);
        } else {
          monitor_addr_and_wait_until_z((PVOID)target);
        }
        result_code = 1u;
      }

      *(unsigned int*)buf = result_code;
      *info = (ULONG_PTR)sizeof(unsigned int);
      status = STATUS_SUCCESS;
    }
  }

  KeReleaseMutex(mutex, FALSE);
  return status;
}

/* ========================================================================
 *  IRP_MJ_DEVICE_CONTROL dispatcher (METHOD_BUFFERED only)
 * ======================================================================*/
static NTSTATUS cus_device_ioctl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PIO_STACK_LOCATION stack;
  ULONG code;
  ULONG in_len;
  ULONG out_len;
  PVOID buf;
  NTSTATUS status;
  ULONG_PTR info;

  UNREFERENCED_PARAMETER(DeviceObject);

  stack = IoGetCurrentIrpStackLocation(Irp);
  code = stack->Parameters.DeviceIoControl.IoControlCode;
  in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
  out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
  buf = Irp->AssociatedIrp.SystemBuffer;
  info = 0;

  cus_log("IoCtl: code=%x inLen=%u outLen=%u", code, in_len, out_len);

  if (buf == NULL) {
    status = STATUS_INVALID_PARAMETER;
  } else {
    switch (code) {
      case IOCTL_CUS_GET_USER_BASE:
        status = cus_ioctl_get_user_base(buf, out_len, &info);
        break;

      case IOCTL_CUS_PHYSICAL_ADDR:
        status = cus_ioctl_phys_addr(buf, in_len, out_len, &info);
        break;

      case IOCTL_CUS_MONITOR_WAIT_Z:
        status = cus_ioctl_monitor_wait(buf, in_len, out_len, &info,
                                         &g_ioctl_mon_z_mutex, FALSE);
        break;

      case IOCTL_CUS_MONITOR_WAIT_O:
        status = cus_ioctl_monitor_wait(buf, in_len, out_len, &info,
                                         &g_ioctl_mon_o_mutex, TRUE);
        break;

      default:
        cus_log("IoCtl: unknown code, rejected");
        status = STATUS_INVALID_DEVICE_REQUEST;
        info = 0;
        break;
    }
  }

  /* Never claim more output bytes than the caller provided. */
  if (info > (ULONG_PTR)out_len) {
    info = (ULONG_PTR)out_len;
  }

  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = info;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

/* ========================================================================
 *  IRP_MJ_PNP handler
 * ======================================================================*/
static NTSTATUS cus_pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
  PCUS_DEVICE_EXTENSION dev_ext;
  PIO_STACK_LOCATION stack;
  NTSTATUS status = STATUS_SUCCESS;

  dev_ext = (PCUS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
  stack = IoGetCurrentIrpStackLocation(Irp);

  switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE: {
      KEVENT event;

      KeInitializeEvent(&event, NotificationEvent, FALSE);

      IoCopyCurrentIrpStackLocationToNext(Irp);
      IoSetCompletionRoutine(Irp, cus_sync_complete, &event, TRUE, TRUE, TRUE);

      status = IoCallDriver(dev_ext->LowerDevice, Irp);
      if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
      }
      status = Irp->IoStatus.Status;

      if (NT_SUCCESS(status)) {
        cus_log("PnP: IRP_MN_START_DEVICE");
        status = cus_read_and_map_bar2(dev_ext->Pdo);
        if (!NT_SUCCESS(status)) {
          cus_log("PnP: BAR2 setup failed: %x", status);
          /* Fail START so PnP tears the device down. */
        }
      } else {
        cus_log("PnP: lower driver START_DEVICE failed: %x", status);
      }

      Irp->IoStatus.Status = status;
      Irp->IoStatus.Information = 0;
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
      return status;
    }

    /* ------------------------------------------------------------------
     * Refuse graceful remove / stop while a handle is open: proceeding
     * would unmap BAR2 underneath an active user mapping.  Surprise
     * removal cannot be refused (hardware is already gone).
     * ----------------------------------------------------------------*/
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE: {
      if (g_is_occupied != 0) {
        cus_log("PnP: query remove/stop rejected, device is open");
        Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_UNSUCCESSFUL;
      }
      IoSkipCurrentIrpStackLocation(Irp);
      return IoCallDriver(dev_ext->LowerDevice, Irp);
    }

    case IRP_MN_STOP_DEVICE: {
      cus_log("PnP: IRP_MN_STOP_DEVICE");
      cus_unmap_bar2();

      IoSkipCurrentIrpStackLocation(Irp);
      return IoCallDriver(dev_ext->LowerDevice, Irp);
    }

    case IRP_MN_REMOVE_DEVICE: {
      UNICODE_STRING sym_link;

      cus_log("PnP: IRP_MN_REMOVE_DEVICE");

      cus_unmap_bar2();

      IoSkipCurrentIrpStackLocation(Irp);
      status = IoCallDriver(dev_ext->LowerDevice, Irp);

      RtlInitUnicodeString(&sym_link, L"\\DosDevices\\CusDriver");
      IoDeleteSymbolicLink(&sym_link);

      IoDetachDevice(dev_ext->LowerDevice);
      IoDeleteDevice(DeviceObject);
      return status;
    }
    /* ------------------------------------------------------------------
     * IRP_MN_SURPRISE_REMOVAL -- device is gone; unmap the BAR before
     * forwarding so we cannot touch disappearing MMIO afterwards.
     * cus_unmap_bar2() is idempotent, so it is safe if REMOVE also runs.
     * ----------------------------------------------------------------*/
    case IRP_MN_SURPRISE_REMOVAL: {
      cus_log("PnP: IRP_MN_SURPRISE_REMOVAL");
      cus_unmap_bar2();

      IoSkipCurrentIrpStackLocation(Irp);
      return IoCallDriver(dev_ext->LowerDevice, Irp);
    }

    default:
      IoSkipCurrentIrpStackLocation(Irp);
      return IoCallDriver(dev_ext->LowerDevice, Irp);
  }
}

/* ========================================================================
 *  Catch-all for unsupported major functions
 * ======================================================================*/
static NTSTATUS cus_unsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
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
  PCUS_DEVICE_EXTENSION dev_ext;
  UNICODE_STRING device_name;
  UNICODE_STRING sym_link_name;

  KeInitializeMutex(&g_log_mutex, 0);
  g_log_mutex_init = TRUE;

  cus_log_init();

  /* Must run AFTER cus_log_init: probe MONITOR/MWAIT support, then fix
   * up the cache state.  Order is part of the protocol. */
  g_is_support_monitor = monitor_init();
  correct_cache();
  cus_log("AddDevice: monitor_init support = %u", (ULONG)g_is_support_monitor);

  cus_log("===========================================");
  cus_log("AddDevice: enter");

  RtlInitUnicodeString(&device_name, L"\\Device\\CusDriver");

  status = IoCreateDevice(DriverObject, sizeof(CUS_DEVICE_EXTENSION),
                          &device_name, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
  if (!NT_SUCCESS(status)) {
    cus_log("AddDevice: IoCreateDevice failed: %x", status);
    return status;
  }

  dev_ext = (PCUS_DEVICE_EXTENSION)fdo->DeviceExtension;
  RtlZeroMemory(dev_ext, sizeof(CUS_DEVICE_EXTENSION));
  dev_ext->Self = fdo;
  dev_ext->Pdo = PhysicalDeviceObject;

  dev_ext->LowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
  if (dev_ext->LowerDevice == NULL) {
    cus_log("AddDevice: IoAttachDeviceToDeviceStack failed");
    IoDeleteDevice(fdo);
    return STATUS_UNSUCCESSFUL;
  }

  RtlInitUnicodeString(&sym_link_name, L"\\DosDevices\\CusDriver");
  status = IoCreateSymbolicLink(&sym_link_name, &device_name);
  if (!NT_SUCCESS(status)) {
    cus_log("AddDevice: IoCreateSymbolicLink failed: %x", status);
    IoDetachDevice(dev_ext->LowerDevice);
    IoDeleteDevice(fdo);
    return status;
  }

  fdo->Flags |= DO_POWER_PAGABLE;
  fdo->Flags &= ~DO_DEVICE_INITIALIZING;

  cus_log("AddDevice: success");
  return STATUS_SUCCESS;
}

/* ========================================================================
 *  DriverUnload
 * ======================================================================*/
VOID CusUnload(PDRIVER_OBJECT DriverObject) {
  UNREFERENCED_PARAMETER(DriverObject);

  cus_log("Unload: enter");

  cus_unmap_bar2();

  cus_log_close();
}

/* ========================================================================
 *  DriverEntry
 * ======================================================================*/
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
  ULONG i;

  UNREFERENCED_PARAMETER(RegistryPath);

  KeInitializeMutex(&g_mutex, 0);
  KeInitializeMutex(&g_ioctl_user_base_mutex, 0);
  KeInitializeMutex(&g_ioctl_phys_addr_mutex, 0);
  KeInitializeMutex(&g_ioctl_mon_z_mutex, 0);
  KeInitializeMutex(&g_ioctl_mon_o_mutex, 0);

  for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
    DriverObject->MajorFunction[i] = cus_unsupported;
  }

  DriverObject->MajorFunction[IRP_MJ_CREATE] = cus_create;
  DriverObject->MajorFunction[IRP_MJ_CLEANUP] = cus_cleanup;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = cus_close;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = cus_device_ioctl;
  DriverObject->MajorFunction[IRP_MJ_PNP] = cus_pnp;
  DriverObject->MajorFunction[IRP_MJ_POWER] = cus_power;
  DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = cus_system_control;

  DriverObject->DriverExtension->AddDevice = CusAddDevice;
  DriverObject->DriverUnload = CusUnload;

  return STATUS_SUCCESS;
}

/* ============================================================================
 *  End of cusdriver.c
 * ==========================================================================*/
