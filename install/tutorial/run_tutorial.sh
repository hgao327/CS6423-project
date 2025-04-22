#!/bin/bash

CURR=$(readlink -f "$0")
ROOT=$(dirname "$CURR")
PLUGIN="${ROOT}/libtutorial.so"
PASS="input-check"
SRC="${ROOT}/sum.c"
CHECK_SRC="${ROOT}/check.c"
BC="sum.bc"
INST_BC="sum_inst.bc"
EXE="sum.exe"

echo ">> compile the program to llvm ir"
echo "clang -c -emit-llvm -o \"${BC}\" \"${SRC}\""
clang -c -emit-llvm -o "${BC}" "${SRC}"
echo ""

echo ">> instrument the program to invoke \"checkInput\" before the first statement of the \"main\" function"
echo "opt -load-pass-plugin \"${PLUGIN}\" -passes=${PASS} \"${BC}\" -o \"${INST_BC}\""
opt -load-pass-plugin "${PLUGIN}" -passes=${PASS} "${BC}" -o "${INST_BC}"
echo ""

echo ">> Link the instrumented program with the definition of \"checkInput\""
echo "clang -o \"${EXE}\" \"${INST_BC}\" \"${CHECK_SRC}\""
clang -o "${EXE}" "${INST_BC}" "${CHECK_SRC}"
echo ""

echo ">> Invoke the transformed executable"
echo "./${EXE} 10 20"
echo ""
./${EXE} 10 20
