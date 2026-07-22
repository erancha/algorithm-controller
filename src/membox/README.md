# membox

Client library (`libmembox`) plus the `membox-manager` process — together the pixel plane of the
[main design](../../README.md#component-apis): a bank of picture-sized slots that camera, nodes,
and controller share without pixels ever crossing gRPC.

## Two-plane split

Every call in `Membox` (`write_frame`, `read_frame`, `release_slots`) picks one of two paths:

- **Bookkeeping, over gRPC** — `write_frame` calls `AllocSlot` on `membox-manager` to obtain a
  slot before writing into it; `release_slots` calls `ReleaseSlots` (⑦). Both carry only slot
  numbers — small messages, regardless of frame size — and both carry a 10 s deadline, so a hung
  manager surfaces as a thrown error, never a stalled caller.
- **Pixels, via mapped shm** — `write_frame`'s payload write is a `memcpy` into the process's own
  mapping of the shared segment; `read_frame` is pure pointer arithmetic (`base + slot`), no RPC
  at all. `membox-manager` never sees a pixel; it only ever allocates and frees offsets.

`membox-manager` creates the POSIX shm segment at startup, sized `slots × frame_bytes` (its
`--slots` and `--frame-bytes` flags); every other process opens the same segment name read-only
(compute nodes, controller) or read-write (camera). A fresh `create` first unlinks any stale
segment of the same name, so a restart never inherits a previous run's mapping.

## Backend abstraction

Callers link `Membox` and see only three calls — `write_frame`, `read_frame`, `release_slots` —
never `ShmSegment` itself. `SlotOffset` is an opaque byte offset that `read_frame` resolves as
`base + slot`, where `base` comes from whichever backend mapped the segment. Today only one
backend ships: POSIX `shm_open` + `mmap` (`src/membox/shm_segment.cpp`). Because no caller above
`Membox` touches `ShmSegment`'s internals, a future backend that maps physical device memory
instead of a POSIX shm object could implement the same three calls with the same `SlotOffset`
semantics and the same zero-copy aliasing — no caller (camera, controller, or compute node) would
need to change to use it.

## Slot = byte offset

A slot is a byte offset into the segment, always a multiple of `frame_bytes`: `FreeList` hands out
offsets `0, frame_bytes, 2*frame_bytes, ...` as slices allocate and release. `read_frame` rejects
any offset that is not aligned to `frame_bytes`, or that would read past the segment's end
(`std::out_of_range`), and `write_frame` rejects a payload whose size does not match `frame_bytes`
(`std::invalid_argument`) — both fail fast rather than reading or writing out of bounds.

## Aliasing spans, and the release-invalidates-views rule

`read_frame` returns a `std::span<const std::byte>` that aliases the mapping directly — no copy.
Two independent mappings of the same shm object observe the same bytes at the same offset, and a
mutation through one is immediately visible through the other's earlier span
(`tests/membox_test.cpp`, `WriteThenReadAliasesSameBytes`).

That aliasing is exactly why a released slot's span must not outlive the release: once
`release_slots` (⑦) returns a slot's offset to the manager's free list, a later `write_frame` (③)
for a different frame may reuse that same offset. A span obtained from `read_frame` before the
release keeps pointing at the same bytes, which now hold someone else's frame — the caller, not
the library, is responsible for not reading through a view whose slot has since been released.

## Flags (`membox-manager`)

`--listen addr` (default `localhost:50050`), `--slots N` (default 512), `--frame-bytes N`
(default 65536), `--shm name` (default `/algctl-membox`).

See the root README's [Component APIs](../../README.md#component-apis) — blocks Ⅴ (`read_frame`)
and Ⅵ (`write_frame`, `release_slots`) are this library's public surface.
