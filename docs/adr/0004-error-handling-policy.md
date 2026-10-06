# Errors: assert on the caller, return on the peer, never both log and return

Three channels, each with exactly one job:

- **assert** guards undefined behaviour — a state a correct caller cannot reach. It reports to the developer and aborts. It is never used to validate the peer's data.
- **return** reports a real failure to the caller, who can act on it.
- **LOG** is a warning to the developer, used only where no return channel carries it. It is not a journal, so there are no `[INFO]` lines.

**An error that is returned is not also logged.** The return value *is* the warning; logging it again duplicates the signal and encourages LOG to be read as a record rather than as a warning.

**A situation that should end the session aborts.** Returning a value implies the caller may continue, and a bare `return` in particular tells them nothing.

**Considered options**: returning an error code for caller mistakes (rejected — the caller has no better move and the mistake stays invisible); logging alongside every return (rejected — turns LOG into a journal the developer learns to ignore); validating everything defensively at runtime (rejected — hides bugs in the caller behind silent error paths).

**Consequences**: `rpc_client_discover_dispatch` returns 0 only for the host's unusable data. A zero `split_number`, a too-small `bytes_per_split`, or a length the layout cannot produce now abort, because the dispatch handshake already refused those. Callers must treat a non-1 return as a real, reportable failure rather than a state to retry through.
