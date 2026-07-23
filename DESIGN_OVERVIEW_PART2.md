# Design Overview Document — Part 2
## Line-Scanner Defect Detection Pipeline
### Stages 3–5: Labelling · Tracing & Computation · Output Logger

---

This document extends **Design Overview (Part 1)**, which describes the two-stage
`DataGeneration → FilterThreshold` pipeline, the lock-free ring buffer, the seqlock metrics
path, and the four qualification constraints (**T**, **<100 ns/pixel**, **m**). Everything in
Part 1 still holds unchanged. Part 2 documents the three stages added on top, plus the small
amount of shared infrastructure they required. Section numbers here are local to Part 2;
cross-references to Part 1 are called out explicitly.

---

## 1. What Part 2 Adds

The pipeline grows from two blocks to **five**, connected by **four** forward channels. No
backward channel is added — label lifetimes are discovered inside Labelling and pushed
forward as events.

```
DataGeneration ─PixelPair→ FilterThreshold ─PixelPair→ Labelling ─LabelledElement→ Tracing ─DefectRecord→ OutputLogger
   (Part 1)                    (Part 1)                (stage 3)                 (stage 4)              (stage 5)
   rate-ctrl @ T             9-tap Gaussian          8-conn CCL,              per-label size +        generic terminal
                             + threshold             union-find               bounding box            sink → CSV
```

| Stage | Block | Input element | Output element | Role |
|-------|-------|---------------|----------------|------|
| 3 | `Labelling` | `PixelPair` | `LabelledElement` | streaming connected-component labelling |
| 4 | `Tracing` | `LabelledElement` | `DefectRecord` | running size + smallest enclosing rectangle |
| 5 | `OutputLogger` | `DefectRecord` | — (terminal) | generic record → CSV sink |

The new blocks preserve every discipline from Part 1: one `std::thread` per block, a plain
non-virtual `read()`/`write()` hot path, seqlock metrics published every 100 cycles, and
drain-on-close termination.

---

## 2. Generalized Channels (extends Part 1 §2)

Part 1's channel was hard-typed to `RingBuffer<PixelPair>`. Two of the new edges carry wider
element types, so the channel had to become **type-generic without adding any hot-path cost**.

### `channels/` — the channel gets its own home

`ring_buffer.hpp` moved from `include/` to a new top-level **`channels/`** folder (sibling of
`blocks/`). The class is otherwise **byte-for-byte unchanged** — it already *is* the generic
channel; it is simply instantiated with more element types now.

### `ChannelBase` — a non-templated handle, virtual only off the hot path

`RingBuffer<T>` now inherits a small non-templated interface so the orchestrator can hold and
manage a channel of any element type through one pointer type:

```cpp
struct ChannelBase {                       // channels/channel_base.hpp
    virtual ~ChannelBase() = default;
    virtual void   close()        noexcept = 0;
    virtual bool   isClosed()     const noexcept = 0;
    virtual bool   isEmpty()      const noexcept = 0;
    virtual size_t occupancy()    const noexcept = 0;   // elements in flight
    virtual size_t capacity()     const noexcept = 0;   // elements
    virtual size_t memoryBytes()  const noexcept = 0;   // capacity × sizeof(T)
};
```

Only the **lifecycle / metrics** methods are virtual — and none of them are called per pixel.
`read()` and `write()` stay **non-virtual** on `RingBuffer<T>` itself, so the two-pixels-per-cycle
fast path has **zero** vtable lookups, exactly as in Part 1. The `alignas(64)` isolation of
`w_`/`r_`/`closed_` is untouched.

### `ChannelType` + `outputType()` + `makeChannel()` — the block names its type, the orchestrator builds it

A block does not create its own channel; it only *declares the element type it emits*, and the
orchestrator constructs the concrete `RingBuffer<T>`:

```cpp
enum class ChannelType { None, PixelPair, LabelledElement, DefectRecord };

// Each block: a one-line getter.
ChannelType outputType() const override { return ChannelType::LabelledElement; }  // e.g. Labelling

// Orchestrator: the single place that maps type → concrete ring.
std::shared_ptr<ChannelBase> makeChannel(ChannelType t, size_t cap) {
    switch (t) {
        case ChannelType::PixelPair:       return std::make_shared<RingBuffer<PixelPair>>(cap);
        case ChannelType::LabelledElement: return std::make_shared<RingBuffer<LabelledElement>>(cap);
        case ChannelType::DefectRecord:    return std::make_shared<RingBuffer<DefectRecord>>(cap);
        case ChannelType::None:            return nullptr;   // terminal block
    }
}
```

Wiring stays symmetric and type-erased; each block down-casts **once**, at wiring time, to the
`RingBuffer<T>` it expects and stores that typed handle for its hot path:

```cpp
auto ch = makeChannel(blocks_[i]->outputType(), cap);
blocks_[i]  ->setOutputChannel(ch);   // shared_ptr<ChannelBase>
blocks_[i+1]->setInputChannel(ch);    // dynamic_pointer_cast<RingBuffer<T>> inside
```

*Trade-off (accepted):* the down-cast is the only runtime type check, and it happens once per
edge at startup — never per element. Because every edge's `T` is fixed in the orchestrator's own
`makeChannel`, a mismatch can only come from a hand-edited yaml that reorders heterogeneous
stages, not from normal use.

### Element types on the wire

| Channel | Element | `sizeof` | Note |
|---------|---------|----------|------|
| DataGen→Filter, Filter→Labelling | `PixelPair` | 2 B | two `uint8` |
| Labelling→Tracing | `LabelledElement` | 22 B | labels + bounded merge/retire events (§4) |
| Tracing→OutputLogger | `DefectRecord` | 16 B | finished defect + CSV trait (§6) |

All three are trivially-copyable PODs — a hard requirement of the ring buffer (it byte-copies
under acquire/release; no pointers, no vtables ride the wire).

---

## 3. Labelling Block (stage 3)

`blocks/labelling/labelling_block.{hpp,cpp}` — a **single-pass, streaming 8-connectivity
connected-component labelling** over the binary pair-stream, under a tight memory rule.

### The memory — one `m` array + exactly two `m/2` arrays

| Structure | Size | Type | Duty |
|-----------|------|------|------|
| `row_[m]` | `m` | `uint16` | the **one** history row, used in place: reads row `r-1`, writes row `r` |
| `parent_[m/2]` | `m/2` | `uint16` | union-find forest **and** free-list (one array, two disjoint overlays) |
| `count_[m/2]` | `m/2` | `uint16` | direct reference count — number of `row_` cells currently holding this id |

Plus O(1) scalars only (`carry_`, `cur_left_`, `free_head_`, `next_id_`, `row_idx_`, `col_idx_`).
`idcap_ = m/2`; usable label ids are `1 .. m/2-1` (id `0` = background, never allocated). This is
exactly the assignment's allowance: **one `m`-wide history row plus two 1-D arrays of `m/2`** — no
third array. It is declared once at `configure()`:

```
[Labelling] memory budget: row[m=16] + parent[m/2=8] + count[m/2=8] uint16 = 64 B
```

### When 2 pixels arrive — one cycle processes a pair

Because the camera delivers two pixels per cycle, Labelling processes the pair as a **batch**:
read *both* pixels' neighbours first, then commit both cells at the end. This needs the `carry`
(the prev-row cell just behind the scan) only **once per cycle** — the right pixel's upper-left
neighbour is the left pixel's own cell, which has not been overwritten yet.

```
when 2 pixels (col c, col c+1) of row r arrive:
  # neighbours are resolved to their current union-find ROOT before use
  N_L  = relabelRoot(row_[c])      # prev-row @ c   → root  (count moved onto root)
  NE_L = relabelRoot(row_[c+1])    # prev-row @ c+1 → root
  Lc   = classify(NW=carry_, N=N_L, NE=NE_L, W=cur_left_)      # left pixel  (0 if background)
  NE_R = relabelRoot(row_[c+2])    # prev-row @ c+2 → root  (0 past the right edge)
  Lc1  = classify(NW=N_L, N=NE_L, NE=NE_R, W=Lc)              # right pixel (0 if background)

  # commit: increments FIRST, then the two lagged releases, then roll carry
  count_[Lc]++;  count_[Lc1]++
  release(carry_)                  # prev-row @ c-1 now out of reach
  release(N_L)                     # prev-row @ c   now out of reach
  carry_ = NE_L                    # prev-row @ c+1 becomes next cycle's NW
  row_[c] = Lc;  row_[c+1] = Lc1;  cur_left_ = Lc1
```

At any instant `row_` holds **row r-1 in columns ≥ c** and **row r in columns < c**; the single
past value the commit would otherwise destroy too early rides in `carry_`. One physical `m`-row
plays both "previous row" and "current row". Every off-image neighbour reads as background `0`, so
borders need no special branches (top row = all-`0` init; left/right columns = `0` reset / `0` past
the edge).

### Union-find with relabel-on-read + row-end reclaim

Neighbour ids read from `row_` may be **stale** (their blob merged since they were written), so
every foreground neighbour is `find()`-resolved to its current root, and the cell is **rewritten in
place to that root** (relabel-on-read). This flattens the forest *and* moves that cell's count off
the stale id onto the survivor — which is what lets an absorbed id's count eventually reach 0.

- **`find`** — climb `parent_` to the root, path-compressed.
- **`unite`** — keep the **lower** id as root, flip one pointer, emit a `MergeEvent{from:higher, to:lower}`. Counts are **not** folded here (relabel-on-read drains them lazily).
- **`classify`** — Rosenfeld decision tree, **≤ 1 union per pixel** (because `W ≡ NW` always).
- **`relabelRoot`** — resolve a read cell to its root, transfer its count.
- **`release`** — a prev-row cell passes out of reach: decrement; if a **root** hits 0 → the blob is finished → emit a retire; recycle the slot at row end.
- **`reclaimRowEnd`** — every cell has been read+compressed exactly once this row, so any id with `count == 0` is provably unreferenced. Rebuild the free-list from those ids; measure the live-id high-water.

An id's full life: `born → root → (maybe merge victim) → count hits 0 → freed → reused`. An
absorbed id is freed **when its last reference is relabelled away** (1–2 rows after the merge), never
at the merge instant — freeing it earlier would strand current-row cells that still point through it.

### Bounded by width, not length

At most `m/2` runs of 1s exist in a row, so at most `m/2` roots are alive at once; a few
merge-absorbed ids linger 1–2 rows. Recycling every dead id (both retired roots and merge victims)
through the free-list caps the live set at `≈ m/2` **regardless of roll length** — 10 rows or 10⁹
rows run in the same fixed arrays.

### The `m/2` cap is a hard wall — and the filter guarantees it never hits

`newLabel()` **throws** the moment a fresh id would exceed `idcap_ = m/2`:

```cpp
if (next_id_ >= idcap_) throw std::runtime_error("Labelling: id pool exhausted (m/2 arrays)");
```

No graceful degradation, no oversizing. The strict `m/2` is safe because the pipeline's **upstream
FilterThreshold guarantees the dangerous input can never arrive**: it is a symmetric Gaussian
low-pass that annihilates the Nyquist (`1010…`) frequency, so a per-pixel-alternating pattern — the
only thing that would need `> m/2` labels in a 16-wide row — cannot reach Labelling's input. The
filter structurally caps blob density; the strict bound leans on that guarantee rather than padding
against an input the pipeline already prevents. A run completing without throwing is itself proof
the bound held.

---

## 4. The Labelling → Tracing element

Channel type `RingBuffer<LabelledElement>`. `MergeEvent` and `LabelledElement` are **owned by
Labelling** (declared in its header as its output contract); Tracing `#include`s them.

```cpp
struct MergeEvent { uint16_t from, to; };   // absorbed id folds into surviving id (to < from)

struct LabelledElement {                     // 22 B, trivially copyable; value-init each cycle
    uint16_t   a, b;        // labels of the two pixels this cycle (0 = background)
    uint16_t   row, col;    // coords of a; b is at col+1
    MergeEvent merges[2];   // (from,to); empty slot = {0,0}  (≤ 2 merges/cycle)
    uint16_t   retires[3];  // finished root ids; empty slot = 0 (≤ 3 at a row-end cycle)
};
```

`0` is never a valid label id, so it doubles as the **empty sentinel** — no `n_merge`/`n_retire`
counts ride the wire. Coordinates ride *in* the element (the assignment's "info limited to the
current two elements"), so Tracing never tracks `m` or counts elements. Everything — labels, merges,
retires — travels in this one element; there is no side channel and no backward channel.

---

## 5. Tracing & Computation Block (stage 4)

`blocks/tracing_computation/tracing_block.{hpp,cpp}` — maintains a running **size** and **smallest
enclosing rectangle** per label; on label death, emits a `DefectRecord` and frees the slot.

### Per-label store — `m/2` slots, matching Labelling's id range

```cpp
struct LabelStats { uint32_t size; uint16_t top, bottom, left, right; bool active; };  // 16 B
std::vector<LabelStats> table_;   // sized labelArrayCap(m) = m/2, indexed by label id
```

`table_` uses the **same** `labelArrayCap(m) = m/2` helper as Labelling, so `table_[id]` is always
in range for any id Labelling can emit. Declared once at `configure()`:

```
[Tracing] memory budget: table[m/2=8] x 16B = 128 B
```

This is Tracing's **own** working set — still `m/2` *slots* (bounded by the same id range), but each
slot is a richer `LabelStats` because Tracing computes geometry. It is reported separately from
Labelling's two `uint16` arrays; the number of label slots is `m/2` on both sides, only the payload
differs. The bounding box is four running extremes: `top`=min row, `bottom`=max row, `left`=min col,
`right`=max col → corners `(top,left)`–`(bottom,right)`.

### Processing one element — merges, then pixels, then retires

Order matters, and FIFO on the channel makes it correct (an absorbed label's pixels were counted in
earlier cycles; the merge folds them into the survivor; every later pixel already carries the
resolved root):

```
1) merges : for each merges[k] != {0,0}: fold table_[from] into table_[to]
            (size += ; top/left = min ; bottom/right = max), then from.active = false
2) pixels : if a: accumulate(a, row, col)      # first pixel of a recycled id re-seeds the box
            if b: accumulate(b, row, col+1)
3) retires: for each retires[k] != 0: emit a DefectRecord from table_[id], then id.active = false
```

### Emitting the box — one struct copy, no geometry work

On retire the box is **already computed** (folded incrementally as pixels arrived), so output is just
a copy into a `DefectRecord`, a forward write (or a buffer append in terminal mode), and freeing the
slot. `active = false` mirrors Labelling's recycling: when Labelling later hands the same id to a new
blob, the next pixel hits the `!active` branch and re-seeds the box from scratch — FIFO guarantees
emit-then-clear happens before any pixel of the reused id arrives, so no leakage between blobs.

**End-to-end path of one coordinate:** Labelling's `row_/col_` counter → `LabelledElement{row,col}`
→ folded into `table_[id]`'s min/max extremes → copied into a `DefectRecord` on retire → written to
CSV by the Output Logger.

---

## 6. Output Logger Block (stage 5) — one generic sink for any record type

`blocks/output_logger/` — a terminal sink meant to sit after **any** block and dump that block's
output to a file, whatever the record type.

### Why compile-time, not runtime polymorphism

The obvious "log a `Loggable*` with a `virtual csvRow()`" fails: the ring buffer stores
**trivially-copyable POD** (bytes copied under the acquire/release protocol — no vtables, no pointers
on the wire). A virtual base cannot ride the channel. So genericity must be **compile-time**.

### The mechanism — a template + a 2-function trait

```cpp
// Any loggable record T supplies exactly these two — nothing else is type-specific:
//   static const char* csvHeader();
//   void csvRow(std::string& out) const;
template <class T>
class OutputLoggerBlock : public Block {          // input = RingBuffer<T>; terminal (no output)
    void run() override {
        std::string buf = T::csvHeader(); buf += '\n';
        T rec;
        while (!stop_) {
            if (in_->read(rec))                     buf += /* rec formats itself */, rec.csvRow(buf);
            else if (in_->isClosed() && in_->isEmpty()) break;
            else                                    LS_PAUSE();
        }
        /* write buf → output file once at exit (off the hot path) */
    }
};
```

Everything that is not `csvRow` — the read loop, buffering, exit-time flush — is identical for every
`T`. `DefectRecord` supplies the trait as member functions (declared in Tracing's header, its output
contract):

```cpp
static const char* csvHeader() { return "label,size,top,left,bottom,right"; }
void csvRow(std::string& out) const;   // e.g. "1,504,0,0,31,15\n"
```

Note `size` is the true pixel count and the four corners give the rectangle, so `size ≠ box area`
for a non-rectangular blob.

### One concrete logger = one thin registration

The factory default-constructs, so each concrete logger is a thin derived struct that fixes its name
(this is also what lets a future `PixelLogger` name itself):

```cpp
struct DefectLogger : OutputLoggerBlock<DefectRecord> {
    DefectLogger() : OutputLoggerBlock<DefectRecord>("OutputLogger") {}
};
LS_REGISTER_BLOCK(deflog, "OutputLogger", line_scanner::DefectLogger)
```

```
blocks/output_logger/
  output_logger_block.hpp          # the OutputLoggerBlock<T> template — shared by all loggers
  defect_logger/
    defect_logger.cpp              # registers OutputLoggerBlock<DefectRecord> as "OutputLogger"
```

A new logger type is a new subfolder with a one-line `.cpp`; the template header is never
duplicated. The output filename comes from `pipeline.yaml: output_path` (else `<block name>.csv`).

---

## 7. Memory Budget, Revisited (extends Part 1 §5)

Two kinds of memory are now reported, and they are judged differently.

### Block working set — declared once, from `m`

Labelling's `row_[m] + parent_[m/2] + count_[m/2]` (all `uint16`) and Tracing's `table_[m/2]` are
the **spec-authorized fixed per-block memory**. They are logged once at `configure()` (the two
`[Labelling]`/`[Tracing]` budget lines above) — a static statement that the allocation equals the
allowance, provable by reading `configure()`.

### Channel memory — measured in elements, real bytes shown

Part 1 converted channel occupancy to bytes with a hardcoded `× 2` (a `PixelPair` is 2 bytes). With
wide elements that is wrong, so each block now publishes the **real** element size of the channel it
samples (`memoryBytes()/capacity()`), and the report judges buffering in **elements-in-flight ≤ m**:

```
Peak channel buffering : 8 / cap 8 elements in-flight (176 B @ 22 B/elem)  [PASS <= m=16 elements]
```

Judging in elements is deliberate: 8 `LabelledElement`s ≈ 176 B would *falsely FAIL* a 16-**byte**
budget, yet it is only 8 items in flight — a fraction of a scanline. The honest bound for "holds less
than a row of state" is **item count**, which back-pressure keeps `≤ capacity (≈ m/2)` on every
channel regardless of element width or roll length. Real bytes are shown for information only.

---

## 8. Metrics Opt-Out — `skip_logging` (extends Part 1 §1, seqlock path)

A pure sink has nothing meaningful to time, so the Output Logger publishes **no** metrics. Rather than
special-casing it, any block can opt out via a per-block yaml flag:

```yaml
  blocks:
    - name: Tracing
    - name: OutputLogger
      skip_logging: true          # excluded from metrics_log.csv AND the final report
```

The flag rides on `BlockSpec`; the **orchestrator** (which maps specs → blocks in pipeline order)
simply omits flagged blocks from both the metrics poller and the final-report collection. The block
still spawns, runs, stops, and joins normally — `skip_logging` affects only what is *reported*, never
what *executes* (the logger still writes its CSV). The `MetricsLogger` needed zero changes; it just
receives a shorter block list.

---

## 9. Output Correctness — Golden Validation (extends Part 1 §7)

The full pipeline is validated end-to-end against **hand-derived** expected outputs (derived by
math, not snapshotted from the program), committed under `data/tests/expected/`. Each case runs in
CSV mode through all five blocks, so the filter's convolution + threshold + 8-pixel warmup and the
labelling / geometry are all exercised together. Because the pipeline is lossless and in-order, the
defect output is deterministic — the same input always yields the same records in the same order.

| Config (`data/tests/…`) | Input | Expected output | What it proves |
|---|---|---|---|
| `all_zeros.yaml` | uniform `0` | *(header only)* | filter rejects DC; no false blobs |
| `all_max.yaml` | uniform `255` | `1,504,0,0,31,15` | full-width run merge + partial-last-row flush + retire |
| `single_defect.yaml` | two `240`s at (row 16, col 8–9) on a `50` bg | `1,2,16,4,16,5` | low-pass discrimination + exact coordinate mapping |

**`all_max` derivation:** uniform 255 survives the filter (`255 × Σweights ≈ 255 ≥ 128`), so every
output pixel is 1. The 9-tap warmup consumes the first 8 px → `512 − 8 = 504` output pixels = 31 full
16-wide rows + a partial 8-px row, all 8-connected into **one** blob. Box `(0,0)–(31,15)`.

**`single_defect` derivation:** the `50` background is killed (`50 × Σw ≈ 50 < 128`). A defect only
survives where **both** `240`s land on the two highest adjacent kernel taps (`w₃+w₄ ≈ 0.585`):
`50 × Σw + 190 × 0.585 ≈ 161 ≥ 128`. That occurs at exactly two output positions → **one** blob,
size 2, box `(16,4)–(16,5)`. The reported cols `4,5` vs input cols `8,9` is the **−4
convolution-center offset**, not drift.

### How to validate

Build first (Part 1 §10), then run each committed config through the pipeline in CSV mode and
diff the emitted records against its golden. Using `single_defect` as the example:

```bash
./build/line_scanner           data/tests/single_defect.yaml   # Linux / macOS
build\Release\line_scanner.exe  data\tests\single_defect.yaml   # Windows
```

The Output Logger writes `_val_actual.csv` (path from the config's `output_path`). Its contents
equal the golden exactly; compare line-ending-insensitively, since the program emits CRLF:

```bash
diff <(tr -d '\r' < _val_actual.csv) data/tests/expected/single_defect.csv   # no output = match
```

Repeat for `all_zeros.yaml` and `all_max.yaml`. `_val_actual.csv` is a gitignored throwaway; each
config sets `OutputLogger: skip_logging: true`, so a validation run prints no metrics noise.

---

## 10. File Structure — additions to Part 1 §8

```
laser_scanner/
├── DESIGN_OVERVIEW.md               ← Part 1 (stages 1–2, infra, constraints)
├── DESIGN_OVERVIEW_PART2.md         ← this document (stages 3–5)
│
├── channels/                        ← NEW top-level folder (was include/ring_buffer.hpp)
│   ├── channel_base.hpp             ← ChannelBase interface + ChannelType enum
│   └── ring_buffer.hpp              ← lock-free SPSC RingBuffer<T> : ChannelBase (moved, unchanged)
│
├── data/tests/
│   ├── all_zeros.yaml               ← NEW: CSV-mode validation configs …
│   ├── all_max.yaml
│   ├── single_defect.yaml
│   └── expected/                    ← NEW: hand-derived golden outputs
│       ├── all_zeros.csv
│       ├── all_max.csv
│       └── single_defect.csv
│
└── blocks/
    ├── labelling/
    │   └── labelling_block.hpp/.cpp        ← stage 3 (declares MergeEvent, LabelledElement)
    ├── tracing_computation/
    │   └── tracing_block.hpp/.cpp          ← stage 4 (declares DefectRecord + CSV trait)
    └── output_logger/
        ├── output_logger_block.hpp         ← OutputLoggerBlock<T> template (shared)
        └── defect_logger/
            └── defect_logger.cpp           ← registers the DefectRecord sink
```

Edited (not new): `include/block.hpp` (type-erased `setInput/OutputChannel`, `outputType()`),
`include/pipeline_config.hpp` (`output_path`, `skip_logging`), `include/metrics.hpp`
(`channel_capacity`, `channel_elem_bytes`), `src/orchestrator.{hpp,cpp}` (`makeChannel`,
`skip_logging` filtering), `src/metrics_logger.cpp` (elements-in-flight memory line),
`CMakeLists.txt` (new sources + `channels/` and per-block include dirs), `pipeline.yaml`
(three new block entries + `output_path`).

---

*Part 1 remains the reference for the ring buffer internals, the seqlock protocol, the four-loop
orchestrator lifecycle, the rate-control / rate-following model, and the T / <100 ns / m constraint
definitions. Part 2 adds only what stages 3–5 introduced.*
