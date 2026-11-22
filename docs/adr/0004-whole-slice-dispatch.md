# 0004: Dispatch begins once the whole slice is imaged

- **Status:** Accepted
- **Date:** 2026-07-21

## Context

A frame-in-die position can be scored as soon as its frame has landed in every die, which can be
long before the whole slice is imaged. Overlapping imaging with compute would need the controller
to count each position's landed dies while the camera stream is still open, and would turn the
"slice imaged" report from a dispatch gate into a completion barrier.

## Decision

The controller sends the first invocation only after the camera reports the slice imaged, and
releases the slice's slots only after every position has replied. Acquisition and compute stay
strictly sequential within a slice. [Streaming dispatch](../streaming-dispatch.md) records the
overlapping variant and its expected gain.

## Consequences

- Each slice's timing is the imaging time plus the compute time; with the default camera pacing,
  imaging dominates.
- The controller's dispatch logic stays a simple pull loop: one worker per registered node, each
  keeping one call in flight.
- The slot count must cover a whole slice ([0002](0002-pixels-in-shared-memory.md)).
