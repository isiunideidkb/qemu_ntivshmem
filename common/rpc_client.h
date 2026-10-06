/*
  coding style C23 + GCC(MinGW)
  clang-format --style=LLVM
  common rpc protocol for client
  use nullptr instead of NULL

  The client is the PASSIVE side of every DIALOG: the host raises the Gate
  (writes 1), the client does its work and lowers it (writes 0). The client
  never raises a Gate on its own initiative.

    stage 1  hello     wait for the Gate, validate the hello, answer with the
                       capability mask, lower the Gate
    stage 2  dispatch  wait for the Gate, walk the Address Space Split by Split
                       into a function table, lower the Gate
    stage 3  function  service the slots whose Gate the host has raised

  Error handling, deliberately:
    assert  guards undefined behaviour, i.e. states a correct caller cannot
            reach. it reports to the developer and aborts; it is never used to
            validate the peer's data
    return  reports a real failure to the caller. an error that is returned is
            not also logged -- the return value is the warning
    LOG     a warning to the developer, used only where there is no return
            channel to carry it. it is not a journal, so there are no [INFO]
            lines

  Relationship to rpc_host.h: same shape, no shared types. The host_*_t types
  and routines are deliberately not reused; common/.draft is the authority.
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

//the intrinsic header must be in scope before the waitpkg probe, or
//_umonitor/_umwait are not declared. what really gates the routines is
//-mwaitpkg: it defines __WAITPKG__ and enables the target builtins.
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
//quickdoc: the blocking primitive the _system routines use. it receives an
//address and returns once that 1byte region changes. injected, because the
//protocol does not dictate how the process blocks.
typedef void (*rpc_client_ssw_t)(void *addr);

static rpc_client_ssw_t client_specific_waiter = nullptr;

void set_client_specific_waiter(void *target) {
  client_specific_waiter = (rpc_client_ssw_t)(target);
  return;
}


/* ========== protocol abstraction routine ========== */
//quickdoc: hello handshake memlayout, stage 1
typedef struct [[gnu::packed]] _rpc_client_handshake_t {
  char hello_message;
  char cap_mask;
  char _1_length;
  char _2_length;
  char _3_length;
  char _4_length;
} rpc_client_handshake_t;

//quickdoc: dispatch handshake memlayout, stage 2
typedef struct [[gnu::packed]] _rpc_client_dispatch_handshake_t {
  char flag_lowaddr;
  char flag_highaddr;
  unsigned short split_number;
  unsigned int bytes_per_split;
} rpc_client_dispatch_handshake_t;

//quickdoc: function kinds, numbered as the draft numbers them
enum rpc_client_dispatch_type : char {
  RPC_CLIENT_HUGE_SHM = (char)0x0,
  RPC_CLIENT_SHM = (char)0x1,
  RPC_CLIENT_FUNC = (char)0x2
};

//quickdoc: capability mask bits, as the draft defines them
enum rpc_client_capability : char {
  RPC_CLIENT_CAP_UMONITOR = (char)0x1, //bit0 waitpkg / umonitor support
  RPC_CLIENT_CAP_PHYS = (char)0x2,     //bit1 physical address passthrough
  RPC_CLIENT_CAP_ERROR = (char)0x4     //bit2 client error return, host clears it
};

//quickdoc: dispatch error codes, stage 2
enum rpc_client_dispatch_error : char {
  RPC_CLIENT_DISP_OK = (char)0x0,
  RPC_CLIENT_DISP_ILLEGAL_FUNC_COUNT = (char)0x1,
  RPC_CLIENT_DISP_ILLEGAL_SPACE_COUNT = (char)0x2
};

struct [[gnu::packed]] __client_disp_list;
//quickdoc: handler for a FUNC slot
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

//quickdoc: every out parameter below is owned by the CALLER


/* ========== init ========== */
//quickdoc: build the hello this client answers with. is_phys and is_umonitor
//pick the capability bits it advertises back
void rpc_client_handshake_init(rpc_client_handshake_t *out_ptr, bool is_phys, bool is_umonitor,
                               unsigned int length) {
  assert(out_ptr != nullptr, "[ASSERT] wrong param, nullptr");
  //note: no power-of-two rule on the length. the draft fixes it as
  //split_number * bytes_per_split and only bytes_per_split is a power of two
  assert(length > 0, "[ASSERT] wrong param, length must be non zero")
  memset(out_ptr, 0, sizeof(rpc_client_handshake_t));
  out_ptr->hello_message = 1;
  //note: built from the enum, NOT copied from rpc_host.h. the host writes these
  //as octal literals (00000010 == 8), which lands PhysAddr on bit3
  if (is_umonitor)
    out_ptr->cap_mask |= RPC_CLIENT_CAP_UMONITOR;
  if (is_phys)
    out_ptr->cap_mask |= RPC_CLIENT_CAP_PHYS;
  unsigned int *length_ptr = (unsigned int *)(&(out_ptr->_1_length));
  *length_ptr = length;
}

//quickdoc: init an empty table over the Address Space. internal: discover is
//the only thing that builds a table
PRIVATE_FUNC void rpc_client_dispatch_init(rpc_client_dispatch_t *hdt_out, void *host_addr,
                                           unsigned int length) {
  assert(hdt_out != nullptr && host_addr != nullptr, "[ASSERT] wrong param, nullptr");
  assert(length > 0, "[ASSERT] wrong param, length must be non zero")
  memset(hdt_out, 0, sizeof(rpc_client_dispatch_t));
  hdt_out->total_length = length;
  hdt_out->start_addr = host_addr;
}


/* ========== table maintenance ========== */
//quickdoc: append one slot. len includes the flag. internal
PRIVATE_FUNC void rpc_client_dispatch_append_node(rpc_client_dispatch_t *hdt_out,
                                                  enum rpc_client_dispatch_type dt, void *base_addr,
                                                  unsigned int len,
                                                  rpc_client_dispatch_func_t df) {
  assert(hdt_out != nullptr && base_addr != nullptr, "[ASSERT] wrong param, nullptr");
  assert(len != 0, "[ASSERT] wrong param, len is zero");
  struct __client_disp_list *node = malloc(sizeof(struct __client_disp_list));
  assert(node != nullptr, "[ASSERT] allocation failed");
  node->length = len;
  node->base_addr = base_addr;
  node->d_t = dt;
  node->next = nullptr;
  node->dispatch_function = (dt == RPC_CLIENT_FUNC) ? df : nullptr;

  if (hdt_out->head == nullptr && hdt_out->end == nullptr) {
    hdt_out->head = node;
    hdt_out->end = node;
  } else {
    assert(hdt_out->end != nullptr, "[ASSERT] invalid field");
    hdt_out->end->next = node;
    hdt_out->end = node;
  }
}

//quickdoc: free the nodes. never touches the Address Space, the caller owns it
void rpc_client_dispatch_destroy_node(rpc_client_dispatch_t *hdt_out) {
  assert(hdt_out != nullptr, "[ASSERT] wrong param, nullptr");
  //destroying a table that was never built is undefined: discover either
  //produced one or it did not, and the caller is expected to know which
  assert(hdt_out->head != nullptr, "[ASSERT] nothing to destroy, the table is empty");
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
}

//quickdoc: throw a half-built table away, for the refusal paths below
PRIVATE_FUNC void rpc_client_dispatch_refuse(rpc_client_dispatch_t *hdt_out) {
  if (hdt_out->head != nullptr) {
    rpc_client_dispatch_destroy_node(hdt_out);
  }
  memset(hdt_out, 0, sizeof *hdt_out);
}

//quickdoc: the node at position index, or nullptr if the table is shorter
struct __client_disp_list *rpc_client_dispatch_node_at(rpc_client_dispatch_t *hdt_out,
                                                       unsigned int index) {
  assert(hdt_out != nullptr, "[ASSERT] wrong param, nullptr");
  struct __client_disp_list *cur_node = hdt_out->head;
  while (index-- > 0 && cur_node != nullptr) {
    cur_node = cur_node->next;
  }
  return cur_node;
}

//quickdoc: register the handler for a FUNC slot. discovery cannot know it, so
//a FUNC slot that is never registered aborts when serviced
void rpc_client_dispatch_set_handler(struct __client_disp_list *node,
                                     rpc_client_dispatch_func_t fn) {
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  node->dispatch_function = fn;
  return;
}

/*
  quickdoc: walk the Address Space and build the function table. the client asks
  the host for nothing here: it derives every slot offset from the dispatch
  handshake it already took.

  a slot spans one Bytes per Split, except HUGE_SHM, whose whole length the draft
  stores in its body at +2. the walk must tile the space exactly, which is the
  draft's closing requirement.

  returns 1  ok
  returns 0  the host's layout is unusable: an impossible HUGE_SHM span, a slot
             that overruns, or slots that do not tile the space
             on 0, and on -1, the table is left empty
  returns -1 split_number is zero, so nothing was dispatched
*/
int rpc_client_discover_dispatch(rpc_client_dispatch_t *hdt_out,
                                 rpc_client_dispatch_handshake_t *hdh_out, void *host_addr,
                                 unsigned int length) {
  assert(hdt_out != nullptr && hdh_out != nullptr && host_addr != nullptr,
         "[ASSERT] wrong param, nullptr");
  //the handshake already refused these, so reaching here with them is a caller
  //that ignored its verdict -- undefined, not a peer error
  assert(hdh_out->split_number >= 1, "[ASSERT] split_number is zero, nothing was dispatched");
  assert(hdh_out->bytes_per_split >= 8, "[ASSERT] bytes_per_split is too small");
  assert((unsigned int)hdh_out->split_number * hdh_out->bytes_per_split == length,
         "[ASSERT] split_number * bytes_per_split does not match length");

  //a refused layout must leave the caller with no table. init resets the
  //structure, so a caller reusing one must destroy the previous table first
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
      //a zero here means the host never published the size. falling back to one
      //Split would silently break a multi-Split node into several, which is the
      //defect this walk exists to avoid, so the layout is refused instead
      unsigned int size = __atomic_load_n((unsigned int *)(slot + 2), __ATOMIC_ACQUIRE);
      if (size < hdh_out->bytes_per_split || (size % hdh_out->bytes_per_split) != 0 ||
          size > (length - offset)) {
        rpc_client_dispatch_refuse(hdt_out);
        return 0;
      }
      span = size;
    }
    //belt and braces: with the three asserts above satisfied and every span a
    //whole number of Splits, this cannot fire. kept because the walk reads
    //memory another process wrote
    if (span > (length - offset)) {
      rpc_client_dispatch_refuse(hdt_out);
      return 0;
    }
    rpc_client_dispatch_append_node(hdt_out, dt, slot, span, nullptr);
    offset += span;
  }
  //belt and braces, same reason as above
  if (offset != length) {
    rpc_client_dispatch_refuse(hdt_out);
    return 0;
  }
  return 1;
}


/* ========== the Gate ========== */
/*
  quickdoc: return once the host has raised the Gate at flag.

  the condition is tested FIRST. by the protocol's timing the host has usually
  raised the Gate before the client reaches here -- the client is the passive
  side -- and parking on an event that has already happened never returns.

  the waitpkg arm re-reads after arming for the same reason: a store that landed
  before the monitor armed will never wake the wait.
*/
PRIVATE_FUNC void rpc_client_wait_until_gate_up(char *flag, bool umonitor_backend) {
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (umonitor_backend) {
#ifdef __USE_WAITPKG__
      _umonitor(flag);
      if (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        _umwait(1, 0);
      }
#else
      assert(false, "[ASSERT] the waitpkg backend was not compiled in");
#endif
    } else {
      if (!client_specific_waiter) {
        assert(false, "[ASSERT] no client waiter injected, see set_client_specific_waiter");
      }
      client_specific_waiter(flag);
    }
  }
}


/* ========== stage 1: hello ========== */
extern int rpc_client_run_hello_handshake_system(rpc_client_handshake_t *hh_out, void *addr);

/*
  quickdoc: the Hello DIALOG. wait for the Gate, validate what the host
  published, answer with the capability mask, lower the Gate.

  returns 1  ok
  returns 0  rejected; the error bit is set for the host
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
    rejected = true;
  }
  //the draft requires the client to check the accuracy of the capability mask:
  //the host leaves the error bit clear and bits 3-7 reserved
  if ((host_cap_mask & ~(RPC_CLIENT_CAP_UMONITOR | RPC_CLIENT_CAP_PHYS)) != 0) {
    rejected = true;
  }
  if (*own_length_ptr != host_length) {
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

//quickdoc: hello, waitpkg entry. returns -1 when not compiled in
int rpc_client_run_hello_handshake_umonitor(rpc_client_handshake_t *hh_out, void *addr) {
#ifdef __USE_WAITPKG__
  assert(hh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU, rolling back");
    return rpc_client_run_hello_handshake_system(hh_out, addr);
  }
  return rpc_client_hello_handshake_common(hh_out, addr, true);
#else
  (void)hh_out;
  (void)addr;
  return -1;
#endif
}

//quickdoc: hello, system entry. returns -1 when there is no system api
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

//quickdoc: stage 1 router. prefer waitpkg when it is compiled in and the cpu
//has it; only the umonitor entry rolls back
int rpc_client_run_hello_handshake(rpc_client_handshake_t *hh_out, void *addr) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    return rpc_client_run_hello_handshake_umonitor(hh_out, addr);
  }
#endif
  return rpc_client_run_hello_handshake_system(hh_out, addr);
}


/* ========== stage 2: dispatch ========== */
extern int rpc_client_run_dispatch_handshake_system(rpc_client_dispatch_handshake_t *hdh_out,
                                                    void *addr);

/*
  quickdoc: the Dispatch DIALOG. wait for the Gate, check the layout the host
  published, answer with an error code, lower the Gate.

  returns 1   ok, the layout is kept in hdh_out
  returns 0   ILLEGAL FUNC COUNT
  returns -1  ILLEGAL SPACE COUNT
  returns -2  a reserved code. defensive: the client authors the code, so only
              an out-of-band one can reach it
*/
PRIVATE_FUNC int rpc_client_dispatch_handshake_common(rpc_client_dispatch_handshake_t *hdh_out,
                                                      void *addr, bool umonitor_backend) {
  char *flag_addr = addr;
  char *error_code_addr = addr + 1;
  unsigned short *split_ptr = (unsigned short *)(addr + 2);
  unsigned int *bps_ptr = (unsigned int *)(addr + 4);

  //a rejection must leave no layout at all, including none from an earlier
  //handshake in the same structure
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
    return -2;
  }
}

//quickdoc: dispatch, waitpkg entry. returns -3 when not compiled in
int rpc_client_run_dispatch_handshake_umonitor(rpc_client_dispatch_handshake_t *hdh_out,
                                               void *addr) {
#ifdef __USE_WAITPKG__
  assert(hdh_out != nullptr && addr != nullptr, "[ASSERT] wrong param, nullptr");
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU, rolling back");
    return rpc_client_run_dispatch_handshake_system(hdh_out, addr);
  }
  return rpc_client_dispatch_handshake_common(hdh_out, addr, true);
#else
  (void)hdh_out;
  (void)addr;
  return -3;
#endif
}

//quickdoc: dispatch, system entry. returns -3 when there is no system api
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

//quickdoc: stage 2 router, see the stage 1 one
int rpc_client_run_dispatch_handshake(rpc_client_dispatch_handshake_t *hdh_out, void *addr) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    return rpc_client_run_dispatch_handshake_umonitor(hdh_out, addr);
  }
#endif
  return rpc_client_run_dispatch_handshake_system(hdh_out, addr);
}


/* ========== stage 3: function ========== */
/*
  quickdoc: service one slot whose Gate the host has already raised. read the
  kind from the head, run the handler for a FUNC, record the published size for
  a HUGE_SHM, then lower the Gate. SHM and HUGE_SHM have no handler: the caller
  reads the shared region once the Gate is down.
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
  }
  __atomic_store_n(flag, 0, __ATOMIC_RELEASE);
}

extern int rpc_client_service_slot_system(struct __client_disp_list *node);

//quickdoc: service one slot, waiting for its Gate. returns -1 when not compiled in
int rpc_client_service_slot_umonitor(struct __client_disp_list *node) {
#ifdef __USE_WAITPKG__
  assert(node != nullptr, "[ASSERT] wrong param, nullptr");
  if (!is_support_umonitor()) {
    LOG("[WARN] waitpkg ISA is not supported on your CPU, rolling back");
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

//quickdoc: service one slot, waiting for its Gate. returns -1 when there is no
//system api
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

//quickdoc: stage 3 router, see the stage 1 one
int rpc_client_service_slot(struct __client_disp_list *node) {
#ifdef __USE_WAITPKG__
  if (is_support_umonitor()) {
    return rpc_client_service_slot_umonitor(node);
  }
#endif
  return rpc_client_service_slot_system(node);
}

/*
  quickdoc: service one slot only if the host has already raised its Gate. never
  waits, so a slot the host leaves alone cannot hold up the others.

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

//quickdoc: one pass over the table, servicing every slot whose Gate is up.
//returns how many were serviced
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
  quickdoc: keep making passes over the table.

  this does not park between passes, and cannot: the injected waiter listens on
  a single address and no address covers several slots, so parking on any one of
  them would let that slot starve the rest. callers that want pacing should
  drive rpc_client_service_ready_slots themselves.
*/
void rpc_client_run_dispatch(rpc_client_dispatch_t *hdt_out) {
  assert(hdt_out != nullptr && hdt_out->start_addr != nullptr,
         "[ASSERT] wrong param, nullptr");
  //running a dispatch loop over a table discovery never built is undefined
  assert(hdt_out->head != nullptr, "[ASSERT] the table is empty, nothing was discovered");
  while (true) {
    rpc_client_service_ready_slots(hdt_out);
  }
}
