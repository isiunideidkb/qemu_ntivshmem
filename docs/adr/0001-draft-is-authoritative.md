# The draft is the authoritative protocol spec

`common/.draft` is the original author's protocol specification and is authoritative. Where `common/rpc_host.h` disagrees with it, **the code is the deviation** — the host implementation is work in progress, not the definition.

The client is written against the draft. Host deviations are **recorded and reported to the original author, not fixed here** (see ADR-0003). This still matters for the client, because it means the client must implement what the draft says, not what the host currently does — the two ends will not interoperate until the host is corrected on the author's side.

**Considered options**: code-as-truth (rejected — it would ratify unintentional drift and make the client a mirror of a bug); per-conflict judgement (rejected — it reopens the same argument at every conflict and gives the client no stable target).

**Consequences**: every conflict in `.scratch/rpc-protocol/audit-draft-vs-code.md` resolves in the draft's favour. Three deviations are known to block interoperability: size constraints, function numbering, and publication order.
