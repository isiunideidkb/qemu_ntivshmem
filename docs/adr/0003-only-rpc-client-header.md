# Only `common/rpc_client.h` may be modified

This contribution adds a client implementation to the original author's project. The **only file that may be written is `common/rpc_client.h`**. Nothing else changes — not `rpc_host.h`, not the guest driver, not the build files, not the draft.

Client:
- is a **user-mode** program, not kernel code. It receives the BAR2 user-mode virtual address from the driver's existing IOCTL and speaks the protocol through ordinary memory access. No kernel primitives, no IRPs.
- is **simpler than the host**: only the minimal negotiation. It mirrors the host's implementation style but does not replicate its full weight.
- is **standard C23**, header-only, mirrored on `rpc_host.h`.

**Considered options**: fixing the host deviations found in the audit (rejected — outside this contribution's remit; the host belongs to the original author); implementing the client inside `cusdriver.c` as kernel code (rejected — the driver is strict C89/WDK and the protocol client needs none of the kernel machinery).

**Consequences**: the header must be self-contained — shared types cannot be extracted into a common header, because extracting them would require editing `rpc_host.h`. The client's known-interop gap with the current host stays open until the author corrects the host; this is a documented handoff item, not a defect in this work.
