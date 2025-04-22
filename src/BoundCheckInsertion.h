#ifndef BOUND_CHECK_INSERTION_H
#define BOUND_CHECK_INSERTION_H

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

class BoundCheckInsertion : public llvm::PassInfoMixin<BoundCheckInsertion> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
  virtual ~BoundCheckInsertion();
  void InstrumentationExample();
};

#endif // BOUND_CHECK_INSERTION_H