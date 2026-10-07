/*
  Ticket 02 -- the waitpkg backend and the auto router.

  On this machine CPUID leaf7 ECX bit5 (WAITPKG) is 0, so the real umonitor path
  cannot run locally and the rollback is what gets exercised. Tests that only
  mean something with the feature report a skip rather than a failure, which is
  the ticket's requirement.

  Every scenario below runs in an isolated child: a scenario that parks on a Gate
  nobody raises would otherwise hang the suite, and the alarm turns that into a
  failure instead. The child makes its own CHECKs and hands the verdict back
  through its exit status, because a child's memory is not the parent's.
*/
#include "harness.h"

#include <pthread.h>

#define SPACE_LEN 12288u

static char space[SPACE_LEN];
static char other_space[SPACE_LEN];
static char snap[SPACE_LEN];

/* A real host raises the Gate while the client is parked. The system backend
   never needs this -- its injected waiter raises the Gate -- but the waitpkg
   backend does, because it parks on the CPU and calls no waiter at all. */
static void *host_raises_gate_after_pause(void *arg) {
  char *flag = arg;
  usleep(30000);
  *(volatile char *)flag = 1;
  return nullptr;
}

/* --- the router, against a host that raises the Gate concurrently --- */

static void scenario_router_hello(void) {
  harness_bind(space, snap, SPACE_LEN);
  rpc_client_handshake_t hh;
  rpc_client_handshake_init(&hh, false, false, SPACE_LEN);
  unsigned int len = SPACE_LEN;
  memcpy(space + 2, &len, sizeof len); /* the host published its length */
  space[0] = 1;                        /* and raised the Gate before handing over */

  /* Only the waitpkg backend needs a host reaching in from outside. The system
     backend is released by its injected waiter at the end of the turn. */
  pthread_t t;
  bool concurrent_host = is_support_umonitor();
  if (concurrent_host) {
    pthread_create(&t, nullptr, host_raises_gate_after_pause, space);
  }
  int rc = rpc_client_run_hello_handshake(&hh, space);
  if (concurrent_host) {
    pthread_join(t, nullptr);
  }

  CHECK(rc == 1, "router: the well formed hello was accepted");
  CHECK(g_gate_at_first_wait == 0,
        "router: the Gate was handed back before the client parked");
}

static void test_the_router_completes_on_whichever_backend_it_picks(void) {
  if (!harness_have_system_backend() && !is_support_umonitor()) {
    SKIP("router: this configuration has no usable backend on this cpu");
    return;
  }
  int child_failures = harness_run_isolated(scenario_router_hello, 10);
  CHECK(child_failures == 0,
        "router: the hello completed rather than hanging, whichever backend it picked");
  if (!is_support_umonitor()) {
    SKIP("waitpkg backend: this cpu has no waitpkg, the system backend ran instead");
  }
}

static void test_the_real_waitpkg_backend_when_the_cpu_has_it(void) {
  if (!is_support_umonitor()) {
    SKIP("real waitpkg backend: cpu has no waitpkg, nothing to exercise");
    return;
  }
  int child_failures = harness_run_isolated(scenario_router_hello, 10);
  CHECK(child_failures == 0,
        "real waitpkg backend: parks on the Gate and completes once the host raises it");
}

/* --- the umonitor entries must land on the system behaviour when they cannot run --- */

static void scenario_umonitor_hello_matches_system(void) {
  rpc_client_handshake_t hh;

  harness_bind(space, snap, SPACE_LEN);
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);
  harness_publish_hello(space, 1, 0, SPACE_LEN);
  int system_rc = rpc_client_run_hello_handshake_system(&hh, space);

  harness_bind(other_space, snap, SPACE_LEN);
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);
  harness_publish_hello(other_space, 1, 0, SPACE_LEN);
  int umonitor_rc = rpc_client_run_hello_handshake_umonitor(&hh, other_space);

  /* the literal bit, not just self-agreement: on this cpu the entry literally
     delegates to the system one, so comparing the two alone proves nothing */
  CHECK(space[1] == 0x01 && other_space[1] == 0x01,
        "umonitor rollback: both answered with the literal bit the draft fixes");
  CHECK(system_rc == umonitor_rc, "umonitor rollback: same verdict as the system entry");
  CHECK(memcmp(space, other_space, SPACE_LEN) == 0,
        "umonitor rollback: byte for byte the same writes as the system entry");
}

/*
  The rollback is only observable when the waitpkg path was compiled in AND the
  system backend actually works AND the running CPU lacks the feature. Without
  the system api both entries are stubs returning -1, so comparing them would
  prove nothing at all.
*/
static void test_the_umonitor_entry_rolls_back(void) {
#if defined(__USE_WAITPKG__) && defined(RPC_CLIENT_HAVE_SYSTEM_API)
  if (is_support_umonitor()) {
    SKIP("umonitor rollback: cpu has waitpkg, the entry takes the real path instead");
    return;
  }
  int child_failures = harness_run_isolated(scenario_umonitor_hello_matches_system, 10);
  CHECK(child_failures == 0, "umonitor rollback: the hello entry behaves exactly as system does");
#else
  SKIP("umonitor rollback: needs the waitpkg path compiled in and a working system backend");
#endif
}

static void scenario_umonitor_dispatch_matches_system(void) {
  rpc_client_dispatch_handshake_t hdh;

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 3, 4096);
  int system_rc = rpc_client_run_dispatch_handshake_system(&hdh, space);

  harness_bind(other_space, snap, SPACE_LEN);
  harness_publish_dispatch(other_space, 1, 0, 3, 4096);
  int umonitor_rc = rpc_client_run_dispatch_handshake_umonitor(&hdh, other_space);

  /* the literal OK code, for the same reason as the hello case */
  CHECK(space[1] == 0x00 && other_space[1] == 0x00,
        "dispatch rollback: both answered with the literal OK error code");
  CHECK(system_rc == umonitor_rc, "dispatch rollback: same verdict as the system entry");
  CHECK(memcmp(space, other_space, SPACE_LEN) == 0,
        "dispatch rollback: byte for byte the same writes as the system entry");
}

static void test_the_dispatch_umonitor_entry_rolls_back(void) {
#if defined(__USE_WAITPKG__) && defined(RPC_CLIENT_HAVE_SYSTEM_API)
  if (is_support_umonitor()) {
    SKIP("dispatch rollback: cpu has waitpkg, the entry takes the real path instead");
    return;
  }
  int child_failures = harness_run_isolated(scenario_umonitor_dispatch_matches_system, 10);
  CHECK(child_failures == 0, "dispatch rollback: the dispatch entry behaves exactly as system does");
#else
  SKIP("dispatch rollback: needs the waitpkg path compiled in and a working system backend");
#endif
}

/* --- nothing is cached between calls --- */

static void scenario_router_is_stateless(void) {
  rpc_client_handshake_t hh;
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);

  for (int i = 0; i < 3; i++) {
    harness_bind(space, snap, SPACE_LEN);
    harness_publish_hello(space, 1, 0, SPACE_LEN);
    int rc = rpc_client_run_hello_handshake(&hh, space);
    CHECK(rc == 1 && space[1] == 0x01, "router: this call agreed with the previous ones");
  }
}

/*
  Honest about what this can and cannot show. The ticket permits the client to
  re-ask the CPU question each call rather than caching it -- that is a licence,
  not observable behaviour: no external effect distinguishes a cached answer
  from a fresh one. What is observable is that consecutive calls agree and leave
  nothing behind, which is what this pins down.
*/
static void test_the_router_carries_no_state_between_calls(void) {
  if (!harness_have_system_backend() && !is_support_umonitor()) {
    SKIP("router state: this configuration has no usable backend on this cpu");
    return;
  }
  int child_failures = harness_run_isolated(scenario_router_is_stateless, 10);
  CHECK(child_failures == 0,
        "router: three consecutive calls all agree and leave nothing behind");
}

int main(void) {
  test_the_router_completes_on_whichever_backend_it_picks();
  test_the_real_waitpkg_backend_when_the_cpu_has_it();
  test_the_umonitor_entry_rolls_back();
  test_the_dispatch_umonitor_entry_rolls_back();
  test_the_router_carries_no_state_between_calls();
  return REPORT();
}
