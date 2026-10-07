/*
  Ticket 07 -- the return-code contract of every entry point.

  Covers all three classes the ticket asks for: success, a peer-reported error,
  and a path that was not compiled in. The last class is only reachable for the
  waitpkg entries, which report -1 / -3 when the switch is off; the system
  entries have no such state on this platform, because the automatic selection
  always finds a system api here.
*/
#include "harness.h"

#define SPACE_LEN 12288u

static char space[SPACE_LEN];
static char snap[SPACE_LEN];
static rpc_client_handshake_t hh;
static rpc_client_dispatch_handshake_t hdh;
static struct __client_disp_list node;

static void bind_and_publish_hello(char hello_message, char host_cap_mask, unsigned int host_length) {
  harness_bind(space, snap, SPACE_LEN);
  harness_publish_hello(space, hello_message, host_cap_mask, host_length);
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);
}

static void test_hello_reports_success_and_a_peer_error(void) {
  REQUIRE_SYSTEM_BACKEND("contract: no system backend compiled in for this configuration");
  bind_and_publish_hello(1, 0, SPACE_LEN);
  CHECK(rpc_client_run_hello_handshake_system(&hh, space) == 1,
        "contract: hello reports 1 on success");

  bind_and_publish_hello(1, 0, SPACE_LEN * 2); /* a rejection the client decides */
  CHECK(rpc_client_run_hello_handshake_system(&hh, space) == 0,
        "contract: hello reports 0 when the client rejects the peer's hello");
}

static void test_dispatch_reports_all_three_local_verdicts(void) {
  REQUIRE_SYSTEM_BACKEND("contract: no system backend compiled in for this configuration");
  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 4, 4096);
  CHECK(rpc_client_run_dispatch_handshake_system(&hdh, space) == 1,
        "contract: dispatch reports 1 on a possible layout");

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 0, 4096);
  CHECK(rpc_client_run_dispatch_handshake_system(&hdh, space) == 0,
        "contract: dispatch reports 0 for an illegal function count");

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 4, 4);
  CHECK(rpc_client_run_dispatch_handshake_system(&hdh, space) == -1,
        "contract: dispatch reports -1 for an illegal space count");
}

/*
  discover has two outcomes it reports: the layout is usable, or the host's data
  is not. Caller errors -- a zero split count, a length the layout cannot
  produce -- abort instead, and 04 covers those.
*/
static void test_discover_reports_ok_or_a_refused_layout(void) {
  rpc_client_dispatch_t dt;

  /* a space carrying real slots: an all-zero one reads as HUGE_SHM nodes with
     no published span, which discovery rightly refuses */
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * 4096, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 1 * 4096, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 2 * 4096, RPC_CLIENT_SHM, 0);
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = 3;
  hdh.bytes_per_split = 4096;
  CHECK(rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN) == 1,
        "contract: discover reports 1 on a tiling layout");
  rpc_client_dispatch_destroy_node(&dt);

  /* the host's own data is unusable, which the caller does need to hear about */
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * 4096, RPC_CLIENT_HUGE_SHM, 0);
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = 3;
  hdh.bytes_per_split = 4096;
  CHECK(rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN) == 0,
        "contract: discover reports 0 when the host's layout is unusable");
}

static void test_the_servicer_reports_serviced_or_not_ready(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space, RPC_CLIENT_SHM, 0);
  harness_arm_node(&node, space, RPC_CLIENT_SHM, 4096, nullptr);

  space[0] = 0;
  CHECK(rpc_client_service_slot_if_ready(&node) == 0,
        "contract: the servicer reports 0 when the Gate is down");

  space[0] = 1;
  CHECK(rpc_client_service_slot_if_ready(&node) == 1,
        "contract: the servicer reports 1 when it serviced the slot");
}

/*
  The not-compiled-in class. Only the waitpkg entries have it: without the
  switch they are honest stubs rather than silently doing something else.
*/
static void test_waitpkg_entries_report_unimplemented_when_switched_off(void) {
#ifndef __USE_WAITPKG__
  bind_and_publish_hello(1, 0, SPACE_LEN);
  CHECK(rpc_client_run_hello_handshake_umonitor(&hh, space) == -1,
        "contract: the hello waitpkg entry reports -1 when it was not compiled in");

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 4, 4096);
  CHECK(rpc_client_run_dispatch_handshake_umonitor(&hdh, space) == -3,
        "contract: the dispatch waitpkg entry reports -3 when it was not compiled in");

  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space, RPC_CLIENT_SHM, 0);
  harness_arm_node(&node, space, RPC_CLIENT_SHM, 4096, nullptr);
  CHECK(rpc_client_service_slot_umonitor(&node) == -1,
        "contract: the stage 3 waitpkg entry reports -1 when it was not compiled in");
#else
  SKIP("not-compiled-in codes: the waitpkg path is compiled in for this configuration");
#endif
}

/*
  The other not-compiled-in class. A waitpkg-only build leaves no system api, so
  the _system entries become stubs too. Between this test and the one above,
  both families of stub are covered by whichever configuration is running.
*/
static void test_system_entries_report_unimplemented_without_a_system_api(void) {
#ifndef RPC_CLIENT_HAVE_SYSTEM_API
  bind_and_publish_hello(1, 0, SPACE_LEN);
  CHECK(rpc_client_run_hello_handshake_system(&hh, space) == -1,
        "contract: the hello system entry reports -1 when there is no system api");

  harness_bind(space, snap, SPACE_LEN);
  harness_publish_dispatch(space, 1, 0, 4, 4096);
  CHECK(rpc_client_run_dispatch_handshake_system(&hdh, space) == -3,
        "contract: the dispatch system entry reports -3 when there is no system api");

  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space, RPC_CLIENT_SHM, 0);
  harness_arm_node(&node, space, RPC_CLIENT_SHM, 4096, nullptr);
  CHECK(rpc_client_service_slot_system(&node) == -1,
        "contract: the stage 3 system entry reports -1 when there is no system api");
#else
  SKIP("system stub codes: a system api is compiled in for this configuration");
#endif
}

int main(void) {
  test_hello_reports_success_and_a_peer_error();
  test_dispatch_reports_all_three_local_verdicts();
  test_discover_reports_ok_or_a_refused_layout();
  test_the_servicer_reports_serviced_or_not_ready();
  test_waitpkg_entries_report_unimplemented_when_switched_off();
  test_system_entries_report_unimplemented_without_a_system_api();
  return REPORT();
}
