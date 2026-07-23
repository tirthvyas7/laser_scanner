# Line-Scanner Defect Detection Pipeline

A plugin-based, multi-stage **C++17** pipeline that detects surface defects in a line-scan
camera stream. Five blocks, each on its own thread, connected by lock-free single-producer /
single-consumer ring buffers, running under hard cycle-time (**T**), per-pixel (**<100 ns**),
and memory (**m**) budgets. No middleware — only the standard library and a single vendored
YAML parser.

```
DataGeneration → FilterThreshold → Labelling → Tracing → OutputLogger
   raw pixels      9-tap Gaussian    8-conn      size +      defect
   @ rate T        + threshold       labelling   bounding    records → CSV
                                                 box
```

---

## Documentation

The design is split across two documents:

- **[DESIGN_OVERVIEW.md](DESIGN_OVERVIEW.md)** — *Part 1 (stages 1–2).* Architecture and design
  patterns, the lock-free SPSC ring buffer, the seqlock metrics path, the four-loop orchestrator
  lifecycle, and the **T / <100 ns / m** constraint definitions.
- **[DESIGN_OVERVIEW_PART2.md](DESIGN_OVERVIEW_PART2.md)** — *Part 2 (stages 3–5).* The Labelling,
  Tracing & Computation, and Output Logger blocks; the generalized `channels/` layer; the
  `m + 2·(m/2)` memory budget; the elements-in-flight channel metric; the `skip_logging` opt-out;
  and the golden end-to-end validation.

Start with Part 1 for the core mechanics, then Part 2 for the connected-component and defect
stages built on top.

---

## Prerequisites

| Platform | Install |
|----------|---------|
| Ubuntu 22.04 | `sudo apt install cmake g++ git` |
| Windows 11 | Visual Studio 2022 or VS 2022 Build Tools with the **Desktop development with C++** workload. CMake is handled for you by `build.bat` — see below. |

yaml-cpp is downloaded and built automatically at configure time — no manual install needed.

---

## Build

**Ubuntu 22.04**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Windows 11** — from the project root:
```bat
.\build.bat
```
`build.bat` is a self-contained bootstrapper that:
1. Locates MSVC via `vswhere`, and **if the C++ toolset is missing**, installs the VS Build Tools
   *Desktop development with C++* workload (one time, UAC-elevated), then imports its environment.
2. Downloads the pinned **CMake 3.22.1** into `tools\` — once, then cached.
3. Configures and builds Release with MSVC (`cl.exe`) via the Visual Studio generator.

It is **incremental**: it never deletes `build\`, so re-running it recompiles only *your* changed
files — yaml-cpp and the CMake download are not rebuilt. Delete the `build\` folder to force a
clean rebuild.

---

## Run

```bash
./build/line_scanner            # Ubuntu 22.04
build\Release\line_scanner.exe  # Windows 11
```

The executable optionally takes a config path (defaults to `pipeline.yaml`):
```bash
build\Release\line_scanner.exe data\tests\single_defect.yaml
```

Edit `pipeline.yaml` to set the mode, input file, output path, and constraints.

### Configuration (`pipeline.yaml`)

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `columns` | 16 | Image width `m` in pixels (also the memory budget) |
| `threshold` | 128 | TV — binary threshold after convolution |
| `cycle_time_ns` | 1000 | T — target cycle time in nanoseconds |
| `mode` | `rng` | `csv` (reads file, stops at EOF) or `rng` (runs until timer) |
| `csv_path` | — | Input CSV path (used when `mode: csv`) |
| `output_path` | — | Terminal block's output file (defaults to `<block name>.csv`) |
| `run_duration_ms` | 20000 | Run time in ms (used when `mode: rng`) |

Each entry under `blocks:` may also carry **`skip_logging: true`** to exclude that block from the
metrics log and final report (used for the terminal `OutputLogger`, which has nothing to time).

### Output files

| File | Contents |
|------|----------|
| `metrics_report.txt` | Final pass/fail summary (per metric-bearing block) |
| `metrics_log.csv` | Per-block metrics sampled every 50 ms |
| `defects.csv` | Finished defect records (`label,size,top,left,bottom,right`) — name = `output_path` |

(`filter_output.csv` is written only when `FilterThreshold` is the terminal block, i.e. no stages
follow it.)

---

## Validation

The pipeline ships with committed **golden** fixtures so anyone can verify correctness
end-to-end. Each config in `data/tests/*.yaml` runs a known input through all five blocks; the
expected output is committed under `data/tests/expected/` and is **hand-derived** (by math, not
snapshotted). Run one and diff against its golden:

```bash
./build/line_scanner data/tests/single_defect.yaml            # writes _val_actual.csv
diff <(tr -d '\r' < _val_actual.csv) data/tests/expected/single_defect.csv   # no output = match
```

Repeat for `all_zeros.yaml` and `all_max.yaml`. See
**[DESIGN_OVERVIEW_PART2.md](DESIGN_OVERVIEW_PART2.md) §9** for the derivations and full details.

---

## Project layout

```
laser_scanner/
├── build.bat                 # Windows one-command build
├── pipeline.yaml             # runtime configuration
├── CMakeLists.txt
├── DESIGN_OVERVIEW.md        # Part 1 (stages 1–2)
├── DESIGN_OVERVIEW_PART2.md  # Part 2 (stages 3–5)
├── channels/                 # ChannelBase + lock-free RingBuffer<T>
├── include/                  # Block base, factory, metrics, config
├── src/                      # main, orchestrator, metrics logger
├── blocks/                   # one folder per pipeline stage
│   ├── data_generation/  filter_threshold/  labelling/
│   ├── tracing_computation/
│   └── output_logger/        # OutputLoggerBlock<T> template + concrete loggers
└── data/
    ├── sample.csv
    └── tests/                # edge-case inputs + expected/ goldens + validation configs
```
