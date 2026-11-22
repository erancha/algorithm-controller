# Architecture

The design behind the [README](../README.md)'s Architecture section: every part, how one slice
moves through them, and how this implementation runs them on one host.
[The inspection tool](inspection-tool.md) defines the terms; [component APIs](component-apis.md)
gives each call's signature; the [ADRs](adr/) record the founding decisions.

**Contents:** [The parts](#the-parts) · [How a slice runs](#how-a-slice-runs) ·
[Shared mechanisms](#shared-mechanisms) · [Storage](#storage) ·
[Running on one host](#running-on-one-host) · [Codebase structure](#codebase-structure)

Arrow style shows how a call behaves: **thick**, the caller waits for a reply; **thin**, a one-way
message; **dotted**, data that moves without a command.

```mermaid
%%{init: {"flowchart": {"wrappingWidth": 400}}}%%
flowchart TB
    DRV[<b>Tool driver</b><br/>whatever runs the tool,<br/>one slice at a time]
    subgraph CTRLM[controller machine — one process]
        CTRL[<b>Controller</b><br/>drives one slice at a time,<br/>collects one value per frame-in-die position<br/><i>gRPC service</i>]
        IDX[<b>frame index</b><br/>lookup table:<br/>die index + frame-in-die → slot]
    end
    subgraph MBM[memory-box machine — every process on it maps the box's shared memory]
        subgraph ACQ[acquisition side]
            CAM[<b>Camera</b><br/>take picture per frame<br/><i>gRPC service</i>]
            MB[(memory box<br/>dedicated image memory,<br/>one picture per slot<br/><i>pixels: shared memory<br/>slot bookkeeping: gRPC</i>)]
        end
        subgraph NODES[compute nodes ×N — one process each]
            CN[<b>Compute node</b><br/>per invocation: runs an algo on the frames<br/>at one frame-in-die position across all dies,<br/>returns a numeric value<br/><i>gRPC service</i>]
        end
    end
    DRV == "①<sup>Ⅰ</sup>&nbsp;slice&nbsp;results&nbsp;⇐&nbsp;<b>process_slice</b>(slice&nbsp;id)" ==> CTRL
    CTRL -- "②<sup>Ⅱ</sup>&nbsp;<b>image_slice</b>" --> CAM
    CAM == "③<sup>Ⅵ</sup>&nbsp;<b>write_frame</b>&nbsp;into&nbsp;a&nbsp;free&nbsp;slot" ==> MB
    CAM -- "④<sup>Ⅲ</sup>&nbsp;<b>on_frame_stored</b>:&nbsp;die&nbsp;index,&nbsp;frame#8209;in#8209;die&nbsp;+&nbsp;slot" --> IDX
    CAM -- "⑤<sup>Ⅲ</sup>&nbsp;<b>on_slice_imaged</b>&nbsp;once&nbsp;all&nbsp;landed" --> CTRL
    IDX -. "each&nbsp;position's&nbsp;slots,&nbsp;in&nbsp;die&nbsp;order<sup>Ⅳa</sup>" .-> CTRL
    CTRL == "⑥<sup>Ⅳ</sup>&nbsp;value&nbsp;⇐&nbsp;<b>process_position</b>:&nbsp;one&nbsp;invocation<br/>per&nbsp;frame#8209;in#8209;die&nbsp;position,<br/>to&nbsp;whichever&nbsp;node&nbsp;is&nbsp;free" ==> CN
    CN == "<b>register_node</b><sup>Ⅶ</sup>(own&nbsp;endpoint),<br/>once&nbsp;at&nbsp;node&nbsp;startup" ==> CTRL
    MB -. "<b>read_frame</b><sup>Ⅴ</sup>:&nbsp;one&nbsp;frame,&nbsp;by&nbsp;its&nbsp;slot" .-> CN
    CTRL == "⑦<sup>Ⅵ</sup>&nbsp;<b>release_slots</b>" ==> MB
```

Two numberings tie the diagram to the code. Circled numbers (①–⑦) are the steps of one slice, in
order, one per solid arrow. Superscript roman numerals name the code blocks in
[component APIs](component-apis.md), and the arrow's **bold** function name heads that block. So
①<sup>Ⅰ</sup> reads "step 1, defined in block Ⅰ". A leading `x ⇐` names the call's return value.

Some arrows carry no step number. Dotted arrows carry data: the memory-box read is block Ⅴ, which
a node performs while serving ⑥, and the hop from the frame index to the controller builds each ⑥
invocation's slot list
([block Ⅳa](component-apis.md#ⅳa-from-lookup-table-to-invocation)). Registration (Ⅶ) happens once
per node at startup, before any slice exists, so it sits outside the ①–⑦ cycle.

Solid arrows travel over gRPC, with one exception: ③'s pixels land in the memory box through
shared memory, and only its slot bookkeeping is a gRPC call. The dotted read (Ⅴ) is pure shared
memory as well. Pixels therefore never cross gRPC, and every network message carries only
identifiers and numbers: gRPC gives the calls one written contract
([ADR 0001](adr/0001-grpc-control-plane.md)), and shared memory spares the pixels a copy
([ADR 0002](adr/0002-pixels-in-shared-memory.md)).

## The parts

- **Tool driver**: whatever runs the inspection tool. It calls `process_slice` once per slice.
- **Controller**: one process on its own machine. It runs one slice at a time and collects one
  value per frame-in-die position; [src/controller](../src/controller/README.md).
- **Frame index**: the controller's lookup table from (die index, frame-in-die position) to slot,
  filled as pictures land.
- **Camera**: photographs a slice and writes each picture into a free slot;
  [src/camera](../src/camera/README.md).
- **Memory box**: a bank of picture-sized slots in shared memory on the memory-box machine. Every
  process on that machine maps it; [src/membox](../src/membox/README.md).
- **Compute nodes**: separate processes on the memory-box machine, any number of them. Each runs
  the algorithm for one position at a time;
  [src/compute_node/algorithm](../src/compute_node/algorithm/README.md).

## How a slice runs

1. The tool driver calls `process_slice` ① with a slice id.
2. The controller asks the camera to photograph the slice with `image_slice` ②.
3. Each picture lands in a free slot ③. The camera reports which die and position that slot
   holds ④, and the controller records it in the frame index.
4. When the camera reports the whole slice imaged ⑤, the controller sends one invocation per
   position ⑥ to whichever compute node is free. An invocation carries that position's slots, one
   per die, and each node runs one invocation at a time.
5. The node reads those frames straight from the memory box, runs the algorithm on them, and
   replies with one number. The controller stores it at the position's index in the results.
6. When every position has replied, the controller releases the slice's slots ⑦, so the next
   slice's pictures can use them, and returns the results as the reply to ①.

## Shared mechanisms

- **Node registration.** The controller is never configured with node addresses. Each compute
  node announces its own address once at startup (Ⅶ), and the controller dispatches to whichever
  nodes have registered so far, so adding a node means starting it. A node whose invocation fails
  or times out is dropped and returns by registering again. A failed node costs only the slice in
  flight. The [controller README](../src/controller/README.md) has the details;
  [ADR 0003](adr/0003-nodes-register-themselves.md) weighs it against a static node list.
- **Deadlines.** Every call carries a deadline, so a stalled peer surfaces as an error instead of
  a hang.
- **Whole-slice dispatch.** The first invocation waits until the whole slice is imaged.
  Dispatching each position as soon as its frame has landed in every die would overlap imaging
  with compute; [streaming dispatch](streaming-dispatch.md) sketches that, and
  [ADR 0004](adr/0004-whole-slice-dispatch.md) says why this design waits.
- **Compute nodes beside the memory box.** Every compute node runs on the memory-box machine,
  since it reads frames from that machine's shared memory.

## Storage

There is no database. A slice's state lives in two places, both gone once the slice ends:

- **Memory box**: one POSIX shared-memory segment holding a fixed number of picture-sized slots. A
  slot is a byte offset into the segment. The [membox README](../src/membox/README.md) covers how
  slots are allocated, read and released.
- **Frame index**: in the controller's memory. It holds slot numbers only, never pixels.

## Running on one host

This implementation puts both machines on one host. `scripts/start.sh` launches each binary on its
own localhost port, and every process maps the same POSIX shared-memory segment.

Five binaries, each a separate OS process. Names, roles and step numbers match the diagram at the
top of this page:

| binary           | role (diagram)          | serves (gRPC)                     | calls |
|------------------|--------------------------|-------------------------------------|-------|
| `tool-driver`    | Tool driver              | —                                    | ① `ProcessSlice` |
| `controller`     | Controller + frame index | `ProcessSlice` (Ⅰ), `RegisterNode` (Ⅶ) | ② `ImageSlice`, ⑥ `ProcessPosition`, ⑦ `ReleaseSlots` |
| `camera`         | Camera                   | `ImageSlice` (Ⅱ, streams Ⅲ back)     | ③ `write_frame` (libmembox) |
| `compute-node`   | Compute node (×N)        | `ProcessPosition` (Ⅳ)                | Ⅴ `read_frame` (libmembox), Ⅶ `RegisterNode` |
| `membox-manager` | memory box bookkeeping   | `AllocSlot`, `ReleaseSlots` (Ⅵ)      | — |

Each process stays near the gRPC runtime's floor of a few tens of MB, and neither the controller
nor a compute node grows with frame size; [memory footprint](development.md#memory-footprint)
has the measurements.

## Codebase structure

| Directory | What it holds |
|---|---|
| `src/proto/` | `inspection.proto`, the gRPC contract between all processes |
| `src/controller/` | the `controller` binary: frame index, node pool, dispatch |
| `src/camera/` | the `camera` binary and the fixture generator |
| `src/membox/` | `libmembox`, linked by camera, controller and compute node (the controller uses only its `MemboxControl`), and the `membox-manager` binary |
| `src/compute_node/` | the `compute-node` binary; `algorithm/` holds the scoring algorithm |
| `src/driver/` | the `tool-driver` binary, which plays the inspection tool |
| `src/common/` | command-line flag parsing and PGM image reading and writing |
| `tests/` | unit and gRPC service tests, plus `e2e.sh` for the five-process run |
| `scripts/` | build, run, test and measurement entry points; see [development](development.md) |
| `docs/` | this map, the topic documents it links, and the ADRs under `adr/` |
