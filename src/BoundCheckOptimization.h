#ifndef BOUND_CHECK_OPTIMIZATION_H
#define BOUND_CHECK_OPTIMIZATION_H

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

class BoundCheckOptimization : public llvm::PassInfoMixin<BoundCheckOptimization> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
  virtual ~BoundCheckOptimization();
};

#endif // BOUND_CHECK_OPTIMIZATION_H