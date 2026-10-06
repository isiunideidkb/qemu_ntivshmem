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
    stage 3  function  wait for a slot's gate, service the call, lower the gate

  Relationship to the host header: this file deliberately does NOT reuse the
  host_*_t types or the host routines. It mirrors rpc_host.h's shape under the
  rpc_client_* prefix, and follows common/.draft as the protocol authority.

  todo list:
  1. discovery: common/.draft fixes that every function starts on a split
     boundary, but never says how a HUGE_SHM node's span is learned before the
     host publishes its size at +2 -- rpc_host.h publishes it only while
     running the node, not at dispatch. This implementation therefore walks one
     split at a time and records what each split's head carries; a HUGE_SHM
     span wider than one split is not reconstructed yet. Needs the author.
  2. stage 3 servicing is per-node here, mirroring the host's thread-per-node
     runner. A single polling loop over rpc_client_dispatch_t would be simpler
     and is probably what the client wants; not done yet.
  3. the _system routines need set_client_specific_waiter() to have been
     called, otherwise they bail out like the host's do.
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
//quickdoc: client_specific_waiter is used by the rpc_client_run_*_system
//routines. it receives an address and listens for modification of that 1byte
//region. if it is not set, the _system routines cannot block and bail out.
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

//quickdoc: free the nodes. does NOT free hdt_out itself, the caller owns it
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
  quickdoc: walk the dispatched address space split by split and build the
  function table. this is the heart of stage 2 -- the client asks the host for
  nothing here, it derives every slot offset from what the dispatch handshake
  told it (split_number, bytes_per_split).

  returns 1   ok
  returns 0   the layout does not add up
  returns -1  nothing was found
*/
int rpc_client_discover_dispatch(rpc_client_dispatch_t *hdt_out,
                                 rpc_client_dispatch_handshake_t *hdh_out, void *host_addr,
                                 unsigned int length) {
  assert(hdt_out != nullptr && hdh_out != nullptr && host_addr != nullptr,
         "[ASSERT] wrong param, nullptr");
  assert(hdh_out->split_number >= 1, "[ASSERT] wrong param, incorrect split number")
  assert(hdh_out->bytes_per_split >= 8, "[ASSERT] wrong param, incorrect bytes_per_split")
  if ((unsigned int)hdh_out->split_number * hdh_out->bytes_per_split != length) {
    LOG("[ERROR] split number * bytes per split != length");
    return 0;
  }
  rpc_client_dispatch_init(hdt_out, host_addr, length);
  hdt_out->split_number = hdh_out->split_number;
  hdt_out->bytes_per_split = hdh_out->bytes_per_split;

  char *addr = host_addr;
  for (unsigned short i = 0; i < hdh_out->split_number; ++i) {
    char *slot = addr + ((unsigned int)i * hdh_out->bytes_per_split);
    char code = __atomic_load_n(slot + 1, __ATOMIC_ACQUIRE);
    enum rpc_client_dispatch_type dt = (enum rpc_client_dispatch_type)code;
    rpc_client_dispatch_append_node(hdt_out, dt, slot, hdh_out->bytes_per_split, nullptr);
  }
  if (hdt_out->head == nullptr) {
    return -1;
  }
  LOG("[INFO] dispatch discovered");
  return 1;
}


/* ========== protocol processing routine, stage 1 ========== */
//note: the following routines are platform dependent
extern int rpc_client_run_hello_handshake_system(rpc_client_handshake_t *hh_out, void *addr);

/*
  quickdoc: hello handshake, waitpkg entry.

  the client waits for the host to raise the gate, validates what the host
  published, answers with its capability mask, then lowers the gate. nothing is
  written before the gate is seen, so the host never observes a half reply.

  returns 1   ok
  returns 0   this client rejected the hello (error bit set for the host)
  returns -1  not compiled in
*/
int rpc_client_run_hello_handshake_umonitor(rpc_client_handshake_t *hh_out, void *addr) {
#ifdef __USE_WAITPKG__
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU,rolling back");
    return rpc_client_run_hello_handshake_system(hh_out, addr);
  }
  assert(hh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  char *flag_addr = addr;
  char *cap_mask_addr = addr + 1;
  unsigned int *host_length_ptr = (unsigned int *)(addr + 2);
  unsigned int *own_length_ptr = (unsigned int *)(&(hh_out->_1_length));

  //wait host
waiting:
  _umonitor(flag_addr);
  _umwait(1, 0);
  if (!__atomic_load_n(flag_addr, __ATOMIC_ACQUIRE))
    goto waiting;

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
#else
  return -1;
#endif
}

/*
  quickdoc: hello handshake, system entry. needs an injected waiter; without
  one this cannot block and bails out like the host's does.
*/
int rpc_client_run_hello_handshake_system(rpc_client_handshake_t *hh_out, void *addr) {
#if defined(__USE_UNIX__) || defined(__USE_WINDOWS__) || defined(__USE_LINUX__)
  assert(hh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  if (!client_specific_waiter) {
    assert(false, "[ASSERT] no client waiter injected, see set_client_specific_waiter");
  }
  char *flag_addr = addr;
  char *cap_mask_addr = addr + 1;
  unsigned int *host_length_ptr = (unsigned int *)(addr + 2);
  unsigned int *own_length_ptr = (unsigned int *)(&(hh_out->_1_length));

waiting:
  client_specific_waiter(flag_addr);
  if (!__atomic_load_n(flag_addr, __ATOMIC_ACQUIRE))
    goto waiting;

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
#else
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
  quickdoc: dispatch handshake, waitpkg entry. same shape as stage 1: wait for
  the gate, read what the host dispatched, answer with the error code, lower
  the gate.

  returns 1   ok
  returns 0   ILLEGAL FUNC COUNT
  returns -1  ILLEGAL SPACE COUNT
  returns -2  a reserved error code was observed
  returns -3  not compiled in
*/
int rpc_client_run_dispatch_handshake_umonitor(rpc_client_dispatch_handshake_t *hdh_out,
                                               void *addr) {
#ifdef __USE_WAITPKG__
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU,rolling back");
    return rpc_client_run_dispatch_handshake_system(hdh_out, addr);
  }
  assert(hdh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  char *flag_addr = addr;
  char *error_code_addr = addr + 1;

waiting:
  _umonitor(flag_addr);
  _umwait(1, 0);
  if (!__atomic_load_n(flag_addr, __ATOMIC_ACQUIRE))
    goto waiting;

  unsigned short host_split_number = __atomic_load_n((unsigned short *)(addr + 2), __ATOMIC_ACQUIRE);
  unsigned int host_bytes_per_split = __atomic_load_n((unsigned int *)(addr + 4), __ATOMIC_ACQUIRE);

  char error_code = 0x0;
  if (host_split_number < 1) {
    error_code = 0x1; //ILLEGAL FUNC COUNT
  } else if (host_bytes_per_split < 8) {
    error_code = 0x2; //ILLEGAL SPACE COUNT
  } else {
    hdh_out->split_number = host_split_number;
    hdh_out->bytes_per_split = host_bytes_per_split;
  }
  __atomic_store_n(error_code_addr, error_code, __ATOMIC_RELEASE);
  __atomic_store_n(flag_addr, 0, __ATOMIC_RELEASE);

  if (error_code == 0x0) {
    return 1;
  } else if (error_code == 0x1) {
    return 0;
  } else if (error_code == 0x2) {
    return -1;
  }
  LOG("[WARN] receives an incorrect reserved code");
  return -2;
#else
  return -3;
#endif
}

//quickdoc: dispatch handshake, system entry. see the hello system entry
int rpc_client_run_dispatch_handshake_system(rpc_client_dispatch_handshake_t *hdh_out, void *addr) {
#if defined(__USE_UNIX__) || defined(__USE_WINDOWS__) || defined(__USE_LINUX__)
  assert(hdh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  if (!client_specific_waiter) {
    assert(false, "[ASSERT] no client waiter injected, see set_client_specific_waiter");
  }
  char *flag_addr = addr;
  char *error_code_addr = addr + 1;

waiting:
  client_specific_waiter(flag_addr);
  if (!__atomic_load_n(flag_addr, __ATOMIC_ACQUIRE))
    goto waiting;

  unsigned short host_split_number = __atomic_load_n((unsigned short *)(addr + 2), __ATOMIC_ACQUIRE);
  unsigned int host_bytes_per_split = __atomic_load_n((unsigned int *)(addr + 4), __ATOMIC_ACQUIRE);

  char error_code = 0x0;
  if (host_split_number < 1) {
    error_code = 0x1; //ILLEGAL FUNC COUNT
  } else if (host_bytes_per_split < 8) {
    error_code = 0x2; //ILLEGAL SPACE COUNT
  } else {
    hdh_out->split_number = host_split_number;
    hdh_out->bytes_per_split = host_bytes_per_split;
  }
  __atomic_store_n(error_code_addr, error_code, __ATOMIC_RELEASE);
  __atomic_store_n(flag_addr, 0, __ATOMIC_RELEASE);

  if (error_code == 0x0) {
    return 1;
  } else if (error_code == 0x1) {
    return 0;
  } else if (error_code == 0x2) {
    return -1;
  }
  LOG("[WARN] receives an incorrect reserved code");
  return -2;
#else
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
  quickdoc: service one function slot until it is taken away.

  mirrors the host's runner, inverted: the client waits for the host to raise
  the slot's gate, does whatever the slot's kind calls for, then lowers the
  gate. there is no handler for SHM / HUGE_SHM -- the caller reads the shared
  region once the routine has lowered the gate; for FUNC the registered handler
  runs in between.

  this is the never-return body the caller is expected to run per slot.
*/
PRIVATE_FUNC void *rpc_client_internal_dispatch_runner_umonitor(void *_param) {
#ifdef __USE_WAITPKG__
  struct __client_disp_list *node = _param;
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  char *flag = node->base_addr;
  char *function_code = (node->base_addr) + 1;
startup:
  //wait for the host to raise the gate
  _umonitor(flag);
  _umwait(1, 0);
  if (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
    goto startup;

  char code = __atomic_load_n(function_code, __ATOMIC_ACQUIRE);
  node->d_t = (enum rpc_client_dispatch_type)code;
  if (node->d_t == RPC_CLIENT_FUNC) {
    assert(node->dispatch_function != nullptr, "[ASSERT] no handler registered for FUNC slot");
    (node->dispatch_function)(node);
  } else if (node->d_t == RPC_CLIENT_HUGE_SHM) {
    node->length =
        __atomic_load_n((unsigned int *)((node->base_addr) + 2), __ATOMIC_ACQUIRE);
    LOG("[INFO] found HUGE_SHM node");
  } else {
    LOG("[INFO] found SHM node");
  }

  //lower the gate, the host is waiting on it
  __atomic_store_n(flag, 0, __ATOMIC_RELEASE);
  goto startup;
#else
  return nullptr;
#endif
}

//quickdoc: private, the system-side alternative to the umonitor runner
PRIVATE_FUNC void *rpc_client_internal_dispatch_runner_system(void *_param) {
#if defined(__USE_UNIX__) || defined(__USE_WINDOWS__) || defined(__USE_LINUX__)
  struct __client_disp_list *node = _param;
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  if (!client_specific_waiter) {
    assert(false, "[ASSERT] no client waiter injected, see set_client_specific_waiter");
  }
  char *flag = node->base_addr;
  char *function_code = (node->base_addr) + 1;
startup:
  //wait for the host to raise the gate
  client_specific_waiter(flag);
  if (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
    goto startup;

  char code = __atomic_load_n(function_code, __ATOMIC_ACQUIRE);
  node->d_t = (enum rpc_client_dispatch_type)code;
  if (node->d_t == RPC_CLIENT_FUNC) {
    assert(node->dispatch_function != nullptr, "[ASSERT] no handler registered for FUNC slot");
    (node->dispatch_function)(node);
  } else if (node->d_t == RPC_CLIENT_HUGE_SHM) {
    node->length =
        __atomic_load_n((unsigned int *)((node->base_addr) + 2), __ATOMIC_ACQUIRE);
    LOG("[INFO] found HUGE_SHM node");
  } else {
    LOG("[INFO] found SHM node");
  }

  //lower the gate, the host is waiting on it
  __atomic_store_n(flag, 0, __ATOMIC_RELEASE);
  goto startup;
#else
  return nullptr;
#endif
}

//quickdoc: run every discovered slot in its own thread. unlike the host there
//is no auto-router entry for this -- a slot's runner picks its own path, so one
//call services the whole table.
PRIVATE_FUNC void *rpc_client_internal_dispatch_runner(void *_param) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    LOG("[INFO] prefer waitpkg,switching to waitpkg routine");
    return rpc_client_internal_dispatch_runner_umonitor(_param);
  }
#endif
  return rpc_client_internal_dispatch_runner_system(_param);
}

//quickdoc: this dispatches every discovered function through its own runner.
//the caller supplies the threading (pthread_create, CreateThread, or a plain
//loop calling rpc_client_internal_dispatch_runner per node).
void rpc_client_run_dispatch_in_threads(rpc_client_dispatch_t *hd_out) {
  assert(hd_out != nullptr && hd_out->start_addr != nullptr && hd_out->total_length != 0,
         "[ASSERT] wrong param, nullptr");
  if (hd_out->head == nullptr) {
    LOG("[INFO] dispatch list is empty");
    return;
  }
  struct __client_disp_list *cur_node = hd_out->head;
  while (true) {
    if (cur_node == nullptr)
      break;
    rpc_client_internal_dispatch_runner(cur_node);
    cur_node = cur_node->next;
  }
  return;
}
