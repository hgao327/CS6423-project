#include "ArrayAccessDetection.h"
#include "BoundCheckInsertion.h"
#include "BoundCheckOptimization.h"
#include "ValueMetadataRemoval.h"

#define REGISTER_FUNC_PASS(PASS_BUILDER, NAME, CLASS)                  \
  do {                                                                 \
    PASS_BUILDER.registerPipelineParsingCallback(                      \
      [](StringRef Name, FunctionPassManager &FPM,                     \
         ArrayRef<PassBuilder::PipelineElement>) {                     \
        if (Name == #NAME) {                                           \
          FPM.addPass(CLASS());                                        \
          return true;                                                 \
        }                                                              \
        return false;                                                  \
      });                                                              \
  } while (0)
  
using namespace llvm;

// This is the core interface for pass plugins. It guarantees that 'opt' will
// be able to recognize HelloWorld when added to the pass pipeline on the
// command line, i.e. via '-passes=hello-world'
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "CS6241_PROJ1", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            REGISTER_FUNC_PASS(PB, access-det, ArrayAccessDetection);
            REGISTER_FUNC_PASS(PB, check-ins, BoundCheckInsertion);
            REGISTER_FUNC_PASS(PB, check-opt, BoundCheckOptimization);
            REGISTER_FUNC_PASS(PB, valuemd-rem, ValueMetadataRemoval);
          }};
}