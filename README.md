# DBIROptimizer - Compiler IR Based Database Query Optimization

> **Single-author course project (Georgia Tech DBSE '25) – Haoyu Gao**

DBIROptimizer is a lightweight research prototype that fuses classic database rewrites with modern compiler techniques. It parses SQL, applies logical/physical optimizations across a three-tier IR, and finally JIT-compiles LLVM IR for execution. The goal is to study how far a single developer can push IR-centric query compilation and what performance it can yield over an interpreted baseline (SQLite).

## Core Architecture

DBIROptimizer implements a three-tier IR approach to bridge high-level relational transformations with low-level code generation:

| Layer | Role | Notable Passes |
|-------|------|----------------|
| **HIR** (logical) | relational algebra DAG | predicate push-down · constant folding |
| **MIR** (physical) | concrete operators & data-flow | heuristic join reorder · filter-project fusion · CSE |
| **LIR** (codegen) | LLVM IR for each operator | register reuse · auto-inlined expressions |

A shallow forward data-flow analysis tracks expression liveness; a redundancy pass eliminates duplicate sub-expressions. SQLite is reused *only* for table storage; all heavy logic executes in compiled native code.

## Repository Structure

```
DBIROptimizer/
├─ ir.h                # IR definitions for HIR and MIR
├─ optimizer.h         # Optimization pass declarations
├─ optimizer.cpp       # Optimization implementations
├─ codegen.h           # LLVM code generation interface
├─ codegen.cpp         # LLVM code generation implementation
├─ dbir.cpp            # Main program with parser and SQLite integration
├─ dbir_interface.py   # Python interface for easier use
├─ CMakeLists.txt      # Build configuration
└─ README.md           # Project documentation
```

## Key Features

### High-Level IR (HIR)
- Represents the logical query plan as a directed acyclic graph (DAG)
- Node types: Select, Project, Join, Filter, Scan, GroupBy, Aggregate
- Optimization passes:
  - **Predicate pushdown**: Moves filter predicates closer to data sources
  - **Constant folding**: Evaluates constant expressions at compile time
  - **Dataflow analysis**: Identifies and eliminates unreachable expressions

### Mid-Level IR (MIR)
- Represents the physical execution plan
- Operator types: HashJoin, NestedLoopJoin, HashAggregate, Scan, Filter, Project, FilterProject
- Optimization passes:
  - **Join reordering**: Heuristic-based join order selection (up to 4 tables)
  - **Operator fusion**: Combines adjacent operators (e.g., FilterProject)
  - **Common Subexpression Elimination (CSE)**: Eliminates redundant expressions

### Low-Level IR (LIR) / Code Generation
- Uses LLVM for code generation and JIT compilation
- Generates optimized native code for each operator
- Implements efficient functions for hash joins, filters, and scans
- Register allocation and reuse for expression evaluation

## Expression Handling
The system supports various expression types:
- Column references
- Constants (integer, float, string, boolean)
- Binary operations (arithmetic, comparison, logical)
- Function calls (partially implemented)

## Development Environment Setup

### Prerequisites
- LLVM ≥15
- SQLite3
- CMake ≥3.20
- C++17 compatible compiler

### Setting Up the Environment with Vagrant (Recommended)
For consistent development, we recommend using the provided virtual machine environment:

1. Install [Vagrant](https://developer.hashicorp.com/vagrant/downloads) (version 2.3.4) and [VirtualBox](https://www.virtualbox.org/wiki/Downloads) (version 7.0.6)

2. Create and set up the virtual machine:
```bash
mkdir DBIROptimizer_VM
cd DBIROptimizer_VM
vagrant box add lechenyu/cs6241
vagrant init lechenyu/cs6241
```

3. For Windows users, you may need to add this to your Vagrantfile to avoid errors:
```
config.vm.provider "virtualbox" do |v|
  v.customize [ "modifyvm", :id, "--uartmode1", "disconnected" ]
end
```

4. Launch and connect to the VM:
```bash
vagrant up
vagrant ssh
```

5. Inside the VM, clone the repository:
```bash
git clone https://github.com/hgao327/CS6423-project.git
cd CS6423-project
```

### Using VS Code for Development
We recommend using VS Code with remote development capabilities:

1. Configure SSH for the virtual machine:
```bash
# For Linux/Mac
cd DBIROptimizer_VM
vagrant up
vagrant ssh-config >> ~/.ssh/config

# For Windows PowerShell
cd DBIROptimizer_VM
vagrant up
vagrant ssh-config | Out-File C:\Users\<USER_NAME>\.ssh\config -Append
```

2. Install VS Code extensions:
   - Remote-SSH
   - C/C++
   - CMake Tools

3. Connect to the VM using Remote-SSH and open the project folder

## Build Instructions

```bash
# Inside the repository directory
mkdir build
cmake -DCMAKE_INSTALL_PREFIX=./install -B build -S . -G Ninja
cd build && ninja install
```

This will build the project and install:
- The DBIROptimizer binary
- All necessary libraries
- Test scripts and benchmarks

## Usage

### Command-Line Interface
```bash
# Run a SQL query with DBIROptimizer
./install/bin/dbir /path/to/database.db "SELECT * FROM orders WHERE status = 'Shipped'"

# Advanced benchmark with multiple queries
./install/bin/dbir /path/to/database.db --benchmark
```

### Python Interface
For easier interaction, use the provided Python interface:

```bash
# Create a test database
python dbir_interface.py --db test.db --create-test-db

# Run a simple query
python dbir_interface.py --db test.db --query "SELECT * FROM orders WHERE status = 'Shipped'"

# Benchmark a query
python dbir_interface.py --db test.db --query "SELECT o.order_id, c.name FROM orders o JOIN customers c ON o.customer_id = c.id WHERE c.city = 'NY'" --benchmark --runs 5

# Run TPC-H benchmark queries
python dbir_interface.py --db tpch.db --tpch-queries benchmark/tpch_queries.json --benchmark
```

## Creating Test Data

### Synthetic Dataset
```bash
python dbir_interface.py --db synth.db --create-test-db
```

This creates a sample database with:
- `customers` table (100 records)
- `orders` table (1000 records)
- Relationship between customers and orders

### TPC-H Dataset
For TPC-H benchmarks, you'll need to set up the TPC-H schema and load data:

```bash
# Assuming you have the TPC-H tools installed
cd /path/to/tpch-tools
dbgen -s 1  # Scale factor 1
# Use the generated .tbl files to create your SQLite database
```

## Quick Start

```bash
# 1. Build the project
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=../install ..
ninja install
cd ..

# 2. Create and initialize a test database
python dbir_interface.py --db test.db --create-test-db

# 3. Run a simple query
./install/bin/dbir test.db "SELECT * FROM orders WHERE status = 'Shipped'"

# 4. Benchmark against SQLite
python dbir_interface.py --db test.db --query "SELECT o.order_id, c.name FROM orders o JOIN customers c ON o.customer_id = c.id WHERE c.city = 'NY'" --benchmark
```

## Reproducing Benchmark Results

To reproduce the benchmarks from the report:

```bash
# Synthetic benchmark
./install/bin/run_bench benchmark/synthetic/workload.yaml

# TPC-H benchmark (Scale Factor 5)
./install/bin/run_bench benchmark/tpch_subset/tpch_sf5.yaml
```

Each script outputs CSV with `sqlite`, `unopt`, and `opt` columns for comparison.

## Benchmarking Results

Benchmarks run on an Intel i7-8650U CPU with 16GB RAM, Ubuntu 22.04:

### Synthetic Dataset (10M rows)

| Query Type | SQLite (ms) | Unopt (ms) | Opt (ms) | Speedup |
|------------|-------------|------------|----------|---------|
| Filter-only | 430 | 360 | 310 | 1.39x |
| Equality Join | 1500 | 1350 | 1100 | 1.36x |
| Join + Filter | 1680 | 1410 | 1200 | 1.40x |
| Group+Filter | 1150 | 960 | 800 | 1.44x |

### TPC-H Subset (Scale Factor 5)

| TPC-H Query | SQLite (s) | Unopt (s) | Opt (s) | Speedup |
|-------------|------------|-----------|---------|---------|
| Q1 | 1.82 | 1.55 | 1.34 | 1.36x |
| Q3 | 2.71 | 2.30 | 2.02 | 1.34x |
| Q6 | 0.86 | 0.75 | 0.65 | 1.32x |
| Q10 | 3.10 | 2.64 | 2.30 | 1.35x |
| Q14 | 2.55 | 2.16 | 1.85 | 1.38x |

*JIT compilation typically adds 30-60 ms overhead per query, which is included in the times above.*

## Architecture Deep Dive

### Query Processing Pipeline

1. **SQL Parsing**: SQL query is parsed into an abstract syntax tree (AST)
2. **HIR Generation**: AST is transformed into HIR nodes representing logical operators
3. **HIR Optimization**: Optimizations like predicate pushdown and constant folding are applied
4. **MIR Generation**: HIR is converted to MIR nodes representing physical operators
5. **MIR Optimization**: Join reordering, operator fusion, and redundancy elimination
6. **Code Generation**: MIR is translated to LLVM IR
7. **JIT Compilation**: LLVM IR is compiled to native code
8. **Execution**: Compiled code is executed with SQLite as the storage layer

### Expression Evaluation

Expressions are evaluated through a recursive visitor pattern:

1. Constants are folded at compile time
2. Column references are translated to tuple attribute accesses
3. Binary operations are translated to corresponding LLVM instructions
4. Common subexpressions are detected and computed only once

### SQLite Integration

DBIROptimizer uses SQLite for:
- Table storage and basic schema information
- Row scanning operations (data is read from SQLite, then processed by compiled code)
- Query result storage

## Transferring Files Between VM and Host

Vagrant syncs the folder `/vagrant` on the virtual machine with the folder containing your Vagrantfile on the host machine. You can use these synced folders to transfer files between the VM and your host system.

## Current Limitations and Future Work

- **Join Optimization**: Currently limited to heuristic join reordering for up to 4 tables. A full dynamic programming approach is planned.
- **Execution Model**: Uses tuple-at-a-time execution. Future work includes vectorized execution and SIMD instructions.
- **Parallelism**: Single-threaded execution only. Parallelization is a key future enhancement.
- **Dataflow Analysis**: Basic forward dataflow analysis. More advanced SSA-based approaches planned.
- **User-Defined Functions**: Limited support. Full integration with LLVM-based code generation is planned.
- **Adaptive Execution**: Runtime statistics collection and re-optimization is planned.
- **Memory Management**: Basic memory management. More sophisticated buffer management is planned.