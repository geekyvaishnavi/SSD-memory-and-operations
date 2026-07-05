
**Design and Simulation of a Flash Translation Layer for SSD Storage Systems**

A simulator of an SSD's Flash Translation Layer (FTL) — modeling
address mapping, garbage collection, and wear leveling — built as a final
year engineering project.

## Motivation

NAND flash cannot be overwritten in place. Every write goes to a fresh
physical page, old data is invalidated rather than erased immediately, and
space is reclaimed later through garbage collection, which erases whole
blocks at a time. This reclamation process causes extra physical writes
beyond what the host requested — write amplification — and repeated erasing
wears out flash blocks over time. This project simulates that full
lifecycle to study how different garbage collection and wear-leveling
policies affect SSD performance and longevity.

## Objectives

- Simulate core SSD/FTL behavior: logical-to-physical address mapping,
  out-of-place writes, and page invalidation
- Implement and compare multiple garbage collection victim-selection
  policies (greedy vs cost-benefit)
- Implement and evaluate wear-leveling strategies (dynamic and static)
- Quantify write amplification under different workload patterns
  (sequential, random, hotspot)
- Visualize NAND state transitions and FTL operations

## Architecture

```
Host Write/Read
      |
      v
 +----------+       +------------------+
 |   FTL    | <-->  | Mapping Table    |
 | (L2P)    |       | (logical->phys)  |
 +----------+       +------------------+
      |
      v
 +----------+       +------------------+
 |   GC     | <-->  | Wear Leveling    |
 +----------+       +------------------+
      |
      v
 +----------------------------+
 |   NAND (Blocks x Pages)     |
 +----------------------------+
```

## Modules

1. **NAND model** — blocks composed of pages; each page is free, valid, or
   invalid; each block tracks its erase count
2. **FTL mapping layer** — logical-to-physical page mapping table, read/write
   entrypoints
3. **Garbage collection engine** — two selectable policies:
   - *Greedy*: reclaims the block with the most invalid pages
   - *Cost-benefit*: weighs invalid-page count against block erase age
4. **Wear-leveling engine** — dynamic (biases free-page allocation toward
   less-worn blocks) and static (migrates cold data out of low-erase-count
   blocks)
5. **Workload generator** — synthetic write patterns: sequential, random,
   and hotspot (skewed access)
6. **Metrics** — write amplification factor (WAF), erase count distribution,
   GC frequency
7. **Visualization** — dashboard/scrubber showing NAND block/page state over
   time and GC/wear-leveling events

## Features

- Page-level address mapping (logical -> physical)
- Out-of-place writes with page invalidation
- Configurable GC policy (greedy / cost-benefit)
- Dynamic and static wear leveling
- Write amplification factor (WAF) and erase-count distribution metrics
- Synthetic workload generator (random, sequential, hotspot) + trace file support
- Visualization of block/page states and operations over time

## Getting Started

```bash
git clone https://github.com/<your-username>/ftlsim.git
cd ftlsim
go build -o ftlsim ./cmd/ftlsim
./ftlsim --workload random --pages 10000 --blocks 128 --gc-policy greedy
```

## Sample Output

```
Total writes: 10000
Total erases: 42
Write Amplification Factor: 1.34
Max erase count: 12 | Min erase count: 9 (wear leveled)
```

## Evaluation

Experiments are run across sequential, random, and hotspot workloads for
each GC policy, comparing:

- Write amplification factor (WAF)
- Erase count distribution across blocks (wear-leveling effectiveness)
- GC trigger frequency

Full results and analysis are documented in the project report (see
`/docs`).

## Project Structure

```
ftlsim/
├── cmd/
│   └── ftlsim/
│       └── main.go
├── internal/
│   ├── nand/
│   │   ├── page.go
│   │   ├── block.go
│   │   └── nand.go
│   ├── ftl/
│   │   ├── mapping.go
│   │   ├── ftl.go
│   │   ├── gc.go
│   │   └── wearlevel.go
│   ├── metrics/
│   │   └── metrics.go
│   └── workload/
│       ├── generator.go
│       └── trace.go
├── pkg/
│   └── api/
│       └── types.go
├── web/                    # visualization dashboard
├── docs/                   # project report, diagrams
├── testdata/
│   └── workloads/
├── go.mod
├── go.sum
├── README.md
├── LICENSE
└── .gitignore
```

## Design Decisions

- **Victim selection:** supports both greedy (most invalid pages) and
  cost-benefit (invalid-page count weighted against erase age) policies for
  direct comparison.
- **Wear leveling:** dynamic leveling biases allocation toward low
  erase-count blocks; static leveling periodically migrates cold data to
  ensure all blocks accumulate wear evenly over time.
- **Mapping granularity:** page-level, not hybrid/block-level, for
  simplicity and clearer invariants.

## Limitations

- Single-threaded, no concurrency modeling
- Synthetic/trace-based workloads only, no real block-device interface
- No power-loss/crash-consistency simulation

## Related Work

For comparison, see [MQSim](https://github.com/CMU-SAFARI/MQSim) and
[FEMU](https://github.com/vtess/FEMU), research-grade SSD
simulators/emulators with far greater scope (multi-queue modeling,
full-system NVMe emulation). ftlsim is a lightweight educational simulator
focused on FTL mapping, GC policy comparison, and wear-leveling mechanics.


## License

MIT
