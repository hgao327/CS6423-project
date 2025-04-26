#include "ArrayAccessDetection.h"
#include "CommonDef.h"
#include "llvm/ADT/SmallSet.h"

using namespace llvm;

static Value *INVALID_BOUND = reinterpret_cast<Value *>(0xFFFFFFFFFFFFFFFFU);

MDTuple *ArrayAccessDetection::calculateBoundforGEP(
    GetElementPtrInst *GI, DenseMap<Value *, CallBase *> &ValueSource,
    DenseMap<CallBase *, Value *> &MallocBound) {
  LLVMContext &Context = GI->getFunction()->getContext();
  if (GI->getSourceElementType()->isArrayTy()) {
    GI->print(verboseOut());
    verboseOut() << "\n  Bound: "
                 << GI->getSourceElementType()->getArrayNumElements() << "\n";
    if (DILocation *Loc = GI->getDebugLoc()) {
      verboseOut() << "  Source: " << Loc->getFilename() << " "
                   << Loc->getLine() << "\n";
    }
    verboseOut() << "\n";
    return MDNode::get(
        Context, {MDString::get(Context, "static array"),
                  ConstantAsMetadata::get(ConstantInt::get(
                      Type::getInt64Ty(Context),
                      GI->getSourceElementType()->getArrayNumElements()))});
  }

  CallBase *Allocator = nullptr;
  Value *Base = GI->getPointerOperand();
  while (Base) {
    Value *BaseBefore = Base;
    if (isa<LoadInst>(Base)) {
      auto Iter = ValueSource.find(Base);
      if (Iter != ValueSource.end()) {
        Base = Iter->getSecond();
      }
    } else if (isa<CallBase>(Base)) {
      CallBase *CB = cast<CallBase>(Base);
      if (CB->getCalledFunction()->getName() == "malloc") {
        Allocator = CB;
      }
    }
    if (BaseBefore == Base) {
      break;
    }
  }

  if (Allocator) {
    Value *Bound;
    auto Iter = MallocBound.find(Allocator);
    if (Iter == MallocBound.end()) {
      Bound = tackleMalloc(Allocator, GI->getResultElementType());
      MallocBound.insert({Allocator, Bound});
    } else {
      Bound = Iter->getSecond();
    }
    if (Bound == INVALID_BOUND) {
      return nullptr;
    } else {
      GI->print(verboseOut());
      if (DILocation *Loc = GI->getDebugLoc()) {
        verboseOut() << "\n  Source: " << Loc->getFilename() << " "
                     << Loc->getLine();
      }
      verboseOut() << "\n  Bound: ";
      Bound->print(verboseOut());
      verboseOut() << "\n  Allocator: ";
      Allocator->print(verboseOut());
      verboseOut() << "\n";
      return MDNode::get(Context, {MDString::get(Context, "dynamic array"),
                                   ValueAsMetadata::get(Bound),
                                   ValueAsMetadata::get(Allocator)});
    }
  } else {
    return nullptr;
  }
}

void ArrayAccessDetection::tackleGEP(
    GetElementPtrInst *GI, DenseMap<Value *, CallBase *> &ValueSource,
    DenseMap<CallBase *, Value *> &MallocBound) {
  Type *SourceType = GI->getSourceElementType();
  Type *ResultType = GI->getResultElementType();
  // only tackle access to non-array elment
  if (SourceType->isVectorTy()) {
    return;
  }
  MDTuple *MT = calculateBoundforGEP(GI, ValueSource, MallocBound);
  if (MT) {
    GI->setMetadata(ACCESS_KEY, MT);
  }
}

Value *ArrayAccessDetection::tackleMalloc(CallBase *CB, Type *ElementTy) {
  Value *Size = CB->getArgOperand(0);
  Module *M = CB->getFunction()->getParent();
  Value *Bound = INVALID_BOUND;
  uint64_t ElemSizeInByte =
      M->getDataLayout().getTypeSizeInBits(ElementTy).getFixedSize() / 8;
  if (isa<BinaryOperator>(Size)) {
    BinaryOperator *BO = cast<BinaryOperator>(Size);
    if (BO->getOpcode() == Instruction::Mul) {
      Value *Op1 = BO->getOperand(0);
      Value *Op2 = BO->getOperand(1);
      if (isa<ConstantInt>(Op1) &&
          cast<ConstantInt>(Op1)->getZExtValue() == ElemSizeInByte) {
        Bound = Op2;
      } else if (isa<ConstantInt>(Op2) &&
                 cast<ConstantInt>(Op2)->getZExtValue() == ElemSizeInByte) {
        Bound = Op1;
      } else {
        Bound = INVALID_BOUND;
      }
    }
  }
  return Bound;
}

PreservedAnalyses ArrayAccessDetection::run(Function &F,
                                            FunctionAnalysisManager &FAM) {
  if (!isCProgram(F.getParent()) && isCxxSTLFunc(F.getName())) {
    return PreservedAnalyses::all();
  }
  verboseOut() << "Detect Array Access in " << F.getName() << "\n";
  // SmallSet<CallBase *, 16> MallocSet;
  DenseMap<Value *, CallBase *> ValueSource;
  DenseMap<CallBase *, Value *> MallocBound;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (isa<GetElementPtrInst>(&I)) {
        GetElementPtrInst *GI = cast<GetElementPtrInst>(&I);
        tackleGEP(GI, ValueSource, MallocBound);
      } else if (isa<CallBase>(&I)) {
        CallBase *CB = cast<CallBase>(&I);
        if (CB->getCalledFunction() &&
            CB->getCalledFunction()->getName() == "malloc") {
          ValueSource.insert({CB, CB});
        }
      } else if (isa<StoreInst>(&I)) {
        StoreInst *SI = cast<StoreInst>(&I);
        Value *Source = SI->getValueOperand();
        auto Iter = ValueSource.find(Source);
        if (Iter != ValueSource.end()) {
          ValueSource.insert({SI->getPointerOperand(), Iter->getSecond()});
        }
      } else if (isa<LoadInst>(&I)) {
        LoadInst *LI = cast<LoadInst>(&I);
        Value *Pointer = LI->getPointerOperand();
        auto Iter = ValueSource.find(Pointer);
        if (Iter != ValueSource.end()) {
          ValueSource.insert({LI, Iter->getSecond()});
        }
      }
    }
  }
  
  return PreservedAnalyses::all();
}

ArrayAccessDetection::~ArrayAccessDetection() {}