/*
  Ticket 05 -- the stage 3 slot servicer, one slot at a time.

  A slot's head is flag@+0 and function_code@+1. The client waits for the host
  to raise the flag, does whatever the kind calls for, then lowers it.
*/
#include "harness.h"

#define BPS 4096u
#define SPACE_LEN (BPS * 2u)

static char space[SPACE_LEN];
static char snap[SPACE_LEN];
static struct __client_disp_list node;
static int handler_calls = 0;
static struct __client_disp_list *last_self = nullptr;

static void counting_handler(struct __client_disp_list *self) {
  handler_calls++;
  last_self = self;
}

/* stamp a slot and point node at it; the host raises its flag unless told not to */
static void arm(enum rpc_client_dispatch_type dt, unsigned int huge_size, char gate) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space, dt, huge_size);
  harness_arm_node(&node, space, dt, BPS, (dt == RPC_CLIENT_FUNC) ? counting_handler : nullptr);
  space[0] = gate;
}

static void test_a_func_slot_runs_its_handler_and_lowers_the_gate(void) {
  REQUIRE_SYSTEM_BACKEND("servicer: no system backend compiled in for this configuration");
  handler_calls = 0;
  last_self = nullptr;
  arm(RPC_CLIENT_FUNC, 0, 1); /* the host raised the Gate before this turn */

  int rc = rpc_client_service_slot_system(&node);

  CHECK(rc == 1, "servicer: a FUNC slot was serviced");
  CHECK(handler_calls == 1, "servicer: the registered handler ran exactly once");
  CHECK(last_self == &node, "servicer: the handler was handed its own slot");
  CHECK(g_gate_at_first_wait == 0,
        "servicer: the Gate was handed back before the servicer parked");
}

static void test_a_shm_slot_needs_no_handler(void) {
  arm(RPC_CLIENT_SHM, 0, 1); /* already raised */

  int rc = rpc_client_service_slot_if_ready(&node);

  CHECK(rc == 1, "servicer: an SHM slot was serviced");
  CHECK(node.d_t == RPC_CLIENT_SHM, "servicer: the kind was read from the slot head");
  CHECK(space[0] == 0, "servicer: the Gate was lowered");
}

static void test_a_huge_shm_slot_records_the_size_the_host_published(void) {
  arm(RPC_CLIENT_HUGE_SHM, 3 * BPS, 1);

  int rc = rpc_client_service_slot_if_ready(&node);

  CHECK(rc == 1, "servicer: a HUGE_SHM slot was serviced");
  CHECK(node.length == 3 * BPS, "servicer: the size the host published was recorded");
  CHECK(space[0] == 0, "servicer: the Gate was lowered");
}

static void test_a_slot_whose_gate_is_down_is_left_alone(void) {
  arm(RPC_CLIENT_SHM, 0, 0);
  harness_freeze();

  int rc = rpc_client_service_slot_if_ready(&node);

  CHECK(rc == 0, "servicer: a slot whose Gate is down is not serviced");
  CHECK(memcmp(space, snap, SPACE_LEN) == 0, "servicer: and nothing was written to it");
}

static void run_a_func_slot_with_no_handler(void) {
  arm(RPC_CLIENT_FUNC, 0, 1);
  node.dispatch_function = nullptr;
  rpc_client_service_slot_if_ready(&node);
}

static void test_a_func_slot_with_no_handler_fails_loudly(void) {
  int status = harness_run_isolated(run_a_func_slot_with_no_handler, 5);
  CHECK(status != 0 && status != 126,
        "servicer: a FUNC slot with no handler aborts rather than silently skipping");
}

static void test_the_router_agrees_with_the_system_servicer(void) {
  REQUIRE_SYSTEM_BACKEND("servicer router: no system backend compiled in for this configuration");

  arm(RPC_CLIENT_SHM, 0, 1);
  int via_router = rpc_client_service_slot(&node);
  /* the literal effects, not just self-agreement: on a system-only build the
     router and the entry are the same path, so comparing them alone proves
     nothing */
  bool router_handed_back = (g_gate_at_first_wait == 0);
  bool router_read_kind = (node.d_t == RPC_CLIENT_SHM);

  arm(RPC_CLIENT_SHM, 0, 1);
  int via_system = rpc_client_service_slot_system(&node);

  CHECK(via_router == 1 && via_system == 1,
        "servicer: the router services a slot just as the system entry does");
  CHECK(router_handed_back && router_read_kind,
        "servicer: the router really serviced it -- read the kind and handed the Gate back");
}

/*
  The flow a real caller has to follow, which nothing else pins down: discovery
  cannot know the application's handlers, so it leaves every FUNC node unset and
  servicing one without registering aborts. Discover, look the node up, register,
  then service.
*/
static void test_the_documented_flow_registers_before_servicing(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space, RPC_CLIENT_FUNC, 0);

  rpc_client_dispatch_handshake_t hdh;
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = 1;
  hdh.bytes_per_split = BPS;

  rpc_client_dispatch_t dt;
  CHECK(rpc_client_discover_dispatch(&dt, &hdh, space, BPS) == 1,
        "flow: a single FUNC slot was discovered");

  struct __client_disp_list *first = rpc_client_dispatch_node_at(&dt, 0);
  CHECK(first != nullptr, "flow: the discovered node can be looked up by position");
  CHECK(first->dispatch_function == nullptr,
        "flow: discovery leaves the handler unset, so the caller must register one");

  handler_calls = 0;
  rpc_client_dispatch_set_handler(first, counting_handler);
  space[0] = 1;

  CHECK(rpc_client_service_slot_if_ready(first) == 1, "flow: the slot was serviced");
  CHECK(handler_calls == 1, "flow: the handler registered after discovery actually ran");

  rpc_client_dispatch_destroy_node(&dt);
}

int main(void) {
  test_a_func_slot_runs_its_handler_and_lowers_the_gate();
  test_a_shm_slot_needs_no_handler();
  test_a_huge_shm_slot_records_the_size_the_host_published();
  test_a_slot_whose_gate_is_down_is_left_alone();
  test_a_func_slot_with_no_handler_fails_loudly();
  test_the_router_agrees_with_the_system_servicer();
  test_the_documented_flow_registers_before_servicing();
  return REPORT();
}
