# Component APIs

The calls behind each arrow of the [architecture diagram](architecture.md#diagram), one block
per roman numeral. [Reading the diagram](architecture.md#reading-the-diagram) explains how the
step numbers and block numerals connect.

Shared types. [Slice anatomy](problem.md#slice-anatomy) shows what slices, dies and frames are:

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
memory box wherever `write_frame` (Ⅵ) found room:

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
SlotOffset write_frame(std::span<const std::byte> pixels);

// Called by the controller once a slice's results are gathered, so its
// slots can hold the next slice's pictures.
void release_slots(std::span<const SlotOffset> slots);
```

### <sup>Ⅶ</sup> Compute node → controller (registration)

```cpp
// Announces this node to the controller, once at node startup: endpoint is the address the
// controller dials back for process_position (⑥) invocations. Registering the same endpoint
// again replaces its connection (a restarted node) rather than growing the pool. Returns once
// the controller has recorded the node, so a successfully started node is dispatchable.
// The controller drops a node whose ⑥ invocation fails or times out; registering again is
// also how such a node rejoins.
void register_node(const std::string& endpoint);
```

