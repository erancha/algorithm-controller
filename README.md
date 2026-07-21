# Algorithm Controller

Design of a controller that, one [`process_slice`](#architecture) call at a time, distributes a
wafer slice's frames across a group of compute nodes and returns the gathered results (one
numeric value per frame-in-die position).

[Problem statement](#problem-statement) · [Terminology](#terminology) ·
[Derived requirements](#derived-requirements) · [Solution overview](#solution-overview) ·
[Architecture](#architecture) · [Component APIs](#component-apis) ·
[Appendix: slice anatomy](#appendix-slice-anatomy)

## Problem statement

*"Design the controller of a wafer-inspection tool that photographs a wafer slice by slice —
[a slice is a strip of dies, and each die is captured as several frames](#appendix-slice-anatomy).
Every picture lands in one slot (AKA offset) of a shared memory box, and from then on a frame is
named by its slot, never copied. The controller, one slice at a time, splits the slice's work
across a group of compute nodes; each invocation runs an algorithm on all the frames that share
one frame-in-die position (i.e. the same frame position in every die) and returns a single
numeric value; the controller then gathers those values: one per frame-in-die position, so
[dies of six frames](#one-slice-diagram) yield six values for the slice."*

## Terminology

- **die** — one copy of the chip's circuit; a wafer carries many identical dies.
- **slice** — a strip of dies.
- **frame** — one camera picture covering part of a die; a die is too large for one shot, so
  each die is imaged as a [grid of frames](#one-slice-diagram).
- **frame-in-die position** — a frame's place in that grid.
- **slot** — one picture-sized cell of the shared memory box, identified by its numeric offset
  into the box (hence AKA "offset").

## Derived requirements

- As each picture lands, the acquisition side reports its placement to the controller: which
  **die index + frame-in-die** it captures, and the slot that holds it.
- The **compute nodes are separate machines**, and any node can serve any invocation.
- Dies are copies of one circuit, so a frame-in-die position is the same physical region in
  every die — the natural input set for cross-die comparison.

## Solution overview

The controller is one process on its own machine; each compute node is a separate machine;
between them sits the memory box — a bank of picture-sized slots that every node reads in
parallel over a fast link, bypassing filesystem and controller. Each `process_slice`
call<sup>①</sup> hands the controller one slice and returns its results. The controller asks the
camera to photograph the slice<sup>②</sup>; each picture lands in a free slot<sup>③</sup>, and
the controller records which die and frame-in-die each slot holds<sup>④</sup> in its frame
index. Once the camera reports the slice fully imaged<sup>⑤</sup>, the controller dispatches one
invocation per frame-in-die position<sup>⑥</sup> — carrying that frame's slots, one per die — to
whichever node is free, so a long-running invocation ties up one machine, not the slice. The node
pulls those frames straight from the memory box, runs the algorithm across them, and returns one
number for the position as ⑥'s reply; the controller writes each reply into the results vector
at its position index. It returns that vector as ①'s reply, then releases the slice's
slots<sup>⑦</sup> for the next slice's pictures. Pixels cross the wire once — from the memory
box to the one node that processes them; everything else on the network is small messages.

Dispatch deliberately waits for the whole slice before the first invocation. A streaming
refinement — dispatching each position as soon as its frame has landed in every die — is
sketched in [streaming dispatch](docs/streaming-dispatch.md) and kept out of this design.

## Architecture

Arrow shape encodes call semantics: **thick** — synchronous request/reply; **thin** — one-way
asynchronous message; **dotted** — data flow, not a command.

```mermaid
%%{init: {"flowchart": {"wrappingWidth": 400}}}%%
flowchart TB
    DRV[<b>Tool driver</b><br/>whatever runs the tool,<br/>one slice at a time]
    subgraph CTRLM[controller machine — one process]
        CTRL[<b>Controller</b><br/>drives one slice at a time,<br/>collects one value per frame-in-die position]
        IDX[<b>frame index</b><br/>lookup table:<br/>die index + frame-in-die → slot]
    end
    subgraph ACQ[acquisition side]
        CAM[<b>Camera</b><br/>take picture per frame]
        MB[(memory box<br/>dedicated image memory,<br/>one picture per slot)]
    end
    subgraph NODES[compute nodes ×N — one machine each]
        CN[<b>Compute node</b><br/>per invocation: runs an algo on the frames<br/>at one frame-in-die position across all dies,<br/>returns a numeric value]
    end
    DRV == "①<sup>Ⅰ</sup>&nbsp;slice&nbsp;results&nbsp;⇐&nbsp;<b>process_slice</b>(slice&nbsp;id)" ==> CTRL
    CTRL -- "②<sup>Ⅱ</sup>&nbsp;<b>image_slice</b>" --> CAM
    CAM == "③<sup>Ⅵ</sup>&nbsp;<b>write_picture</b>&nbsp;into&nbsp;a&nbsp;free&nbsp;slot" ==> MB
    CAM -- "④<sup>Ⅲ</sup>&nbsp;<b>on_frame_stored</b>:&nbsp;die&nbsp;index,&nbsp;frame#8209;in#8209;die&nbsp;+&nbsp;slot" --> IDX
    CAM -- "⑤<sup>Ⅲ</sup>&nbsp;<b>on_slice_imaged</b>&nbsp;once&nbsp;all&nbsp;landed" --> CTRL
    IDX -. "each&nbsp;position's&nbsp;slots,&nbsp;in&nbsp;die&nbsp;order<sup>Ⅳa</sup>" .-> CTRL
    CTRL == "⑥<sup>Ⅳ</sup>&nbsp;value&nbsp;⇐&nbsp;<b>process_position</b>:&nbsp;one&nbsp;invocation<br/>per&nbsp;frame#8209;in#8209;die&nbsp;position,<br/>to&nbsp;whichever&nbsp;node&nbsp;is&nbsp;free" ==> CN
    MB -. "<b>read_frame</b><sup>Ⅴ</sup>:&nbsp;one&nbsp;frame,&nbsp;by&nbsp;its&nbsp;slot" .-> CN
    CTRL -- "⑦<sup>Ⅵ</sup>&nbsp;<b>release_slots</b>" --> MB
```

## Component APIs

Two numberings tie the diagram to the code: circled numbers (①–⑦) are the chronological steps,
one per solid arrow; superscript roman numerals name the code blocks below. So ①<sup>Ⅰ</sup>
reads "step 1, defined in block Ⅰ" — and the arrow's **bold** function heads that block under
the same name. A leading `x ⇐` names the call's return value. Dotted arrows carry data, not
steps: the memory-box read is block Ⅴ, pulled by a node while serving ⑥, and the frame-index →
controller hop composes each ⑥ invocation's slot list — block Ⅳa,
[from lookup table to invocation](#ⅳa-from-lookup-table-to-invocation). Every cross-machine
call carries only identifiers and numbers; pixels move solely through memory-box reads.

Shared vocabulary — [slice anatomy](#appendix-slice-anatomy) visualizes slices, dies and frames:

```cpp
using SliceId    = std::uint64_t;
using DieIndex   = std::uint32_t;
using FrameInDie = std::uint32_t; // frame position within a die — the same region in every die
using SlotOffset = std::uint64_t; // names one picture-sized slot by its offset in the memory box
```

### <sup>Ⅰ</sup> Tool driver → controller (①)

```cpp
// Public entry point of the controller — one call runs one slice end to end: images it (②),
// dispatches one node invocation per frame-in-die position (⑥), and returns the gathered
// results — element i is the value for frame-in-die position i.
std::vector<double> process_slice(SliceId slice);
```

### <sup>Ⅱ</sup> Controller → camera (②)

```cpp
// Starts imaging one slice. Returns immediately; per-frame placements (④) and the
// completion signal (⑤) arrive as callbacks while pictures land in the memory box (③).
void image_slice(SliceId slice);
```

### <sup>Ⅲ</sup> Camera → controller (④, ⑤)

```cpp
// One call per stored picture — adds the frame-index entry (die, frame) → slot.
void on_frame_stored(SliceId slice, DieIndex die, FrameInDie frame, SlotOffset slot);

// Every frame of the slice is in the memory box; the controller may start dispatching (⑥).
void on_slice_imaged(SliceId slice);
```

### <sup>Ⅳ</sup> Controller → compute node (⑥)

```cpp
// One invocation, sent to whichever node is free: run the algorithm on the frames at one
// frame-in-die position — process_slice (Ⅰ) issues one such call per position. `slots` lists
// the position's slot number for each die, in die order, resolved via the frame index. The
// span views only this list of numbers — the slots it names may lie anywhere in the memory
// box. The reply is the single numeric value for that position; the controller keeps one
// invocation in flight on every free node and writes each reply into the results vector
// at its position index.
double process_position(SliceId slice, FrameInDie position,
                        std::span<const SlotOffset> slots);
```

#### <sup>Ⅳa</sup> From lookup table to invocation

Once every placement (④) is in, the frame index is a completed table of
(die index, frame-in-die) → slot. For position `p` the controller walks the
dies in order, appending each die's slot to a `std::vector<SlotOffset>`. Traced for position 1
of a three-die slice whose frame index holds (die 0, pos 1) → 9, (die 1, pos 1) → 5,
(die 2, pos 1) → 106:

```cpp
std::vector<SlotOffset> slots;                          // starts empty: {}
for (DieIndex die = 0; die < die_count; ++die)
    slots.push_back(frame_index.at({die, position}));   // appends that die's slot at the end
// die 0: at({0, 1}) returns   9 → slots is {9}
// die 1: at({1, 1}) returns   5 → slots is {9, 5}
// die 2: at({2, 1}) returns 106 → slots is {9, 5, 106}

process_position(slice, position, slots);               // sends {9, 5, 106} as a span
```

`std::span` can only view a contiguous sequence, and `std::vector` guarantees contiguous element
storage by the standard — that pairing is what makes the implicit conversion safe. The contiguity
applies to the list of slot numbers only; the slots those numbers name are scattered across the
memory box wherever `write_picture` (Ⅵ) found room:

```
controller's vector:   [ 9, 5, 106 ]          <- contiguous, the span views this
                         │  │   │
memory box:     slot 9 ──┘  │   └───── slot 106
                (die 0)   slot 5       (die 2)
                          (die 1)
```

The span itself never crosses the wire — it is a local pointer + length, meaningless on another
machine. The RPC layer serializes the vector's few `uint64_t` values into the request; the node's
stub deserializes them into its own contiguous buffer and presents a fresh span to the handler,
which then pulls the actual pixels slot by slot via `read_frame` (Ⅴ).

### <sup>Ⅴ</sup> Compute node → memory box (dotted read)

```cpp
// Maps the slot at the given offset for reading — a node calls it once per die while
// serving ⑥. The view aliases memory-box storage; pixels are not copied.
std::span<const std::byte> read_frame(SlotOffset slot);
```

### <sup>Ⅵ</sup> Camera → memory box (③), controller → memory box (⑦)

```cpp
// Stores one picture into a free slot and returns which slot was chosen —
// the camera then reports that slot to the controller (④).
SlotOffset write_picture(std::span<const std::byte> pixels);

// Called by the controller once a slice's results are gathered, so its
// slots can hold the next slice's pictures.
void release_slots(std::span<const SlotOffset> slots);
```

## Appendix: slice anatomy

An example slice: three dies, each imaged as six frames. The highlighted frames share one
frame-in-die position (frame 1 of every die) — the input set of a single invocation, which
returns one value for that position. Six positions, so this slice's
[`process_slice`](#ⅰ-tool-driver--controller-①) reply holds six values.

### One slice diagram

```mermaid
block-beta
    columns 1
    block:SLICE:1
        columns 3
        block:D0:1
            columns 3
            d0t["die 0"]:3
            d0f0["frame 0"] d0f1["frame 1"] d0f2["frame 2"]
            d0f3["frame 3"] d0f4["frame 4"] d0f5["frame 5"]
        end
        block:D1:1
            columns 3
            d1t["die 1"]:3
            d1f0["frame 0"] d1f1["frame 1"] d1f2["frame 2"]
            d1f3["frame 3"] d1f4["frame 4"] d1f5["frame 5"]
        end
        block:D2:1
            columns 3
            d2t["die 2"]:3
            d2f0["frame 0"] d2f1["frame 1"] d2f2["frame 2"]
            d2f3["frame 3"] d2f4["frame 4"] d2f5["frame 5"]
        end
    end
    classDef pos fill:transparent,stroke:#e8a33d,stroke-width:3px
    classDef caption fill:transparent,stroke:transparent
    classDef slice fill:transparent,stroke:transparent
    class d0f1,d1f1,d2f1 pos
    class d0t,d1t,d2t caption
    class SLICE slice
```
