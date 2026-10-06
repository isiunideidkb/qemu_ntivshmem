# Synchronisation is strict single-writer ping-pong

At any instant exactly one side writes the Address Space. Within a DIALOG the initiating side sets the Gate and the other side clears it, so ownership alternates strictly and no two writers ever overlap.

Content is written first and the Gate is published **last**, as the single point at which the write becomes visible to the other side.

**Considered options**: atomic whole-structure stores (`rpc_host.h` currently does this) — rejected as the definition because it makes the publication point implicit in a compiler builtin rather than explicit in the protocol; concurrent writes to disjoint regions — rejected, it would require real memory barriers and forfeit portability, which is the point of the project.

**Consequences**: no memory barriers are needed beyond release ordering, and no structure can ever be observed half-written. The cost is throughput — the two ends can never work in parallel, only in turn.

**Scope of zeroing**: the host zero-fills the **entire** Address Space at the end of the Hello DIALOG and again at the end of the Dispatch DIALOG — not after every function call. The draft's per-call "client clean result" is the only cleanup inside the function stage. This distinction matters at multi-gigabyte sizes.
