#!/usr/bin/env bash
#
# Ticket 07 -- the configuration matrix.
#
# Every test must compile and pass under each platform switch combination, and
# the waitpkg-only tests must report a skip rather than a failure on a CPU that
# has no waitpkg. Nothing here includes the host header.
#
# Usage: ./run-tests.sh [repo-root]

set -u

ROOT="${1:-$(cd "$(dirname "$0")/../../.." && pwd)}"
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# The three configurations the ticket calls for. They must be genuinely
# different: the earlier version listed "system only" and "automatic" as the
# same command line, so the auto-select branch was never compiled at all, and
# "waitpkg only" still defined ENABLE_UNIX so it was not waitpkg only either.
#
# "automatic" carries -mwaitpkg deliberately. Without it no configuration has
# the waitpkg path and a system api at once, so the rollback tests skipped in
# every configuration and cond4's waitpkg arm was never compiled. With it, cond4
# picks up both backends by itself, which is exactly what "automatic" means.
CONFIGS=(
  "system only|-DENABLE_UNIX"
  "waitpkg only|-DENABLE_WAITPKG -mwaitpkg"
  "automatic|-mwaitpkg"
)

total_fail=0
total_skip=0

# The host is out of scope and its header does not compile. No test may reach
# for it, not even in another configuration.
if grep -lE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"common/rpc_host\.h"' \
     "$TESTS_DIR"/*.c "$TESTS_DIR"/harness.h >/dev/null 2>&1; then
  echo "REFUSING: a test includes the host header, which is out of scope"
  exit 1
fi

for test_src in "$TESTS_DIR"/[0-9][0-9]-*.c; do
  name="$(basename "$test_src" .c)"
  for config in "${CONFIGS[@]}"; do
    label="${config%%|*}"
    flags="${config##*|}"
    bin="$OUT/$name.$(echo "$label" | tr ' ' '_')"

    printf '%-34s %-12s ' "$name" "$label"

    if ! gcc -std=c23 -I"$ROOT" -pthread -D_DEFAULT_SOURCE -Wall -Wextra $flags -o "$bin" "$test_src" 2>"$OUT/cc.log"; then
      echo "COMPILE FAIL"
      sed 's/^/    /' "$OUT/cc.log" | head -5
      total_fail=$((total_fail + 1))
      continue
    fi

    if ! out="$("$bin" 2>&1)"; then
      echo "TEST FAIL"
      echo "$out" | grep -E '^FAIL' | sed 's/^/    /'
      total_fail=$((total_fail + 1))
      continue
    fi

    skips="$(echo "$out" | grep -c '^skip' || true)"
    total_skip=$((total_skip + skips))
    if [ "$skips" -gt 0 ]; then
      echo "PASS ($skips skipped)"
    else
      echo "PASS"
    fi
  done
done

echo
if [ "$total_fail" -eq 0 ]; then
  echo "ALL PASSED (${total_skip} skipped)"
  exit 0
fi
echo "FAILURES: $total_fail"
exit 1
