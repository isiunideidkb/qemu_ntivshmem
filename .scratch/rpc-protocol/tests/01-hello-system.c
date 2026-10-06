/*
  Ticket 01 -- the Hello DIALOG on the system path, and the test seam itself.

  The seam under test is exactly one thing: the Address Space byte buffer, plus
  the injected waiter that drives the Gate. Tests fabricate the host side as raw
  bytes and assert the bytes the client writes back.

  The host header is never included: it does not compile, and it is out of scope
  for this contribution.
*/
#include "harness.h"

/* Deliberately NOT a power of two: the draft fixes length as
   split_number * bytes_per_split, so 3 x 4096 is a legal address space and the
   client must not reject it. rpc_host.h wrongly requires a power of two. */
#define SPACE_LEN 12288u

static char space[SPACE_LEN];
static char snap[SPACE_LEN];
static rpc_client_handshake_t hh;

static void bind_and_publish(char hello_message, char host_cap_mask, unsigned int host_length) {
  harness_bind(space, snap, SPACE_LEN);
  harness_publish_hello(space, hello_message, host_cap_mask, host_length);
}

/* --- the seam itself --- */

static void test_the_seam_drives_the_protocol(void) {
  bind_and_publish(1, 0, SPACE_LEN);
  rpc_client_handshake_init(&hh, false, false, SPACE_LEN);

  int rc = rpc_client_run_hello_handshake_system(&hh, space);

  CHECK(rc == 1, "seam: a client entry runs against a fabricated host and returns a verdict");
  CHECK(g_waiter_calls >= 1, "seam: the injected waiter was actually used");
}

/* --- acceptance criteria --- */

/*
  The draft fixes the bits: bit0 = UMONITOR, bit1 = PhysAddr. These literals are
  the independent source of truth -- deliberately NOT the client's own idea of
  its mask, which is what made the earlier version of this test tautological.
*/
static void test_answers_with_the_drafts_capability_bits(void) {
  struct {
    bool phys;
    bool umonitor;
    char expected;
    const char *what;
  } cases[] = {
      {false, true, 0x01, "mask: umonitor only lands on bit0"},
      {true, false, 0x02, "mask: phys only lands on bit1"},
      {true, true, 0x03, "mask: both occupy bit0|bit1 and nothing above"},
      {false, false, 0x00, "mask: neither advertises no bits"},
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    bind_and_publish(1, 0, SPACE_LEN);
    rpc_client_handshake_init(&hh, cases[i].phys, cases[i].umonitor, SPACE_LEN);

    int rc = rpc_client_run_hello_handshake_system(&hh, space);

    CHECK(rc == 1 && space[1] == cases[i].expected, cases[i].what);
  }
}

static void test_accepts_a_valid_hello(void) {
  bind_and_publish(1, 0, SPACE_LEN);
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);

  int rc = rpc_client_run_hello_handshake_system(&hh, space);

  CHECK(rc == 1, "valid hello: reported as accepted");
  CHECK(space[1] == hh.cap_mask, "valid hello: client answered with its own Capability Mask");
  CHECK(space[0] == 0, "valid hello: client lowered the Gate");
}

static void test_rejects_a_malformed_host_capability_mask(void) {
  /* bit2 is the client's error return and the host is expected to leave it 0;
     bits 3-7 are reserved. the draft requires the client to check this. */
  char bad[] = {0x04, 0x08, 0x80};
  const char *what[] = {"malformed host mask (error bit set): rejected",
                        "malformed host mask (reserved bit3): rejected",
                        "malformed host mask (reserved bit7): rejected"};
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    bind_and_publish(1, bad[i], SPACE_LEN);
    rpc_client_handshake_init(&hh, false, true, SPACE_LEN);

    int rc = rpc_client_run_hello_handshake_system(&hh, space);

    CHECK(rc == 0 && (space[1] & RPC_CLIENT_CAP_ERROR) != 0, what[i]);
  }
}

static void test_rejects_a_bad_hello_message(void) {
  bind_and_publish(2, 0, SPACE_LEN); /* non-zero so the client proceeds, but not 1 */
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);

  int rc = rpc_client_run_hello_handshake_system(&hh, space);

  CHECK(rc == 0, "bad hello message: reported as rejected");
  CHECK((space[1] & RPC_CLIENT_CAP_ERROR) != 0, "bad hello message: error bit set for the host");
  CHECK(space[0] == 0, "bad hello message: client still lowered the Gate");
}

static void test_rejects_a_length_mismatch(void) {
  bind_and_publish(1, 0, SPACE_LEN * 2); /* host announces a different size */
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);

  int rc = rpc_client_run_hello_handshake_system(&hh, space);

  CHECK(rc == 0, "length mismatch: reported as rejected");
  CHECK((space[1] & RPC_CLIENT_CAP_ERROR) != 0, "length mismatch: error bit set for the host");
  CHECK(space[0] == 0, "length mismatch: client still lowered the Gate");
}

static void test_writes_nothing_while_the_gate_is_down(void) {
  bind_and_publish(0, 0, SPACE_LEN); /* gate down: byte 0 is 0 */
  harness_freeze();
  harness_raise_gate_on(3); /* the gate only comes up on the third waiter call */
  rpc_client_handshake_init(&hh, false, true, SPACE_LEN);

  int rc = rpc_client_run_hello_handshake_system(&hh, space);

  CHECK(g_untouched_while_gate_down == 1, "gate down: client wrote nothing while waiting");
  CHECK(g_waiter_calls >= 3, "gate down: client kept waiting rather than proceeding");
  CHECK(rc == 1, "gate down: once raised, the handshake completed normally");
}

static void test_a_rejection_still_lowers_the_gate(void) {
  bind_and_publish(1, 0, SPACE_LEN + 1); /* reject path */
  rpc_client_handshake_init(&hh, true, true, SPACE_LEN); /* both bits -> mask 0b11 */

  rpc_client_run_hello_handshake_system(&hh, space);

  /* the host is released either way, otherwise it would hang forever */
  CHECK(space[0] == 0, "rejection path: host is never left waiting on the Gate");
  CHECK(space[1] == (char)(hh.cap_mask | RPC_CLIENT_CAP_ERROR),
        "rejection path: answer is the client mask plus the error bit");
}

/* --- the injected waiter is mandatory on the system path --- */

static void run_without_a_waiter(void) {
  harness_drop_waiter();
  memset(space, 0, SPACE_LEN);
  space[0] = 1;
  rpc_client_handshake_init(&hh, false, false, SPACE_LEN);
  rpc_client_run_hello_handshake_system(&hh, space);
}

static void test_fails_loudly_without_a_waiter(void) {
  /* the child aborts, so it reports a non-zero status that is not the "killed"
     sentinel */
  int status = harness_run_isolated(run_without_a_waiter, 5);
  CHECK(status != 0 && status != 126,
        "no waiter injected: system path fails loudly instead of spinning");
}

int main(void) {
  /* this whole file is about the system path */
  if (!harness_have_system_backend()) {
    SKIP("the hello system path: no system backend compiled in for this configuration");
    return REPORT();
  }
  test_the_seam_drives_the_protocol();
  test_answers_with_the_drafts_capability_bits();
  test_accepts_a_valid_hello();
  test_rejects_a_malformed_host_capability_mask();
  test_rejects_a_bad_hello_message();
  test_rejects_a_length_mismatch();
  test_writes_nothing_while_the_gate_is_down();
  test_a_rejection_still_lowers_the_gate();
  test_fails_loudly_without_a_waiter();
  return REPORT();
}
