# CS6423-project
## DBIROptimizer — Compiler‑IR‑Driven Query Optimizer

> **Single‑author course project (Georgia Tech DBSE ’25) – Haoyu Gao**

DBIROptimizer is a lightweight research prototype that fuses classic database rewrites with modern compiler techniques.  
It parses SQL, applies logical/physical optimizations across a three‑tier IR, and finally JIT‑compiles LLVM IR for execution.  
The goal is to study how far a single developer can push IR‑centric query compilation and what performance it can yield over an interpreted baseline (SQLite).

---

### Key Ideas
| Layer | Role | Notable Passes |
|-------|------|----------------|
| **HIR** (logical) | relational algebra DAG | predicate push‑down · constant folding |
| **MIR** (physical) | concrete operators & data‑flow | heuristic join reorder · filter‑project fusion · CSE |
| **LIR** (codegen) | LLVM IR for each operator | register reuse · auto‑inlined expressions |

A shallow forward data‑flow analysis tracks expression liveness; a redundancy pass eliminates duplicate sub‑expressions.  
SQLite is reused *only* for table storage; all heavy logic executes in compiled native code.

---

### Repo Layout
```
DBIROptimizer/
  ├─ src/
  │   ├─ parser/          # hand‑written SQL parser → AST
  │   ├─ ir/              # HIR & MIR node defs, visitors
  │   ├─ opt/             # optimization passes
  │   ├─ codegen/         # LLVM IR templates & helpers
  │   └─ runtime/         # tuple loop, hash tables, etc.
  ├─ benchmark/
  │   ├─ synthetic/       # data generators & queries
  │   └─ tpch_subset/     # Q1 Q3 Q6 Q10 Q14 scripts
  ├─ tests/               # unit + regression tests
  ├─ CMakeLists.txt
  └─ README.md            # (this file)
```

---

### Build Instructions

```bash
# deps: LLVM ≥15, SQLite3, CMake ≥3.20
git clone https://github.com/yourname/DBIROptimizer.git
cd DBIROptimizer
mkdir build && cd build
cmake -DLLVM_DIR=/path/to/llvm/share/llvm/cmake ..
make -j$(nproc)
```

The build produces:

* `dbiopt_cli` – interactive shell (`.help` for commands)  
* `run_query`  – CLI runner: `run_query <sql_file>`  
* `run_bench`  – scripted benchmark harness

---

### Quick Start

```bash
# 1. initialise SQLite test db
scripts/init_synth.sh       # synthetic 10 M rows
scripts/init_tpch_sf1.sh    # TPC‑H tables SF=1

# 2. interactive shell
./build/dbiopt_cli
> .open data/synth.db
> EXPLAIN SELECT * FROM T1 WHERE val < 10;

# 3. compile & run a query
./build/run_query "SELECT COUNT(*) FROM T1 WHERE val<10;"
```

The CLI prints (i) chosen plan, (ii) ms spent in JIT, (iii) execution time, and (iv) result rows.

---

### Reproducing Paper/Report Numbers

```bash
# Synthetic workload in Table 1
./build/run_bench benchmark/synthetic/workload.yaml

# TPC‑H subset SF=5 in Table 2
./build/run_bench benchmark/tpch_subset/tpch_sf5.yaml
```

Each script outputs CSV with `sqlite`, `unopt`, `opt` columns.

---

### Current Limitations
* Join DP enumerator unfinished – only ≤4‑table heuristic.  
* Tuple‑at‑a‑time loops; no vectorized batches/SIMD yet.  
* Single‑threaded runtime – no intra‑query parallelism.  
* UDFs parsed but not translated to LLVM; executes via SQLite fallback.  
* Only basic SSA‑free data‑flow; deeper PRE/LICM left for future work.

See `doc/future_work.md` for a detailed roadmap.

---

### Benchmarks (Core Machine : i7‑8650U, 16 GB, Ubuntu 22.04)

| Query | SQLite | Unopt | **Opt** | Gain vs SQLite |
|-------|--------|-------|---------|----------------|
| Filter  | 430 ms | 360 ms | **310 ms** | ‑28 % |
| Join    | 1.50 s | 1.35 s | **1.10 s** | ‑27 % |
| TPC‑H Q1| 1.82 s | 1.55 s | **1.34 s** | ‑26 % |

_JIT latency averages 30–60 ms and is included in times above._
