# 0003: Compute nodes announce themselves to the controller

- **Status:** Accepted
- **Date:** 2026-07-21

## Context

The controller hands each frame-in-die position to whichever compute node is free, so it needs
the set of nodes. The simplest source is a static list of node addresses given to the controller
at launch, but then every node's address must be known and kept correct before the controller
starts, and adding a node means reconfiguring and restarting it.

## Decision

Only the controller's address is well known. Each compute node calls `RegisterNode` with its own
address once at startup, waiting for the controller to come up, so launch order does not matter.
The pool is whatever has registered; a re-registration replaces the node's connection. A node
whose invocation fails or times out is dropped, and it returns by registering again. A slice
that finds the pool empty waits up to 10 s for a first registration, then fails.

## Consequences

- Adding a node means starting it; the controller takes no node configuration.
- A failed node costs only the slice in flight.
- The controller cannot tell a node that crashed before registering from one that was never
  started; at this scope that is acceptable.
