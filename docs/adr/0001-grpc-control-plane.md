# 0001: gRPC and Protobuf carry every call between processes

- **Status:** Accepted
- **Date:** 2026-07-21

## Context

The design (`docs/architecture.md`) names seven calls between five processes: unary calls with
deadlines, and one stream of per-frame reports that ends when the slice is imaged. The
implementation runs each part as its own OS process, so those calls need a transport, a
serialization format and a written contract that the processes cannot drift from.

## Decision

Every call between processes is a gRPC call, defined once in `src/proto/inspection.proto`. The
camera's per-frame reports are a server stream whose normal end is the "slice imaged" event; an
error status before the end marks the slice failed. Every call carries a deadline.

Rejected alternatives:

- **Plain TCP**: framing, serialization, request correlation, timeouts and the event stream
  would all be hand-written, burying the design under plumbing.
- **ZeroMQ**: lighter, but without an interface definition the contract lives only in prose.
- **DDS**: strong at publish-subscribe, but heavy for five point-to-point links.

## Consequences

- The `.proto` file is the contract; changing a call means changing it, and every binary
  rebuilds against it.
- gRPC is linked statically into every binary, which makes each link use a few GB of memory;
  `scripts/common.sh` caps build parallelism at the core count for that reason.
- Pixels must not travel this way; [0002](0002-pixels-in-shared-memory.md) keeps them out.
