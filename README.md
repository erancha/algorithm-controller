# Algorithm Controller

Design for the interview question: *"Our tool photographs a wafer slice by slice — [a slice is a
strip of dies, and each die is captured as several frames](#slice-anatomy). Every picture lands in
one slot of a shared memory box, and from then on a frame is named by its slot/offset, never
copied. Design the controller that, one slice at a time, splits the slice's work across a group of
compute nodes; each invocation runs an algorithm on all the frames that share one frame-in-die
position (i.e. the same frame position in every die) and returns a single numeric value; the
controller then gathers those values: one per frame-in-die position, so [dies of six
frames](#slice-diagram) yield six values for the slice."*

## Requirements

- The wafer is imaged **slice by slice**; a slice contains dies, and each die is imaged as
  multiple **frames**.
- Taking a picture writes it into the **memory box**: each slot holds exactly one picture, and a
  frame is referenced by its **slot/offset** from then on — pixels are never copied around.
- The imaging side reports back which picture belongs where: **die index + frame-in-die**, mapped
  to the slot/offset that holds it.
- The controller processes **all frames of one slice** as a unit before moving to the next slice.
- A set of **compute nodes** — separate machines — does the work. The unit of work is one
  invocation: **run an algorithm on all frames that share one frame-in-die position** (i.e. the
  same frame position in every die of the slice) and return a **single numeric value** for that
  position. Any node can serve any invocation. Every die is a copy of the same circuit, so one
  frame-in-die position is the same physical region in every die — the natural input set for
  comparing that region across dies.
- The controller collects one numeric value per frame-in-die position — that set of values is the
  output of processing a slice.

## Architecture

**Solution overview.** The system spans several machines: the controller is one process on its own
machine, each compute node is a separate machine, and between them sits the memory box — a
dedicated bank of picture-sized memory slots that every node can read in parallel over a fast
link, without going through a filesystem or the controller. The controller works one slice at a
time. It asks the camera to photograph the slice<sup>①</sup>; each picture is written into a free
slot of the memory box<sup>②</sup>, and the controller records which die and frame-in-die each
slot holds<sup>③</sup>. It then orders processing of all frames of the slice<sup>④</sup>: one
invocation per frame-in-die position, carrying the slot/offsets of that frame in every die, sent
to whichever node is free next — so a slow position ties up one machine, not the slice. The node
pulls those frames straight out of the memory box, runs the algorithm across them — every die is
a copy of the same circuit, so the input set is one physical region seen once per die — and
returns one number for the position<sup>⑤</sup>. The controller gathers a value per
position<sup>⑥</sup> and moves on to the next slice. Pixels cross the wire once — from the memory
box to the one node that processes them; everything else on the network is small messages.

```mermaid
%%{init: {"flowchart": {"wrappingWidth": 400}}}%%
flowchart TB
    subgraph ACQ[acquisition side]
        CAM[<b>Camera</b><br/>take picture per frame]
        MB[(memory box<br/>dedicated image memory,<br/>one picture per slot)]
        CAM -- ②&nbsp;picture&nbsp;into&nbsp;free&nbsp;slot --> MB
    end
    subgraph CTRLM[controller machine — one process]
        CTRL[<b>Controller</b><br/>drives one slice at a time]
        IDX[<b>frame index</b><br/>die index + frame-in-die → slot/offset]
        AGG[<b>result collector</b><br/>one numeric value per frame-in-die position]
    end
    subgraph NODES[compute nodes ×N — one machine each]
        CN[<b>Compute node</b><br/>per invocation: runs an algo on the frames<br/>at one frame-in-die position across all dies,<br/>returns a numeric value]
    end
    CTRL -- ① image one slice --> CAM
    CAM -- ③&nbsp;die&nbsp;index,&nbsp;frame#8209;in#8209;die&nbsp;+&nbsp;slot/offset --> IDX
    CTRL -- "④&nbsp;process(all&nbsp;frames&nbsp;of&nbsp;one&nbsp;slice):<br/>one&nbsp;invocation&nbsp;per&nbsp;frame#8209;in#8209;die&nbsp;position,<br/>to&nbsp;whichever&nbsp;node&nbsp;is&nbsp;free" --> CN
    MB -. reads&nbsp;that&nbsp;frame&nbsp;of&nbsp;every&nbsp;die<br/>at&nbsp;the&nbsp;given&nbsp;slot/offsets .-> CN
    CN -- ⑤&nbsp;numeric&nbsp;value&nbsp;per&nbsp;position,&nbsp;over&nbsp;network --> AGG
    AGG -- ⑥ results of the slice --> CTRL
```

## Slice anatomy

How a slice decomposes — the outer box is one slice, a strip of dies, and each die is imaged as a
grid of frames. The highlighted frames share one frame-in-die position (frame 1 of every die):
together they are the input set of a single invocation, which returns one numeric value for that
position.

### Slice diagram

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
    classDef pos fill:#f9d976,stroke:#b8860b,color:#000
    classDef caption fill:transparent,stroke:transparent
    class d0f1,d1f1,d2f1 pos
    class d0t,d1t,d2t caption
```
