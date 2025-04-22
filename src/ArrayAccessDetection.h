#ifndef ARRAY_ACCESS_DETECTION_H
#define ARRAY_ACCESS_DETECTION_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"

class ArrayAccessDetection : public llvm::PassInfoMixin<ArrayAccessDetection>
{
public:
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
  llvm::MDTuple *calculateBoundforGEP(llvm::GetElementPtrInst *GI, llvm::DenseMap<llvm::Value *, llvm::CallBase *> &ValueSource, llvm::DenseMap<llvm::CallBase *, llvm::Value *> &MallocBound);
  void tackleGEP(llvm::GetElementPtrInst *GI, llvm::DenseMap<llvm::Value *, llvm::CallBase *> &ValueSource, llvm::DenseMap<llvm::CallBase *, llvm::Value *> &MallocBound);
  llvm::Value *tackleMalloc(llvm::CallBase *CB, llvm::Type *ElementTy);
  static bool isRequired() { return true; }
  virtual ~ArrayAccessDetection();
};

#endif // ARRAY_ACCESS_DETECTION_H