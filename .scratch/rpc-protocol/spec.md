# Spec: RPC Client

**Status:** ready-for-agent

## Problem Statement

This project carries a host/guest RPC protocol over qemu's `ivshmem-plain` shared memory. It has a complete design draft and a host-side implementation, but **no client at all** — the client header is six lines of comment, and the guest driver never speaks the protocol.

So the protocol cannot be exercised by anyone. Nothing on either end has been run against anything: the host header does not even compile, and there is no second participant to talk to. A contributor asked by the original author to implement the client side has no way to know whether their work is right, because there is no counterpart to test against and no agreed target to test to.

The contributor needs a client that can be **written and verified on its own**, against the draft as the authority, without waiting on the host to be fixed.

## Solution

A header-only C23 client implementing the passive side of all three DIALOGs of the protocol. It takes the Address Space base and its length as ordinary parameters, and speaks the protocol through plain memory access — no kernel involvement, no driver, no host cooperation.

Because the entire interface between the two ends is the Address Space itself, the client is verifiable in isolation: a test writes fabricated host-side bytes into a buffer and asserts the bytes the client writes back. The host is never involved.

## User Stories

1. As the contributor, I want a client that speaks the protocol as the draft defines it, so that my work targets the specification rather than the host's current behaviour.
2. As the contributor, I want the client to take the Address Space base and length as parameters, so that the client does not care how they were obtained.
3. As the contributor, I want the client to be header-only, so that it can be dropped into any translation unit without a build step.
4. As the contributor, I want the client to be standard C23, so that it matches the project's stated coding style.
5. As the contributor, I want the client to mirror the host's shape under a distinct name prefix, so that a reader who knows one end can read the other.
6. As the contributor, I want the client's types to be self-contained, so that no change to the host header is required to use it.
7. As the contributor, I want a Hello DIALOG that waits for the host to raise the Gate, so that the client never writes before the host is ready.
8. As the contributor, I want the Hello DIALOG to validate the host's hello message, so that a mis-sequenced start is detected rather than silently accepted.
9. As the contributor, I want the Hello DIALOG to compare the host's advertised Address Space length against my own, so that a size mismatch is caught before anything is read.
10. As the contributor, I want the client to report a rejected Hello through the Capability Mask error bit, so that the host learns why through the protocol's own channel.
11. As the contributor, I want the client to lower the Gate after answering, so that the host is released to continue.
12. As the contributor, I want a Dispatch DIALOG that waits for the Gate, so that the same discipline governs every stage.
13. As the contributor, I want the Dispatch DIALOG to validate `split_number` and `bytes_per_split`, so that an impossible layout is rejected with the correct error code.
14. As the contributor, I want the Dispatch DIALOG to answer with the protocol's error codes, so that the host can distinguish an illegal function count from an illegal space count.
15. As the contributor, I want to discover the dispatched Functions by walking the Address Space Split by Split, so that the client derives every slot offset itself from information the host already sent.
16. As the contributor, I want the discovered Functions held in a table I own, so that I can inspect, traverse, and free them under my own control.
17. As the contributor, I want a Function's kind to be read from its head in the Address Space, so that the client does not need a separate function catalogue.
18. As the contributor, I want a HUGE_SHM Function's span read from the size field in its body, so that the client follows the draft's layout rather than inventing one.
19. As the contributor, I want a stage-3 servicer that waits for a slot's Gate and lowers it after servicing, so that Function calls follow the same handshake as the earlier stages.
20. As the contributor, I want to register a handler for FUNC slots, so that callable Functions can be serviced by my own code.
21. As the contributor, I want SHM and HUGE_SHM slots to need no handler, so that shared-region access stays the caller's business.
22. As the contributor, I want a waitpkg-based path, so that the client can park on the Gate on CPUs that support it.
23. As the contributor, I want a system-based path, so that the client works on platforms without waitpkg.
24. As the contributor, I want to inject my own wait routine for the system path, so that the client does not dictate how the process blocks.
25. As the contributor, I want an automatic router that prefers waitpkg when it is both compiled in and available at runtime, so that I do not have to choose.
26. As the contributor, I want the waitpkg path to fall back on its own when the running CPU lacks the feature, so that a binary built with waitpkg still runs on older hardware.
27. As the contributor, I want the system path to fail loudly when no waiter was injected, so that I do not silently get a spin loop.
28. As the contributor, I want each routine to return the protocol's result codes, so that I can branch on success, peer error, and unimplemented paths.
29. As the contributor, I want to free the discovered table without freeing my own buffer, so that the client never owns memory it did not allocate.
30. As the contributor, I want a test seam that is only the Address Space buffer, so that tests assert what the client writes and nothing about how it is written.
31. As the contributor, I want to drive the Gate deterministically in tests through an injected waiter, so that tests do not depend on a real CPU park or on timing.
32. As the contributor, I want tests that never include the host header, so that my verification does not depend on code I am not allowed to change.
33. As the contributor, I want the client to compile under each supported platform switch combination, so that no configuration is broken by construction.
34. As the contributor, I want a written record of where the host deviates from the draft, so that the author can act on it without me modifying their code.
35. As the original author, I want a client that matches the draft, so that when the host is corrected the two ends meet without a second round of negotiation.

## Implementation Decisions

**Authority and scope.** The design draft is the authoritative specification; the host implementation is a deviating artefact. Only the client header may be written — no host, driver, build, or draft changes.

**Shape.** The client mirrors the host header's structure under a `rpc_client_*` prefix: one base entry per operation, plus `_umonitor` and `_system` variants, plus a router. Types are self-contained and never reuse the host's — a `host_*_t` name must not appear, since the client's own types would then be misnamed.

**The client is passive.** In every DIALOG the host raises the Gate (writes 1) and the client lowers it (writes 0). The client never raises a Gate of its own initiative. It waits for the Gate to become 1, does its work, then writes 0.

**Synchronisation model.** Strict single-writer ping-pong: at any instant exactly one side writes the Address Space, so no memory barriers are needed beyond release ordering and no structure can be observed half-written. Content is written first and the Gate is published last. The host zero-fills the entire Address Space only at the end of the Hello and Dispatch DIALOGs, not per Function call.

**Parameters.** The Address Space base and its length are plain parameters (`void*` and `unsigned int`). The client takes no interest in their provenance.

**Capability Mask.** bit0 declares UMONITOR/WAITPKG support, bit1 declares Physical Address Passthrough, bit2 is the client's error return, which the client sets and the host clears. The client answers the Hello with its own mask, setting bit2 when it rejects.

**Function numbering** follows the draft: `0x0` is huge-page SHM, `0x1` is SHM, `0x2` is rpc. The host's current enum has `0x0` and `0x1` swapped; that is a host defect recorded for the author, not a reason for the client to diverge.

**Discovery.** The client walks the Address Space Split by Split, `bytes_per_split` at a time, for `split_number` Splits, recording each slot's kind from its head and its offset. A HUGE_SHM node's span is taken from the size field in its body, per the draft; the client relies on the host having written it by the time discovery runs. This is the one place the draft under-specifies, and the obligation it places on the host is recorded below.

**Routing.** Each base entry checks the compile-time waitpkg switch, then the runtime CPU feature, and only then routes to the waitpkg variant; otherwise it routes to the system variant. The waitpkg variant repeats the runtime check and falls back to the system variant itself. The feature question may therefore be asked twice, which is deliberate and harmless.

**The system path needs an injected waiter.** It is a function pointer receiving an address, listening for change in that one-byte region. If none is injected the path cannot block and fails loudly rather than spinning.

**Naming the waiter setter.** The client exposes a `set_client_specific_waiter` in the same spirit as the host's `set_host_system_specific_waiter`, so the two ends read alike.

**Return conventions** mirror the host: Hello returns 1 for success, 0 when the client rejected, -1 when not compiled in. Dispatch returns 1 success, 0 illegal function count, -1 illegal space count, -2 reserved error, -3 not compiled in. Discovery returns 1 success, 0 inconsistent layout, -1 nothing found.

## Testing Decisions

**What makes a good test here.** Only external behaviour: write bytes into an Address Space buffer, call a client entry point, assert the bytes the client wrote back and the Gate's final value. No test may assert on private helpers, internal field ordering, or the client's call sequence. If a test would survive a rewrite of the header's internals while its observable writes stayed the same, it is a good test.

**The seam — exactly one.** The Address Space byte buffer. It is the real boundary between the two ends, so it is the highest possible seam and requires no host code. Tests fabricate the host side as raw bytes; the host header is **never included**, both because it does not compile and because the host is out of scope.

**The waiter is the second half of the same seam.** Tests inject a waiter that flips the Gate to 1, so the system path can be driven deterministically with no real CPU park and no timing dependence. Tests must not depend on wall-clock waiting.

**Modules under test.** The Hello DIALOG (router, system path, and the waitpkg path where the CPU supports it), the Dispatch DIALOG (same three), discovery including the table it builds, the stage-3 servicer, and table teardown.

**Configurations.** The client header must compile, and its tests run, under each platform switch combination: system-only, waitpkg-only, and automatic selection. The waitpkg path is skipped at runtime, not failed, on CPUs without the feature.

**Prior art.** None — the repository has no test infrastructure, build system, or CI. This spec establishes the first, and it deliberately introduces the smallest one that works: a single C file compiled with a C23 compiler, living in the scratch area rather than the repository.

**Where the tests live.** Under the scratch area, not in the repository, so the contribution stays confined to the client header.

## Out of Scope

- Any change to the host implementation, including the fact that it does not currently compile, and including its deviations from the draft. These are reported to the author, not fixed here.
- The guest driver and its wait primitives.
- Making the two ends talk to each other end to end. That cannot happen until the host compiles and is corrected, and it is not this contribution's job.
- Parallel or threaded servicing of derived Functions. The servicer is sequential.
- Kernel-mode operation of any kind.
- The author's separate fork effort, which is unrelated to this work.
- Any change to the draft itself.

## Further Notes

**Items to report to the author, none of which the client can fix:**

1. The host header does not compile in any configuration: a `void*` is passed to an atomic store that requires a typed pointer, in a routine outside every platform guard.
2. The host's Function numbering has the huge-page SHM and SHM codes swapped relative to the draft.
3. The host publishes a HUGE_SHM node's size only while running that node, not at Dispatch time. The client's discovery relies on the size being present earlier.
4. The host sets the Gate inside a single whole-structure store rather than publishing it last, and zeroes only the header rather than the whole Address Space at the end of the first two DIALOGs.
5. Size constraints differ from the draft: no lower bound of 2 MB on Bytes per Split, and an extra power-of-two requirement on the Address Space length.
6. The guest wait routine cannot link against its own log function, and the guest and host probe different CPU features for the same capability bit.
7. The shared CPU-feature probe header has no include guard, so it cannot be included alongside the client header in one translation unit.

**The one open question.** How a multi-Split HUGE_SHM node's span is learned at discovery is not defined by the draft. This spec resolves it by following the draft's own layout — read the size field in the node's body — and records the host-side obligation in item 3 above. If the author rules otherwise, this is the single decision to revisit.
