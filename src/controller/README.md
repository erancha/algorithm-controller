# controller

Serves ① `ProcessSlice`, the [main design](../../README.md#ⅰ-tool-driver--controller-①)'s public
entry point: images a slice, builds the frame index from the placement stream, dispatches one
`ProcessPosition` per frame-in-die position, and gathers the replies. Also serves Ⅶ
`RegisterNode`, through which compute nodes announce themselves
(`src/controller/controller_service.hpp`).

## Ⅶ: node pool by registration

The controller takes no node addresses. Each compute node calls `RegisterNode` with its own
endpoint at startup; the controller dials a channel back to that endpoint and adds it to a
mutex-guarded pool keyed by endpoint, so re-registering (a restarted node) replaces the
connection instead of duplicating the entry. Dispatch reads the live pool at every checkout
([Ⅳa](#ⅳa-free-node-dispatch-from-the-live-pool)), so a node registering mid-slice starts
serving the slice in flight. An arriving slice with an empty pool waits up
to 10 s for the first registration — nodes and controller launch concurrently, so "no node has
registered yet" is a legal startup state, bounded rather than absorbed — then fails with
`FAILED_PRECONDITION, "no compute nodes registered"`.

Eviction is registration's inverse: a node whose `ProcessPosition` call fails or times out is
removed from the pool, so a dead or stalled node costs the slice in flight, never every slice
after it. A restarted node re-registers and returns. Eviction is skipped when the endpoint
re-registered after the failed call checked it out — the pool then holds a fresh connection the
observed failure says nothing about.

## Building the frame index from the ④ stream

`ProcessSlice` opens one streaming `ImageSlice` call to the camera — bounded by a 5-minute
deadline, generous because a paced slice legitimately images for seconds — and reads it to
completion. Each
`FrameStored` message (④) becomes one `(die, frame_in_die) → slot` entry in a
`std::map<std::pair<uint32_t, uint32_t>, uint64_t>`; a clean stream end is the completion signal
(⑤). That map *is* the frame index the design describes — a lookup table completed by the time
dispatch starts, not touched again while positions are in flight.

## Geometry derived from placements, never configured

`controller` takes no die-count or position-count flag. While consuming the stream it tracks the
maximum die index and maximum frame-in-die position seen; once the stream ends, `dies = max_die +
1` and `positions = max_pos + 1`. This is why a slice's die count can vary — `tests/e2e.sh` runs a
20-die slice and a 12-die slice back to back — with no controller restart or reconfiguration: the
count comes from what the camera actually reported, never from a flag.

## Fail-fast on an incomplete slice

Before dispatch, `ProcessSlice` checks `frame_index.size() == dies * positions`. Any mismatch —
some `(die, position)` pair the stream never reported — aborts the slice with
`FAILED_PRECONDITION, "incomplete slice"` rather than dispatching with holes in the input. An
empty stream (`"camera reported no frames"`) and a non-OK stream `Finish()` status are rejected
the same way, and on every abort path any slots the camera already wrote are released before
returning, so a partial slice never leaks memory-box capacity.

## Ⅳa: free-node dispatch from the live pool

Once the frame index is complete, `ProcessSlice` composes each position's slot list in die order
(Ⅳa — see the [worked trace](../../README.md#ⅳa-from-lookup-table-to-invocation)) and dispatches
positions in order: check out a free node from the live pool — blocking while every node is
mid-call, since each in-flight invocation is bounded by the per-position deadline — and call that
node's `ProcessPosition` (⑥) on its own thread, writing the reply into `values[position]`. A node
serves at most one call at a time, and because every checkout reads the live pool, a node that
registers while the slice is already dispatching starts receiving positions immediately — the
pool being non-empty is enough to begin, and concurrency grows as registrations land. A `ProcessPosition` failure or timeout on any node evicts that node,
stops new dispatch, releases the slice's slots, and returns that error as the slice's result — a
stalled node and a crashed node end the same way, as a bounded error.

## Flags

`--listen addr` (default `localhost:50052`), `--camera addr` (default `localhost:50051`),
`--manager addr` (default `localhost:50050`), `--shm name` (default `/algctl-membox`),
`--frame-bytes N` (default 65536), `--camera-stream-deadline-s N` (default 300) and
`--position-deadline-s N` (default 60) — the two internal deadlines scale with frame size, since
a position's compute cost grows with its pixel count; `scripts/start.sh` derives both from
`--frame-dim`. The driver's overall `--deadline-s` must stay above them so the controller's more
specific diagnoses fire first.
