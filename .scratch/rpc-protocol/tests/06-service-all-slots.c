/*
  Ticket 06 -- every slot in the table gets serviced.

  The regression: the earlier entry walked the table calling a runner that never
  returned, so the first slot consumed the loop and every slot after it was
  never reached. These tests pin that down with tables of two and three slots.
*/
#include "harness.h"

#define BPS 4096u
#define MAX_SLOTS 3u
#define SPACE_LEN (BPS * MAX_SLOTS)

static char space[SPACE_LEN];
static char snap[SPACE_LEN];
static rpc_client_dispatch_t dt;
static int handler_calls[MAX_SLOTS];
static bool loop_slot_served[MAX_SLOTS];

static void index_handler_0(struct __client_disp_list *s) {
  (void)s;
  handler_calls[0]++;
}
static void index_handler_1(struct __client_disp_list *s) {
  (void)s;
  handler_calls[1]++;
}
static void index_handler_2(struct __client_disp_list *s) {
  (void)s;
  handler_calls[2]++;
}

static rpc_client_dispatch_func_t handlers[MAX_SLOTS] = {index_handler_0, index_handler_1,
                                                         index_handler_2};

/*
  build a table of n FUNC slots, none of their Gates raised. fn, when given, is
  registered on every slot; otherwise each gets its own indexed handler so the
  tests can tell which slot ran.
*/
static void build_table(unsigned int n, rpc_client_dispatch_func_t fn) {
  harness_bind(space, snap, SPACE_LEN);
  memset(&dt, 0, sizeof dt);
  rpc_client_dispatch_init(&dt, space, SPACE_LEN);
  dt.split_number = (unsigned short)n;
  dt.bytes_per_split = BPS;
  for (unsigned int i = 0; i < n; i++) {
    char *slot = space + (i * BPS);
    harness_stamp_slot(slot, RPC_CLIENT_FUNC, 0);
    slot[0] = 0;
    handler_calls[i] = 0;
    loop_slot_served[i] = false;
    rpc_client_dispatch_append_node(&dt, RPC_CLIENT_FUNC, slot, BPS,
                                    fn != nullptr ? fn : handlers[i]);
  }
}

static void raise_gate(unsigned int i) { space[i * BPS] = 1; }

static void test_one_pass_services_every_slot_that_is_ready(void) {
  build_table(3, nullptr);
  raise_gate(0);
  raise_gate(1);
  raise_gate(2);

  unsigned int serviced = rpc_client_service_ready_slots(&dt);

  CHECK(serviced == 3, "multi slot: one pass serviced all three slots");
  CHECK(handler_calls[0] == 1 && handler_calls[1] == 1 && handler_calls[2] == 1,
        "multi slot: every slot's handler ran, not just the first");
  CHECK(space[0 * BPS] == 0 && space[1 * BPS] == 0 && space[2 * BPS] == 0,
        "multi slot: every Gate was lowered");
}

static void test_a_slot_the_host_ignores_does_not_hold_up_the_others(void) {
  build_table(3, nullptr);
  raise_gate(2); /* only the last slot */

  unsigned int serviced = rpc_client_service_ready_slots(&dt);

  CHECK(serviced == 1, "multi slot: only the ready slot was serviced");
  CHECK(handler_calls[2] == 1, "multi slot: the last slot was reached even though the first was idle");
  CHECK(handler_calls[0] == 0 && handler_calls[1] == 0,
        "multi slot: the idle slots were left alone");
  CHECK(space[0 * BPS] == 0 && space[1 * BPS] == 0, "multi slot: the idle slots were not written to");
}

static void test_a_second_pass_picks_up_what_the_first_missed(void) {
  build_table(2, nullptr);
  raise_gate(0);

  unsigned int first = rpc_client_service_ready_slots(&dt);
  raise_gate(1);
  unsigned int second = rpc_client_service_ready_slots(&dt);

  CHECK(first == 1 && second == 1, "multi slot: each pass services what is ready at that moment");
  CHECK(handler_calls[0] == 1 && handler_calls[1] == 1,
        "multi slot: both slots were serviced across the two passes");
}

static void test_an_empty_table_is_handled(void) {
  build_table(0, nullptr);

  unsigned int serviced = rpc_client_service_ready_slots(&dt);

  CHECK(serviced == 0, "multi slot: an empty table services nothing and does not crash");
}

static void test_an_idle_table_services_nothing(void) {
  build_table(3, nullptr);

  unsigned int serviced = rpc_client_service_ready_slots(&dt);

  CHECK(serviced == 0, "multi slot: with every Gate down, a pass is a no-op");
  CHECK(handler_calls[0] == 0 && handler_calls[1] == 0 && handler_calls[2] == 0,
        "multi slot: no handler ran");
}

/*
  The run loop never returns for a live table, so an empty one is the only case
  where its plain return can be asserted. Running it under an alarm turns a
  spin into a failure instead of a hung suite.
*/
static void scenario_run_loop_on_an_empty_table(void) {
  build_table(0, nullptr);
  rpc_client_run_dispatch(&dt);
}

static void test_the_run_loop_returns_on_an_empty_table(void) {
  int child_failures = harness_run_isolated(scenario_run_loop_on_an_empty_table, 5);
  CHECK(child_failures == 0,
        "multi slot: the run loop returns on an empty table rather than spinning");
}

/*
  The defect this ticket exists to fix lived in the table walk, and the tests
  above only exercise the pass helper -- the loop body itself was reached by no
  test. This drives rpc_client_run_dispatch over a live three-slot table and
  leaves from inside a handler once every slot has been serviced, so a loop that
  stops after the first slot hangs and trips the alarm.
*/
/*
  Counts DISTINCT slots, not invocations. A global counter cannot tell progress
  from the defect: the original bug re-invoked the first slot's handler forever,
  which drives a global total to three just as happily as servicing three
  different slots does.
*/
static void loop_exit_handler(struct __client_disp_list *s) {
  unsigned int idx = (unsigned int)(((char *)s->base_addr) - space) / BPS;
  if (idx < MAX_SLOTS) {
    loop_slot_served[idx] = true;
  }
  unsigned int distinct = 0;
  for (unsigned int i = 0; i < MAX_SLOTS; i++) {
    if (loop_slot_served[i]) {
      distinct++;
    }
  }
  if (distinct == MAX_SLOTS) {
    fflush(stdout);
    _exit(0); /* every slot was reached, so the walk did not stop at the first */
  }
}

static void scenario_run_loop_services_every_slot(void) {
  build_table(3, loop_exit_handler);
  raise_gate(0);
  raise_gate(1);
  raise_gate(2);
  rpc_client_run_dispatch(&dt);
}

static void test_the_run_loop_reaches_every_slot(void) {
  int child_failures = harness_run_isolated(scenario_run_loop_services_every_slot, 5);
  CHECK(child_failures == 0,
        "multi slot: the run loop walked past the first slot and serviced all three");
}

int main(void) {
  test_one_pass_services_every_slot_that_is_ready();
  test_a_slot_the_host_ignores_does_not_hold_up_the_others();
  test_a_second_pass_picks_up_what_the_first_missed();
  test_an_empty_table_is_handled();
  test_an_idle_table_services_nothing();
  test_the_run_loop_returns_on_an_empty_table();
  test_the_run_loop_reaches_every_slot();
  return REPORT();
}
