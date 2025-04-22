#ifndef VALUE_METADATA_REMOVAL_H
#define VALUE_METADATA_REMOVAL_H

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

class ValueMetadataRemoval : public llvm::PassInfoMixin<ValueMetadataRemoval> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
};

#endif // VALUE_METADATA_REMOVAL_H