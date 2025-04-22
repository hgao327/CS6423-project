#include "ValueMetadataRemoval.h"
#include "CommonDef.h"

using namespace llvm;

PreservedAnalyses ValueMetadataRemoval::run(Function &F, FunctionAnalysisManager &FAM) {
  if (!isCProgram(F.getParent()) && isCxxSTLFunc(F.getName())) {
    return PreservedAnalyses::all();
  }
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (I.getOpcode() == Instruction::GetElementPtr && I.getMetadata(ACCESS_KEY)) {
        Metadata *ArrayType = I.getMetadata(ACCESS_KEY)->getOperand(0).get();
        I.setMetadata(ACCESS_KEY, MDNode::get(F.getParent()->getContext(), ArrayType));
      }
    }
  }
  return PreservedAnalyses::none();
}
