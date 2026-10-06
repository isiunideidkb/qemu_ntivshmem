/*
  Shared harness for the rpc client tests.

  The seam is exactly one thing: the Address Space byte buffer, plus the
  injected waiter that lets a test drive the Gate deterministically. Tests
  fabricate the host side as raw bytes and assert what the client writes back.

  The host header is never included: it does not compile, and it is out of
  scope for this contribution.
*/
#pragma once

#include "common/rpc_client.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks_run = 0;
static int checks_failed = 0;
static int checks_skipped = 0;

#define CHECK(cond, msg)                                                                           \
  do {                                                                                             \
    checks_run++;                                                                                  \
    if (cond) {                                                                                    \
      printf("ok   %s\n", (msg));                                                                  \
    } else {                                                                                       \
      printf("FAIL %s\n", (msg));                                                                  \
      checks_failed++;                                                                             \
    }                                                                                              \
  } while (0)

#define SKIP(msg)                                                                                  \
  do {                                                                                             \
    printf("skip %s\n", (msg));                                                                    \
    checks_skipped++;                                                                              \
  } while (0)

#define REPORT()                                                                                   \
  (printf("\n%s (%d checks, %d skipped, %d failure%s)\n",                                          \
          checks_failed ? "FAILED" : "PASSED", checks_run, checks_skipped, checks_failed,          \
          checks_failed == 1 ? "" : "s"),                                                          \
   checks_failed ? 1 : 0)


/* --- a fake host, driving the Gate through the injected waiter --- */

static char *g_space = nullptr;
static char *g_snapshot = nullptr;
static unsigned int g_space_len = 0;
static int g_waiter_calls = 0;
static int g_raise_on_call = 1;
static int g_untouched_while_gate_down = 1;

static void harness_fake_waiter(void *addr) {
  g_waiter_calls++;
  if (g_waiter_calls < g_raise_on_call) {
    /* the client must not have written anything yet */
    if (g_snapshot != nullptr && memcmp(g_space, g_snapshot, g_space_len) != 0) {
      g_untouched_while_gate_down = 0;
    }
    return;
  }
  /* the real host publishes its message and raises the Gate in one act, so
     never clobber a byte the test already published */
  if (*(volatile char *)addr == 0) {
    *(volatile char *)addr = 1;
  }
}

static inline void harness_bind(char *space, char *snapshot, unsigned int len) {
  g_space = space;
  g_snapshot = snapshot;
  g_space_len = len;
  memset(space, 0, len);
  g_waiter_calls = 0;
  g_raise_on_call = 1;
  g_untouched_while_gate_down = 1;
  set_client_specific_waiter((void *)harness_fake_waiter);
}

/* make the host take n waiter calls before it raises the Gate */
static inline void harness_raise_gate_on(int n) {
  g_raise_on_call = n;
  g_waiter_calls = 0;
}

/* freeze the published host bytes so the waiter can spot an early write */
static inline void harness_freeze(void) { memcpy(g_snapshot, g_space, g_space_len); }

static inline void harness_drop_waiter(void) { set_client_specific_waiter(nullptr); }

/* --- running something in a child, so a hang or an abort is observable --- */

/*
  run fn in a child under an alarm, and bring the verdict back.

  Two reasons this exists. A scenario that parks on a Gate nobody raises would
  otherwise hang the whole suite, and the alarm turns that into a failure. And a
  child's memory is not the parent's: a test that runs a scenario in a child and
  then asserts on the parent's copy of the buffers is asserting on untouched
  memory, and passes by construction. So the child either makes its own CHECKs
  or aborts outright, and reports through its exit status.

  returns the child's failure count, or 126 if it was killed -- by the alarm, or
  by anything else. A scenario that is expected to abort therefore shows up as a
  non-zero value that is not 126.
*/
static inline int harness_run_isolated(void (*fn)(void), int alarm_seconds) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    /* the child inherited the parent's counters, so only its own failures may
       count -- otherwise it reports failures it never caused */
    checks_run = 0;
    checks_failed = 0;
    if (alarm_seconds > 0) {
      alarm((unsigned int)alarm_seconds);
    }
    fn();
    fflush(stdout); /* _exit does not flush, and the child's CHECK lines are the diagnosis */
    _exit(checks_failed > 125 ? 125 : checks_failed);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status)) {
    return 126;
  }
  return WEXITSTATUS(status);
}


/* --- byte layout helpers, from the draft --- */

/* stage 1: hello_message@0 (doubles as the Gate), cap_mask@1, length@2..5 */
static inline void harness_publish_hello(char *space, char hello_message, char cap_mask,
                                         unsigned int length) {
  space[0] = hello_message;
  space[1] = cap_mask;
  memcpy(space + 2, &length, sizeof length);
}

/* stage 2: gate@0, error_code@1, split_number@2, bytes_per_split@4..7 */
static inline void harness_publish_dispatch(char *space, char gate, char error_code,
                                            unsigned short split_number, unsigned int bps) {
  space[0] = gate;
  space[1] = error_code;
  memcpy(space + 2, &split_number, sizeof split_number);
  memcpy(space + 4, &bps, sizeof bps);
}

/* stage 3: a slot's head is flag@+0, function_code@+1 */
static inline void harness_stamp_slot(char *slot, enum rpc_client_dispatch_type dt,
                                      unsigned int huge_size) {
  slot[1] = (char)dt;
  if (dt == RPC_CLIENT_HUGE_SHM) {
    memcpy(slot + 2, &huge_size, sizeof huge_size);
  }
}

/* point a table node at a slot, without hand-building it at each call site */
static inline void harness_arm_node(struct __client_disp_list *node, char *slot,
                                    enum rpc_client_dispatch_type dt, unsigned int length,
                                    rpc_client_dispatch_func_t fn) {
  memset(node, 0, sizeof *node);
  node->base_addr = slot;
  node->length = length;
  node->d_t = dt;
  node->dispatch_function = fn;
}


/* --- backend availability --- */

/*
  A waitpkg-only build leaves RPC_CLIENT_HAVE_SYSTEM_API undefined, so the
  _system entries become honest stubs returning -1 / -3 rather than working.
  Tests that need a working system backend skip in that configuration instead of
  failing, and ticket 07 covers the stubs themselves.
*/
static inline bool harness_have_system_backend(void) {
#ifdef RPC_CLIENT_HAVE_SYSTEM_API
  return true;
#else
  return false;
#endif
}

#define REQUIRE_SYSTEM_BACKEND(msg)                                                                \
  do {                                                                                             \
    if (!harness_have_system_backend()) {                                                          \
      SKIP(msg);                                                                                   \
      return;                                                                                      \
    }                                                                                              \
  } while (0)
