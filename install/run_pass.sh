#!/bin/bash
set -e

function usage() {
  local name='run_pass_and_compile.sh'
  echo "Run all passes to analyze and transfrom the specified benchmark"
  echo "USAGE: ${name} [benchmark]"
  echo ""
  echo "Options:"
  echo "    valid micro-benchmark:  ${MICRO_BENCHS[*]}"
  echo "    valid large benchmark:  ${LARGE_BENCHS[*]}"
}

CURR=$(readlink -f "$0")
ROOT=$(dirname "$CURR")
PLUGIN="${ROOT}/libproj1.so"
PASS="mem2reg,access-det,check-ins,check-opt,valuemd-rem"
MICRO_BENCH_DIR="${ROOT}/benchmark/micro_benchmark"
MICRO_BENCHS=$(sed -e 's/\.bc/ /g' -e 's/[ \t]*$//g' <(find "${MICRO_BENCH_DIR}" -name "*.bc" -printf "%f" | sort | tr '\n' ' '))
LARGE_BENCH_DIR="${ROOT}/benchmark/large_benchmark"
LARGE_BENCHS=$(sed -e 's/\.bc/ /g' -e 's/[ \t]*$//g' <(find "${LARGE_BENCH_DIR}" -name "*.bc" -printf "%f" | sort | tr '\n' ' '))

if [ $# -lt 1 ]; then
  usage
  exit 1
fi

BENCH="${MICRO_BENCH_DIR}/${1}.bc"
if [ ! -e ${BENCH} ]; then
  BENCH="${LARGE_BENCH_DIR}/${1}.bc"
  if [ ! -e ${BENCH} ]; then
    echo "Invalid benchmark: ${1}"
    usage
    exit 1
  fi
fi

BENCH_TRANS="${1}-transformed.bc"
BENCH_TRANS_LL="${1}-transformed.ll"
BENCH_LL="${1}-original.ll"
echo "==============================Transform LLVM IR========================================"
echo "opt -load-pass-plugin \"${PLUGIN}\" -passes=${PASS} \"${BENCH}\" -o \"${BENCH_TRANS}\""
opt -load-pass-plugin "${PLUGIN}" -passes=${PASS} "${BENCH}" -o "${BENCH_TRANS}"
echo "=========================Generate Human-Readable Format================================"
echo "llvm-dis ${BENCH_TRANS} -o ${BENCH_TRANS_LL}"
llvm-dis ${BENCH_TRANS} -o ${BENCH_TRANS_LL}
echo "llvm-dis \"${BENCH}\" -o ${BENCH_LL}"
llvm-dis "${BENCH}" -o ${BENCH_LL}
echo ""
echo "Successfully transform the LLVM IR"
echo "Output:"
echo "Transformed LLVM IR:                            ${BENCH_TRANS}"
echo "Transformed LLVM IR in human-readable format:   ${BENCH_TRANS_LL}"
echo "Original LLVM IR in human-readable format:      ${BENCH_LL}"