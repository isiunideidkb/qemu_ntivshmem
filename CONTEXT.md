# qemu_ntivhsmem

A host/guest RPC protocol carried over qemu's `ivshmem-plain` shared memory. It aims at universal, portable, concise remote calls with as little copying as possible — ultimately serving OpenGL 3D acceleration and clipboard sharing for WinXP and old Unix guests.

## Language

### Structure

**Address Space**:
The single shared memory region through which host and guest exchange everything. Addressed by platform-independent offset, never by a native pointer.

**Platform-Independent Offset**:
A 4-byte unsigned offset from the base of the Address Space, used in place of any host- or guest-native pointer.
_Avoid_: pointer, physical address, PhysAddr

**Split**:
One fixed-size division of the Address Space. Everything placed in the space is measured in Splits.

**Bytes per Split**:
The size of one Split, uniform across the whole Address Space. Chosen so that Functions tile it exactly.

### Synchronisation

**DIALOG**:
One complete exchange between host and guest, scoped to a single stage — the handshake, the dispatch, or one function call. Each DIALOG can succeed or fail independently of the others.
_Avoid_: session, transaction, round-trip, conversation

**Gate**:
The synchronisation word a DIALOG turns on. Exactly one side writes at a time: the caller sets the Gate, the other side observes it and clears it, and ownership alternates strictly within the DIALOG. Because ownership never overlaps, the Gate is the only ordering primitive the protocol needs.
_Avoid_: flag, lock, semaphore, signal, doorbell

**Marker**:
A read-only constant placed in the Address Space that identifies what a structure is. Neither side ever writes it as a synchronisation act.
_Avoid_: flag, tag, type field, magic number

### Protocol stages

**Hello**:
The handshake DIALOG that opens a connection.
_Avoid_: handshake, init, preamble, greeting

**Capability Mask**:
The bits a guest declares to advertise which optional protocol features it supports. The host reads it to decide what it may rely on.
_Avoid_: feature flags, options, version, feature set

**Dispatch**:
The DIALOG in which the host declares the set of Functions it offers, and how the Address Space is divided among them.
_Avoid_: registration, setup, negotiation, enrolment

**Physical Address Passthrough**:
A capability in which the host exposes a region of its own physical memory to the guest directly, sparing the copy through the Address Space. Offered only when both ends advertise it.
_Avoid_: zero-copy, DMA, shared page, mmap

**Function**:
A single callable entry the host offers, occupying a whole number of Splits in the Address Space.
_Avoid_: RPC, method, endpoint, routine
