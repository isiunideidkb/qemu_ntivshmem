/*
  Ticket 04 -- discovery, including a HUGE_SHM node that spans several Splits.

  The span regression: the earlier implementation appended exactly one Bytes per
  Split per slot and never read the size a HUGE_SHM carries in its body, so a
  node covering two Splits came back as two independent slots.
*/
#include "harness.h"

#define BPS 4096u
#define SPLITS 4u
#define SPACE_LEN (BPS * SPLITS)

static char space[SPACE_LEN];
static char snap[SPACE_LEN];
static rpc_client_dispatch_t dt;
static rpc_client_dispatch_handshake_t hdh;

static int discover(void) {
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = SPLITS;
  hdh.bytes_per_split = BPS;
  memset(&dt, 0, sizeof dt);
  return rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN);
}

static unsigned int node_count(void) {
  unsigned int n = 0;
  for (struct __client_disp_list *p = dt.head; p != nullptr; p = p->next) {
    n++;
  }
  return n;
}

static void test_a_huge_node_spanning_two_splits_is_stepped_over(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * BPS, RPC_CLIENT_HUGE_SHM, 2 * BPS);
  harness_stamp_slot(space + 2 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 3 * BPS, RPC_CLIENT_FUNC, 0);

  int rc = discover();

  CHECK(rc == 1, "discover: a layout that tiles the space exactly is accepted");
  CHECK(node_count() == 3, "discover: a two Split HUGE_SHM is one slot, not two");
  CHECK(rpc_client_dispatch_node_at(&dt, 0) != nullptr && rpc_client_dispatch_node_at(&dt, 0)->length == 2 * BPS,
        "discover: the HUGE_SHM kept the span its body declared");
  CHECK(rpc_client_dispatch_node_at(&dt, 1) != nullptr && rpc_client_dispatch_node_at(&dt, 1)->base_addr == space + 2 * BPS,
        "discover: the next slot starts after the whole HUGE_SHM, not inside it");
  CHECK(rpc_client_dispatch_node_at(&dt, 2) != nullptr && rpc_client_dispatch_node_at(&dt, 2)->base_addr == space + 3 * BPS,
        "discover: the slot after that starts one Split later");
}

static void test_every_slot_kind_is_recorded_in_order(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * BPS, RPC_CLIENT_FUNC, 0);
  harness_stamp_slot(space + 1 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 2 * BPS, RPC_CLIENT_HUGE_SHM, BPS);
  harness_stamp_slot(space + 3 * BPS, RPC_CLIENT_FUNC, 0);

  int rc = discover();

  CHECK(rc == 1, "discover: a four slot layout is accepted");
  CHECK(node_count() == 4, "discover: every Split became a slot");
  CHECK(rpc_client_dispatch_node_at(&dt, 0)->d_t == RPC_CLIENT_FUNC && rpc_client_dispatch_node_at(&dt, 1)->d_t == RPC_CLIENT_SHM &&
            rpc_client_dispatch_node_at(&dt, 2)->d_t == RPC_CLIENT_HUGE_SHM && rpc_client_dispatch_node_at(&dt, 3)->d_t == RPC_CLIENT_FUNC,
        "discover: the kinds came back in the order they sit in the space");
  CHECK(rpc_client_dispatch_node_at(&dt, 1)->length == BPS, "discover: an SHM slot spans exactly one Bytes per Split");
}

/*
  A zero split_number cannot reach here through the handshake, which refuses it.
  The client asserts on states a correct caller cannot reach rather than handing
  back a value the caller might plough on from.
*/
static void run_discover_with_zero_splits(void) {
  harness_bind(space, snap, SPACE_LEN);
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = 0;
  hdh.bytes_per_split = BPS;
  memset(&dt, 0, sizeof dt);
  rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN);
}

static void test_zero_splits_is_a_caller_error(void) {
  int status = harness_run_isolated(run_discover_with_zero_splits, 5);
  CHECK(status != 0 && status != 126,
        "discover: zero splits aborts -- the handshake already refused that layout");
}

/* a length the layout cannot produce is the caller's arithmetic, not the host's */
static void run_discover_with_a_mismatched_length(void) {
  harness_bind(space, snap, SPACE_LEN);
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = 3; /* 3 x 4096 != 16384 */
  hdh.bytes_per_split = BPS;
  memset(&dt, 0, sizeof dt);
  rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN);
}

static void test_a_mismatched_length_is_a_caller_error(void) {
  int status = harness_run_isolated(run_discover_with_a_mismatched_length, 5);
  CHECK(status != 0 && status != 126,
        "discover: a length the layout cannot produce aborts as a caller error");
}

/*
  A HUGE_SHM whose size field is still zero: the host has not published it yet.
  Falling back to a single Split would hand back a table that silently breaks a
  multi-Split node into several -- the very defect this walk exists to avoid --
  so the layout is refused instead of reported as understood.
*/
static void test_an_unpublished_huge_size_is_refused(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * BPS, RPC_CLIENT_HUGE_SHM, 0);
  harness_stamp_slot(space + 1 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 2 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 3 * BPS, RPC_CLIENT_SHM, 0);

  CHECK(discover() == 0,
        "discover: a HUGE_SHM with no published size is refused rather than guessed at");
}

static void test_an_oversized_huge_size_is_refused(void) {
  harness_bind(space, snap, SPACE_LEN);
  /* a size that would run past the end of the space */
  harness_stamp_slot(space + 0 * BPS, RPC_CLIENT_HUGE_SHM, 8 * BPS);

  CHECK(discover() == 0, "discover: a span that overruns the address space is refused");
}

/*
  A refusal must leave the caller with no table at all. The mid-walk case is the
  one that matters: by the time a later slot turns out to be unusable, earlier
  nodes are already linked into the structure.
*/
static void test_a_refusal_clears_an_earlier_table(void) {
  /* build a real table first, in the structure we are about to reuse */
  harness_bind(space, snap, SPACE_LEN);
  for (unsigned int i = 0; i < SPLITS; i++) {
    harness_stamp_slot(space + (i * BPS), RPC_CLIENT_SHM, 0);
  }
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = SPLITS;
  hdh.bytes_per_split = BPS;
  CHECK(rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN) == 1,
        "discover: a good layout built a table");
  CHECK(node_count() == 4, "discover: four slots were found");
  rpc_client_dispatch_destroy_node(&dt); /* the caller's job before reuse */

  /* now refuse into the same structure, part way through the walk */
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 1 * BPS, RPC_CLIENT_HUGE_SHM, 9 * BPS); /* overruns */
  memset(&hdh, 0, sizeof hdh);
  hdh.split_number = SPLITS;
  hdh.bytes_per_split = BPS;

  CHECK(rpc_client_discover_dispatch(&dt, &hdh, space, SPACE_LEN) == 0,
        "discover: the overrunning slot was refused");
  CHECK(node_count() == 0 && dt.head == nullptr && dt.total_length == 0,
        "discover: a mid-walk refusal frees the half-built table and empties the structure");
}

static void test_destroying_the_table_leaves_the_space_alone(void) {
  harness_bind(space, snap, SPACE_LEN);
  harness_stamp_slot(space + 0 * BPS, RPC_CLIENT_FUNC, 0);
  harness_stamp_slot(space + 1 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 2 * BPS, RPC_CLIENT_SHM, 0);
  harness_stamp_slot(space + 3 * BPS, RPC_CLIENT_SHM, 0);
  discover();

  char before[SPACE_LEN];
  memcpy(before, space, SPACE_LEN);

  rpc_client_dispatch_destroy_node(&dt);

  CHECK(dt.head == nullptr && dt.end == nullptr, "discover: teardown clears the table");
  CHECK(memcmp(before, space, SPACE_LEN) == 0,
        "discover: teardown never touches the caller's Address Space");
}

int main(void) {
  test_a_huge_node_spanning_two_splits_is_stepped_over();
  test_every_slot_kind_is_recorded_in_order();
  test_zero_splits_is_a_caller_error();
  test_a_mismatched_length_is_a_caller_error();
  test_an_unpublished_huge_size_is_refused();
  test_an_oversized_huge_size_is_refused();
  test_a_refusal_clears_an_earlier_table();
  test_destroying_the_table_leaves_the_space_alone();
  return REPORT();
}
