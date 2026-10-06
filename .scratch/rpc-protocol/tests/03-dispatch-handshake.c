/*
  Ticket 03 -- the Dispatch DIALOG handshake.

  Byte layout, from the draft: gate@0, error_code@1, split_number@2..3,
  bytes_per_split@4..7.
*/
#include "harness.h"

#define SPACE_LEN 12288u

static char space[SPACE_LEN];
static char snap[SPACE_LEN];
static int rc = 0;
static rpc_client_dispatch_handshake_t hdh;

static void run_dispatch(char gate, char error_code, unsigned short splits, unsigned int bps) {
  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, gate, error_code, splits, bps);
  memset(&hdh, 0, sizeof hdh);
  rc = rpc_client_run_dispatch_handshake_system(&hdh, space);
}

static void test_accepts_a_possible_layout(void) {
  run_dispatch(1, 0, 3, 4096);

  CHECK(rc == 1, "dispatch: a possible layout is accepted");
  CHECK(space[1] == 0x0, "dispatch: the error code written back is OK");
  CHECK(space[0] == 0, "dispatch: the Gate was lowered");
  CHECK(hdh.split_number == 3 && hdh.bytes_per_split == 4096,
        "dispatch: the layout was kept for discovery to use");
}

static void test_rejects_an_impossible_function_count(void) {
  run_dispatch(1, 0, 0, 4096);

  CHECK(rc == 0, "dispatch: zero splits is reported as ILLEGAL FUNC COUNT");
  CHECK(space[1] == 0x1, "dispatch: the error code written back is 0x1");
  CHECK(space[0] == 0, "dispatch: the Gate was lowered even on rejection");
}

static void test_rejects_an_impossible_space_count(void) {
  run_dispatch(1, 0, 3, 4);

  CHECK(rc == -1, "dispatch: a too small Bytes per Split is ILLEGAL SPACE COUNT");
  CHECK(space[1] == 0x2, "dispatch: the error code written back is 0x2");
  CHECK(space[0] == 0, "dispatch: the Gate was lowered even on rejection");
}

static void test_reads_nothing_until_the_gate_is_up(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 0, 0, 3, 4096); /* gate still down */
  harness_freeze();
  harness_raise_gate_on(3);
  memset(&hdh, 0, sizeof hdh);

  rc = rpc_client_run_dispatch_handshake_system(&hdh, space);

  CHECK(g_untouched_while_gate_down == 1,
        "dispatch: nothing was written while the Gate was still down");
  CHECK(g_waiter_calls >= 3, "dispatch: the client kept waiting rather than proceeding");
  CHECK(rc == 1, "dispatch: once raised, the handshake completed normally");
}

/*
  Seed the structure with a layout from an earlier handshake first. The earlier
  version of this test cleared it before running, so it could not have observed
  a rejection leaving stale state behind -- it passed by construction.
*/
static void test_a_rejection_clears_an_earlier_layout(void) {
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = 7;
  hdh.bytes_per_split = 65536;

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 0, 4096); /* reject: zero splits */
  rc = rpc_client_run_dispatch_handshake_system(&hdh, space);

  CHECK(rc == 0, "dispatch: the impossible layout was rejected");
  CHECK(hdh.split_number == 0 && hdh.bytes_per_split == 0,
        "dispatch: a rejection clears a layout left over from an earlier handshake");
}

/* the router must agree with the entry it picks */
static void test_the_router_agrees_with_the_system_entry(void) {
  rpc_client_dispatch_handshake_t via_router;
  rpc_client_dispatch_handshake_t via_system;

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 5, 8192);
  memset(&via_router, 0, sizeof via_router);
  int router_rc = rpc_client_run_dispatch_handshake(&via_router, space);

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 5, 8192);
  memset(&via_system, 0, sizeof via_system);
  int system_rc = rpc_client_run_dispatch_handshake_system(&via_system, space);

  CHECK(router_rc == system_rc, "dispatch: the router returns what the system entry returns");
  CHECK(via_router.split_number == via_system.split_number &&
            via_router.bytes_per_split == via_system.bytes_per_split,
        "dispatch: the router keeps the same layout as the system entry");
  /* the literal values, not just self-agreement: on a system-only build the
     router and the entry are the same path, so comparing them alone proves
     nothing */
  CHECK(via_router.split_number == 5 && via_router.bytes_per_split == 8192,
        "dispatch: the router kept the literal layout the host published");
  CHECK(space[1] == 0x00, "dispatch: the router wrote the literal OK error code");
}

int main(void) {
  if (!harness_have_system_backend()) {
    SKIP("dispatch handshake: no system backend compiled in for this configuration");
    return REPORT();
  }
  test_accepts_a_possible_layout();
  test_rejects_an_impossible_function_count();
  test_rejects_an_impossible_space_count();
  test_reads_nothing_until_the_gate_is_up();
  test_a_rejection_clears_an_earlier_layout();
  test_the_router_agrees_with_the_system_entry();
  return REPORT();
}
