/*
  coding style C23 + GCC(MinGW)
  clang-format --style=LLVM
  common rpc protocol for client
  use nullptr instead of NULL

  The client is the PASSIVE side of every DIALOG. The host raises the gate
  (writes 1); the client does its work and lowers it (writes 0). The client
  never raises a gate on its own initiative.

    stage 1  hello     wait for the gate, validate the hello, answer with the
                       capability mask, lower the gate
    stage 2  dispatch  wait for the gate, discover the function layout split
                       by split into a table, lower the gate
    stage 3  function  service the slots the host raises, then lower each gate

  The wait is the only thing the two backends do differently, so it is factored
  into rpc_client_wait_until_gate_up and every stage shares one body. The public
  entries still come in _umonitor / _system pairs with a router in front, so the
  shape mirrors rpc_host.h.

  Relationship to the host header: this file deliberately does NOT reuse the
  host_*_t types or the host routines. It follows common/.draft as the protocol
  authority.

  todo list:
  1. a HUGE_SHM node's span is read from the size in its body (see
     rpc_client_discover_dispatch). The draft puts the size there but rpc_host.h
     only publishes it while running the node, so against today's host a
     multi-Split HUGE_SHM cannot be discovered. Reported to the author.
  2. rpc_client_run_dispatch polls; it does not park across several slots,
     because the injected waiter listens on one address only.
*/

/* ===== PLATFORM CHECK ===== */
#pragma once
#ifndef __GNUC__
#error "wrong platform"
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

/* ===== PLATFORM SWITCH ===== */
//clean predefs
#if defined(__USE_WINDOWS__)
#undef __USE_WINDOWS__
#endif
#if defined(__USE_WAITPKG__)
#undef __USE_WAITPKG__
#endif
#if defined(__USE_LINUX__)
#undef __USE_LINUX__
#endif
#if defined(__USE_UNIX__)
#undef __USE_UNIX__
#endif

//ENABLE_* manually selected macro,must be platform suited
#if defined(ENABLE_WINDOWS)
#if __has_include(<windows.h>) && defined(__MINGW32__)
#define __USE_WINDOWS__
#else
#error "there's no windows sdk"
#endif
#elif defined(ENABLE_UNIX)
#if __has_include(<unistd.h>) && defined(__unix__)
#define __USE_UNIX__
#else
#error "there's no POSIX headers"
#endif
#elif defined(ENABLE_LINUX)
#if __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#define __USE_LINUX__
#else
#error "there's no linux specific headers"
#endif
#endif

/* ===== INTRINSIC HEADER ===== */
//note: must be in scope before the waitpkg probe, so that _umonitor/_umwait are
//declared. what actually gates the waitpkg routines is -mwaitpkg, which defines
//__WAITPKG__ and turns the target builtins on. __has_builtin alone goes true as
//soon as <immintrin.h> is in scope, which would let the switch through and then
//fail at the call site with "target specific option mismatch", so it is checked
//together with the target macro.
#include <immintrin.h>

#if defined(ENABLE_WAITPKG)
#if defined(__WAITPKG__) && __has_builtin(__builtin_ia32_umwait) &&                                 \
    __has_builtin(__builtin_ia32_umonitor)
#define __USE_WAITPKG__
#pragma message "using waitpkg intrinsics"
#else
#error "making sure you specified -mwaitpkg option"
#endif
#endif

//cond4: auto select when nothing was chosen by hand
#if !defined(ENABLE_WINDOWS) && !defined(ENABLE_WAITPKG) && !defined(ENABLE_LINUX) &&               \
    !defined(ENABLE_UNIX)
#if defined(__WAITPKG__) && __has_builtin(__builtin_ia32_umwait) &&                                 \
    __has_builtin(__builtin_ia32_umonitor)
#define __USE_WAITPKG__
#endif
//note: waitpkg sits in parallel with the system api
#if defined(__unix__)
#define __USE_UNIX__
#elif __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#define __USE_LINUX__
#elif __has_include(<windows.h>) && defined(__MINGW32__)
#define __USE_WINDOWS__
#else
#error "can not select the platform api"
#endif
#endif

/* ===== system api availability ===== */
#if defined(__USE_UNIX__) || defined(__USE_WINDOWS__) || defined(__USE_LINUX__)
#define RPC_CLIENT_HAVE_SYSTEM_API 1
#endif


/* ===== HEADERS ===== */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "umonitor_check.h"
#ifdef __USE_WINDOWS__
#include <windows.h>
#endif
#ifdef __USE_UNIX__
#include <pthread.h>
#include <unistd.h>
#endif

/* ===== UTILITY MACROS ===== */
#ifdef RPC_CLIENT_DEBUG
#define LOG(content, ...) printf(content "\n" __VA_OPT__(,) __VA_ARGS__);
#else
#define LOG(content, ...)
#endif
#ifndef assert
#define assert(cond, content)                                                                      \
  if (cond) {                                                                                      \
  } else {                                                                                         \
    printf(content "\n");                                                                          \
    exit(-1);                                                                                      \
  }
#else
#error "please remove #include <assert.h>"
#endif
#define PRIVATE_FUNC static


/* ========== waiter injection ========== */
//quickdoc: client_specific_waiter is used by the _system routines. it receives
//an address and listens for modification of that 1byte region. if it is not
//set, the _system routines cannot block and bail out rather than spinning.
//private linkage (static), inject through set_client_specific_waiter.
typedef void (*rpc_client_ssw_t)(void *addr);

static rpc_client_ssw_t client_specific_waiter = nullptr;

void set_client_specific_waiter(void *target) {
  client_specific_waiter = (rpc_client_ssw_t)(target);
  return;
}


/* ========== protocol abstraction routine ========== */
//quickdoc: abstract hello handshake memlayout structure, stage 1
typedef struct [[gnu::packed]] _rpc_client_handshake_t {
  char hello_message;
  char cap_mask;
  char _1_length;
  char _2_length;
  char _3_length;
  char _4_length;
} rpc_client_handshake_t;

//quickdoc: abstract dispatch handshake memlayout structure, stage 2
typedef struct [[gnu::packed]] _rpc_client_dispatch_handshake_t {
  char flag_lowaddr;
  char flag_highaddr;
  unsigned short split_number;
  unsigned int bytes_per_split;
} rpc_client_dispatch_handshake_t;

//quickdoc: function kinds, numbered as common/.draft numbers them
enum rpc_client_dispatch_type : char {
  RPC_CLIENT_HUGE_SHM = (char)0x0,
  RPC_CLIENT_SHM = (char)0x1,
  RPC_CLIENT_FUNC = (char)0x2
};

//quickdoc: capability mask bits, as common/.draft defines them
enum rpc_client_capability : char {
  RPC_CLIENT_CAP_UMONITOR = (char)0x1, //bit0 waitpkg / umonitor support
  RPC_CLIENT_CAP_PHYS = (char)0x2,     //bit1 physical address passthrough
  RPC_CLIENT_CAP_ERROR = (char)0x4     //bit2 client error return, host clears it
};

//quickdoc: dispatch handshake error codes, stage 2
enum rpc_client_dispatch_error : char {
  RPC_CLIENT_DISP_OK = (char)0x0,
  RPC_CLIENT_DISP_ILLEGAL_FUNC_COUNT = (char)0x1,
  RPC_CLIENT_DISP_ILLEGAL_SPACE_COUNT = (char)0x2
};

struct [[gnu::packed]] __client_disp_list;
//quickdoc: handler for a FUNC slot. self carries the slot, the caller owns it
typedef void (*rpc_client_dispatch_func_t)(struct __client_disp_list *self);

struct [[gnu::packed]] __client_disp_list {
  rpc_client_dispatch_func_t dispatch_function; //nullptr for SHM / HUGE_SHM
  unsigned int length;                          //include flag
  void *base_addr;                              //include flag
  enum rpc_client_dispatch_type d_t;
  struct __client_disp_list *next;
};

//quickdoc: the discovered function table, one node per slot
typedef struct [[gnu::packed]] _rpc_client_dispatch_t {
  struct __client_disp_list *head;
  struct __client_disp_list *end;
  unsigned int total_length;
  unsigned int bytes_per_split;
  unsigned short split_number;
  void *start_addr;
} rpc_client_dispatch_t;

//quickdoc: note: every out parameter below is owned by the CALLER


/* ========== init ========== */
//quickdoc: fill the hello this client answers with. is_phys and is_umonitor
//decide which capability bits the client advertises back to the host
void rpc_client_handshake_init(rpc_client_handshake_t *out_ptr, bool is_phys, bool is_umonitor,
                               unsigned int length) {
  assert(out_ptr != nullptr, "[ASSERT] wrong param, nullptr");
  //note: no power-of-two requirement here. the draft fixes length as
  //split_number * bytes_per_split, and only bytes_per_split is a power of two;
  //rpc_host.h adds a power-of-two rule on length that the draft does not have.
  assert(length > 0, "[ASSERT] wrong param, hello handshake length must be non zero")
  memset(out_ptr, 0, sizeof(rpc_client_handshake_t));
  out_ptr->hello_message = 1;
  //note: built from the enum above, deliberately NOT copied from rpc_host.h.
  //the host writes these as octal literals (00000010 == 8), which lands
  //PhysAddr on bit3 instead of bit1.
  if (is_umonitor)
    out_ptr->cap_mask |= RPC_CLIENT_CAP_UMONITOR;
  if (is_phys)
    out_ptr->cap_mask |= RPC_CLIENT_CAP_PHYS;
  unsigned int *length_ptr = (unsigned int *)(&(out_ptr->_1_length));
  *length_ptr = length;
}

//quickdoc: init the function table. length is the whole address space and
//host_addr is its base; where they came from is the caller's business
void rpc_client_dispatch_init(rpc_client_dispatch_t *hdt_out, void *host_addr, unsigned int length) {
  assert(hdt_out != nullptr && host_addr != nullptr, "[ASSERT] wrong param, nullptr");
  //note: see rpc_client_handshake_init -- the draft puts no power-of-two rule
  //on the address space length
  assert(length > 0, "[ASSERT] wrong param, dispatch length must be non zero")
  memset(hdt_out, 0, sizeof(rpc_client_dispatch_t));
  hdt_out->total_length = length;
  hdt_out->start_addr = host_addr;
  hdt_out->end = nullptr;
  hdt_out->head = nullptr;
  return;
}


/* ========== table maintenance ========== */
//quickdoc: append one discovered slot. len includes the flag
void rpc_client_dispatch_append_node(rpc_client_dispatch_t *hdt_out, enum rpc_client_dispatch_type dt,
                                     void *base_addr, unsigned int len,
                                     rpc_client_dispatch_func_t df) {
  assert(hdt_out != nullptr && base_addr != nullptr, "[ASSERT] wrong param, nullptr");
  assert(len != 0, "[ASSERT] wrong param, incorrect len");
  struct __client_disp_list *node = malloc(sizeof(struct __client_disp_list));
  assert(node != nullptr, "[ASSERT] allocation failed");
  node->length = len;
  node->base_addr = base_addr;
  node->d_t = dt;
  node->next = nullptr;
  if (dt == RPC_CLIENT_FUNC) {
    node->dispatch_function = df;
  } else {
    node->dispatch_function = nullptr;
  }

  if (hdt_out->head == nullptr && hdt_out->end == nullptr) {
    hdt_out->head = node;
    hdt_out->end = node;
  } else {
    assert(hdt_out->end != nullptr, "[ASSERT] invalid field");
    hdt_out->end->next = node;
    hdt_out->end = node;
  }
  return;
}

//quickdoc: free the nodes. does NOT free hdt_out itself, the caller owns it.
//it never touches the Address Space, which the caller owns too
void rpc_client_dispatch_destroy_node(rpc_client_dispatch_t *hdt_out) {
  assert(hdt_out != nullptr, "[ASSERT] wrong param, nullptr");
  if (hdt_out->head == nullptr) {
    LOG("[WARN] invalid HEAD");
    return;
  }
  struct __client_disp_list *cur_node = hdt_out->head;
  struct __client_disp_list *tmp_prev_node = nullptr;
  while (true) {
    if (cur_node == nullptr) {
      break;
    }
    tmp_prev_node = cur_node;
    cur_node = cur_node->next;
    free(tmp_prev_node);
  }
  hdt_out->head = nullptr;
  hdt_out->end = nullptr;
  return;
}

/*
  quickdoc: throw away a half-built table. used by the refusal paths in
  rpc_client_discover_dispatch, so a refused layout leaves the caller with an
  empty table rather than a partial node list
*/
PRIVATE_FUNC void rpc_client_dispatch_refuse(rpc_client_dispatch_t *hdt_out) {
  if (hdt_out->head != nullptr) {
    rpc_client_dispatch_destroy_node(hdt_out);
  }
  memset(hdt_out, 0, sizeof *hdt_out);
}

/*
  quickdoc: walk the dispatched address space and build the function table.
  this is the heart of stage 2 -- the client asks the host for nothing here, it
  derives every slot offset from what the dispatch handshake told it
  (split_number, bytes_per_split).

  a slot's span is one Bytes per Split, except for HUGE_SHM, whose span the
  draft stores in its body at +2. the walk must tile the whole address space
  exactly -- the draft's closing requirement that every function's flag and size
  adds up to the space length.

  returns 1   ok
  returns 0   the layout was refused: an impossible Bytes per Split, splits that
              do not multiply out to the space length, or a HUGE_SHM that does
              not declare a usable span
  returns -1  nothing was dispatched

  on anything other than 1 the table is left empty -- no half-built node list
  for the caller to trip over. the structure is reset on entry, so a caller
  reusing one must destroy any previous table first.
*/
int rpc_client_discover_dispatch(rpc_client_dispatch_t *hdt_out,
                                 rpc_client_dispatch_handshake_t *hdh_out, void *host_addr,
                                 unsigned int length) {
  assert(hdt_out != nullptr && hdh_out != nullptr && host_addr != nullptr,
         "[ASSERT] wrong param, nullptr");
  //a refused layout must leave the caller with no table at all, rather than
  //whatever the structure happened to hold before
  memset(hdt_out, 0, sizeof *hdt_out);
  if (hdh_out->split_number < 1) {
    LOG("[ERROR] nothing was dispatched");
    return -1;
  }
  if (hdh_out->bytes_per_split < 8) {
    LOG("[ERROR] bytes per split is too small");
    return 0;
  }
  if ((unsigned int)hdh_out->split_number * hdh_out->bytes_per_split != length) {
    LOG("[ERROR] split number * bytes per split != length");
    return 0;
  }
  rpc_client_dispatch_init(hdt_out, host_addr, length);
  hdt_out->split_number = hdh_out->split_number;
  hdt_out->bytes_per_split = hdh_out->bytes_per_split;

  char *addr = host_addr;
  unsigned int offset = 0;
  while (offset < length) {
    char *slot = addr + offset;
    char code = __atomic_load_n(slot + 1, __ATOMIC_ACQUIRE);
    enum rpc_client_dispatch_type dt = (enum rpc_client_dispatch_type)code;

    unsigned int span = hdh_out->bytes_per_split;
    if (dt == RPC_CLIENT_HUGE_SHM) {
      //the draft stores the whole node length here, flag included, and requires
      //every node to cover a whole number of Splits.
      //
      //a size of zero means the host never published one. falling back to a
      //single split would hand back a table that silently breaks a multi-Split
      //node into several -- exactly the defect this walk exists to avoid -- so
      //the layout is refused instead of reported as understood.
      unsigned int size = __atomic_load_n((unsigned int *)(slot + 2), __ATOMIC_ACQUIRE);
      if (size < hdh_out->bytes_per_split || (size % hdh_out->bytes_per_split) != 0 ||
          size > (length - offset)) {
        LOG("[ERROR] a HUGE_SHM does not declare a usable span");
        rpc_client_dispatch_refuse(hdt_out);
        return 0;
      }
      span = size;
    }
    //belt and braces. with bps >= 8, length == splits * bps and every span
    //forced to a whole number of Splits, this cannot fire as the code stands.
    //it is kept on purpose: this walk reads memory another process wrote, and
    //a bound check is the wrong thing to delete for tidiness if the tiling
    //precondition is ever relaxed.
    if (span > (length - offset)) {
      LOG("[ERROR] a slot overruns the address space");
      rpc_client_dispatch_refuse(hdt_out);
      return 0;
    }
    rpc_client_dispatch_append_node(hdt_out, dt, slot, span, nullptr);
    offset += span;
  }
  //belt and braces, for the same reason as the bound check above
  if (offset != length) {
    LOG("[ERROR] the slots do not tile the address space");
    rpc_client_dispatch_refuse(hdt_out);
    return 0;
  }
  LOG("[INFO] dispatch discovered");
  return 1;
}


/* ========== the only thing the two backends do differently ========== */
/*
  quickdoc: park on that 1byte region, then let the caller re-read it.

  note: park BEFORE checking, never the other way round. checking first and
  parking after leaves a window in which the host raises the gate between the
  two, and the client then parks on an event that already happened.
*/
PRIVATE_FUNC void rpc_client_gate_park_umonitor(char *flag) {
#ifdef __USE_WAITPKG__
  _umonitor(flag);
  _umwait(1, 0);
#else
  (void)flag;
#endif
}

PRIVATE_FUNC void rpc_client_gate_park_system(char *flag) {
  if (!client_specific_waiter) {
    assert(false, "[ASSERT] no client waiter injected, see set_client_specific_waiter");
  }
  client_specific_waiter(flag);
}

//quickdoc: return once the host has raised the Gate at flag
PRIVATE_FUNC void rpc_client_wait_until_gate_up(char *flag, bool umonitor_backend) {
  do {
    if (umonitor_backend) {
      rpc_client_gate_park_umonitor(flag);
    } else {
      rpc_client_gate_park_system(flag);
    }
  } while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE));
}


/* ========== protocol processing routine, stage 1 ========== */
extern int rpc_client_run_hello_handshake_system(rpc_client_handshake_t *hh_out, void *addr);

/*
  quickdoc: the Hello DIALOG itself, shared by both backends.

  the client waits for the host to raise the gate, validates what the host
  published, answers with its capability mask, then lowers the gate. nothing is
  written before the gate is seen, so the host never observes a half reply.

  returns 1   ok
  returns 0   this client rejected the hello (error bit set for the host)
*/
PRIVATE_FUNC int rpc_client_hello_handshake_common(rpc_client_handshake_t *hh_out, void *addr,
                                                   bool umonitor_backend) {
  char *flag_addr = addr;
  char *cap_mask_addr = addr + 1;
  unsigned int *host_length_ptr = (unsigned int *)(addr + 2);
  unsigned int *own_length_ptr = (unsigned int *)(&(hh_out->_1_length));

  rpc_client_wait_until_gate_up(flag_addr, umonitor_backend);

  char hello_message = __atomic_load_n((char *)addr, __ATOMIC_ACQUIRE);
  char host_cap_mask = __atomic_load_n(cap_mask_addr, __ATOMIC_ACQUIRE);
  unsigned int host_length = __atomic_load_n(host_length_ptr, __ATOMIC_ACQUIRE);

  bool rejected = false;
  if (hello_message != 1) {
    LOG("[ERROR] incorrect hello message");
    rejected = true;
  }
  //the draft requires the client to check the accuracy of the capability mask:
  //the host leaves the error bit clear, and bits 3-7 are reserved
  if ((host_cap_mask & ~(RPC_CLIENT_CAP_UMONITOR | RPC_CLIENT_CAP_PHYS)) != 0) {
    LOG("[ERROR] host capability mask is not well formed");
    rejected = true;
  }
  if (*own_length_ptr != host_length) {
    LOG("[ERROR] address space length mismatch");
    rejected = true;
  }
  if (rejected) {
    __atomic_store_n(cap_mask_addr, (char)(hh_out->cap_mask | RPC_CLIENT_CAP_ERROR),
                     __ATOMIC_RELEASE);
    __atomic_store_n(flag_addr, 0, __ATOMIC_RELEASE);
    return 0;
  }
  __atomic_store_n(cap_mask_addr, hh_out->cap_mask, __ATOMIC_RELEASE);
  __atomic_store_n(flag_addr, 0, __ATOMIC_RELEASE);
  return 1;
}

/*
  quickdoc: hello handshake, waitpkg entry.
  returns 1 ok, 0 rejected, -1 not compiled in
*/
int rpc_client_run_hello_handshake_umonitor(rpc_client_handshake_t *hh_out, void *addr) {
#ifdef __USE_WAITPKG__
  assert(hh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU,rolling back");
    return rpc_client_run_hello_handshake_system(hh_out, addr);
  }
  return rpc_client_hello_handshake_common(hh_out, addr, true);
#else
  (void)hh_out;
  (void)addr;
  return -1;
#endif
}

/*
  quickdoc: hello handshake, system entry. needs an injected waiter; without one
  this cannot block and bails out like the host's does.
*/
int rpc_client_run_hello_handshake_system(rpc_client_handshake_t *hh_out, void *addr) {
#ifdef RPC_CLIENT_HAVE_SYSTEM_API
  assert(hh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  return rpc_client_hello_handshake_common(hh_out, addr, false);
#else
  (void)hh_out;
  (void)addr;
  return -1;
#endif
}

/*
  quickdoc: stage 1 auto router. prefer waitpkg when it was compiled in AND the
  running cpu has it. only the umonitor routine rolls back; the router just
  picks the entry, and the same cpuid question may end up asked twice.
*/
int rpc_client_run_hello_handshake(rpc_client_handshake_t *hh_out, void *addr) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    LOG("[INFO] prefer waitpkg,switching to waitpkg routine");
    return rpc_client_run_hello_handshake_umonitor(hh_out, addr);
  }
#endif
  return rpc_client_run_hello_handshake_system(hh_out, addr);
}


/* ========== protocol processing routine, stage 2 ========== */
extern int rpc_client_run_dispatch_handshake_system(rpc_client_dispatch_handshake_t *hdh_out,
                                                    void *addr);

/*
  quickdoc: the Dispatch DIALOG itself, shared by both backends.

  returns 1   ok, layout kept in hdh_out
  returns 0   ILLEGAL FUNC COUNT
  returns -1  ILLEGAL SPACE COUNT
  returns -2  a reserved error code -- defensive, the client authors the code
              itself, so only a future out-of-band code can reach it
*/
PRIVATE_FUNC int rpc_client_dispatch_handshake_common(rpc_client_dispatch_handshake_t *hdh_out,
                                                      void *addr, bool umonitor_backend) {
  char *flag_addr = addr;
  char *error_code_addr = addr + 1;
  unsigned short *split_ptr = (unsigned short *)(addr + 2);
  unsigned int *bps_ptr = (unsigned int *)(addr + 4);

  //a rejected handshake must leave the caller with no layout at all -- including
  //none left over from an earlier handshake in the same structure
  memset(hdh_out, 0, sizeof *hdh_out);

  rpc_client_wait_until_gate_up(flag_addr, umonitor_backend);

  unsigned short host_split_number = __atomic_load_n(split_ptr, __ATOMIC_ACQUIRE);
  unsigned int host_bytes_per_split = __atomic_load_n(bps_ptr, __ATOMIC_ACQUIRE);

  char error_code = RPC_CLIENT_DISP_OK;
  if (host_split_number < 1) {
    error_code = RPC_CLIENT_DISP_ILLEGAL_FUNC_COUNT;
  } else if (host_bytes_per_split < 8) {
    error_code = RPC_CLIENT_DISP_ILLEGAL_SPACE_COUNT;
  } else {
    hdh_out->split_number = host_split_number;
    hdh_out->bytes_per_split = host_bytes_per_split;
  }
  __atomic_store_n(error_code_addr, error_code, __ATOMIC_RELEASE);
  __atomic_store_n(flag_addr, 0, __ATOMIC_RELEASE);

  switch (error_code) {
  case RPC_CLIENT_DISP_OK:
    return 1;
  case RPC_CLIENT_DISP_ILLEGAL_FUNC_COUNT:
    return 0;
  case RPC_CLIENT_DISP_ILLEGAL_SPACE_COUNT:
    return -1;
  default:
    LOG("[WARN] receives an incorrect reserved code");
    return -2;
  }
}

//quickdoc: dispatch handshake, waitpkg entry. returns -3 when not compiled in
int rpc_client_run_dispatch_handshake_umonitor(rpc_client_dispatch_handshake_t *hdh_out,
                                               void *addr) {
#ifdef __USE_WAITPKG__
  assert(hdh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU,rolling back");
    return rpc_client_run_dispatch_handshake_system(hdh_out, addr);
  }
  return rpc_client_dispatch_handshake_common(hdh_out, addr, true);
#else
  (void)hdh_out;
  (void)addr;
  return -3;
#endif
}

//quickdoc: dispatch handshake, system entry. see the hello system entry
int rpc_client_run_dispatch_handshake_system(rpc_client_dispatch_handshake_t *hdh_out, void *addr) {
#ifdef RPC_CLIENT_HAVE_SYSTEM_API
  assert(hdh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  return rpc_client_dispatch_handshake_common(hdh_out, addr, false);
#else
  (void)hdh_out;
  (void)addr;
  return -3;
#endif
}

//quickdoc: stage 2 auto router, see rpc_client_run_hello_handshake
int rpc_client_run_dispatch_handshake(rpc_client_dispatch_handshake_t *hdh_out, void *addr) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    LOG("[INFO] prefer waitpkg,switching to waitpkg routine");
    return rpc_client_run_dispatch_handshake_umonitor(hdh_out, addr);
  }
#endif
  return rpc_client_run_dispatch_handshake_system(hdh_out, addr);
}


/* ========== protocol processing routine, stage 3 ========== */
/*
  quickdoc: service one slot whose Gate the host has already raised.

  the shared body: read the slot's kind from its head, run the handler for a
  FUNC, record the published size for a HUGE_SHM, then lower the Gate so the
  host can proceed. SHM and HUGE_SHM have no handler -- the caller reads the
  shared region once the Gate is down.
*/
PRIVATE_FUNC void rpc_client_slot_service_body(struct __client_disp_list *node) {
  char *flag = node->base_addr;
  char *function_code = (node->base_addr) + 1;

  char code = __atomic_load_n(function_code, __ATOMIC_ACQUIRE);
  node->d_t = (enum rpc_client_dispatch_type)code;
  if (node->d_t == RPC_CLIENT_FUNC) {
    assert(node->dispatch_function != nullptr, "[ASSERT] no handler registered for FUNC slot");
    (node->dispatch_function)(node);
  } else if (node->d_t == RPC_CLIENT_HUGE_SHM) {
    node->length = __atomic_load_n((unsigned int *)((node->base_addr) + 2), __ATOMIC_ACQUIRE);
    LOG("[INFO] found HUGE_SHM node");
  } else {
    LOG("[INFO] found SHM node");
  }

  //lower the gate, the host is waiting on it
  __atomic_store_n(flag, 0, __ATOMIC_RELEASE);
}

extern int rpc_client_service_slot_system(struct __client_disp_list *node);

//quickdoc: service one slot, parking until the host raises its Gate.
//returns 1 serviced, -1 not compiled in
int rpc_client_service_slot_umonitor(struct __client_disp_list *node) {
#ifdef __USE_WAITPKG__
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU,rolling back");
    return rpc_client_service_slot_system(node);
  }
  rpc_client_wait_until_gate_up(node->base_addr, true);
  rpc_client_slot_service_body(node);
  return 1;
#else
  (void)node;
  return -1;
#endif
}

//quickdoc: service one slot, parking until the host raises its Gate.
//returns 1 serviced, -1 not compiled in
int rpc_client_service_slot_system(struct __client_disp_list *node) {
#ifdef RPC_CLIENT_HAVE_SYSTEM_API
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  rpc_client_wait_until_gate_up(node->base_addr, false);
  rpc_client_slot_service_body(node);
  return 1;
#else
  (void)node;
  return -1;
#endif
}

//quickdoc: stage 3 auto router, see rpc_client_run_hello_handshake
int rpc_client_service_slot(struct __client_disp_list *node) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    LOG("[INFO] prefer waitpkg,switching to waitpkg routine");
    return rpc_client_service_slot_umonitor(node);
  }
#endif
  return rpc_client_service_slot_system(node);
}

/*
  quickdoc: service one slot only if the host has already raised its Gate.

  this never parks, so a slot the host leaves alone cannot hold up the others.

  returns 1 serviced, 0 the Gate was down
*/
int rpc_client_service_slot_if_ready(struct __client_disp_list *node) {
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  if (!__atomic_load_n((char *)node->base_addr, __ATOMIC_ACQUIRE)) {
    return 0;
  }
  rpc_client_slot_service_body(node);
  return 1;
}

/*
  quickdoc: one pass over the whole table, servicing every slot whose Gate is
  up. every slot gets its turn -- none of them can block another.

  returns how many slots were serviced in this pass
*/
unsigned int rpc_client_service_ready_slots(rpc_client_dispatch_t *hdt_out) {
  assert(hdt_out != nullptr, "[ASSERT] wrong param, nullptr");
  unsigned int serviced = 0;
  struct __client_disp_list *cur_node = hdt_out->head;
  while (cur_node != nullptr) {
    if (rpc_client_service_slot_if_ready(cur_node)) {
      serviced++;
    }
    cur_node = cur_node->next;
  }
  return serviced;
}

/*
  quickdoc: the node at position index, or nullptr if the table is shorter.
  callers use this to register handlers after discovery.
*/
struct __client_disp_list *rpc_client_dispatch_node_at(rpc_client_dispatch_t *hdt_out,
                                                       unsigned int index) {
  assert(hdt_out != nullptr, "[ASSERT] wrong param, nullptr");
  struct __client_disp_list *cur_node = hdt_out->head;
  while (index-- > 0 && cur_node != nullptr) {
    cur_node = cur_node->next;
  }
  return cur_node;
}

/*
  quickdoc: register the handler for a FUNC slot.

  discovery cannot know the application's handlers, so it leaves every node's
  dispatch_function null. a caller that intends to service a FUNC slot must
  register one here first, otherwise servicing that slot aborts rather than
  silently skipping the call.
*/
void rpc_client_dispatch_set_handler(struct __client_disp_list *node,
                                     rpc_client_dispatch_func_t fn) {
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  node->dispatch_function = fn;
  return;
}

/*
  quickdoc: keep making passes over the table, servicing whatever is ready.

  this does not return for a table with slots in it, and it deliberately does
  not park between passes. it cannot: the injected waiter listens on a single
  address, and no single address covers several slots, so parking on any one of
  them would let that slot starve every other one. callers who want pacing
  should drive rpc_client_service_ready_slots themselves rather than call this.

  an empty table returns immediately without touching anything.
*/
void rpc_client_run_dispatch(rpc_client_dispatch_t *hdt_out) {
  assert(hdt_out != nullptr && hdt_out->start_addr != nullptr && hdt_out->total_length != 0,
         "[ASSERT] wrong param, nullptr");
  if (hdt_out->head == nullptr) {
    LOG("[INFO] dispatch list is empty");
    return;
  }
  while (true) {
    rpc_client_service_ready_slots(hdt_out);
  }
}
