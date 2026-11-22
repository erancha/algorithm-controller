# 0002: Pixels live in shared memory and never cross gRPC

- **Status:** Accepted
- **Date:** 2026-07-21

## Context

A production-class inspection camera writes 16 MiB frames at about 200 frames per second, and
every frame of a slice is read again by a compute node. Copying pixels through the control calls
([0001](0001-grpc-control-plane.md)) would double the memory traffic and make the controller a
bottleneck. Real tools move pixels into dedicated image memory by DMA, outside their control
middleware.

## Decision

A memory box holds the pixels: one POSIX shared-memory segment of fixed, picture-sized slots on
the memory-box machine, mapped by every process there. `libmembox` hides it behind three calls:
`write_frame` allocates a slot through `membox-manager` and copies the picture in; `read_frame`
is pointer arithmetic on the mapping, with no call at all; `release_slots` returns a slice's
slots. Control calls carry slot numbers, never pixels.

## Consequences

- A compute node reads frames in place, so its memory does not grow with frame size.
- Compute nodes must run on the memory-box machine. Moving them elsewhere is erancha/
  algorithm-controller#1.
- The slot count must cover the largest slice, since slots are released only when a slice ends;
  `scripts/start.sh` derives it from the frame size and refuses a size `/dev/shm` cannot hold.
- A hardware backend that maps device memory could replace the shared-memory segment behind the
  same three calls.
