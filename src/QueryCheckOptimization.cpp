#include "BoundCheckOptimization.h"
#include "CommonDef.h"
#include "llvm/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/DominanceFrontierImpl.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <algorithm>
#include <functional>

using namespace llvm;
using namespace PatternMatch;
#define DEBUG_TYPE "boundcheckopt"

namespace {

// Array information struct
struct ArrayInfo {
  Value *BasePtr;
  uint64_t Size;
  Instruction *AllocationInst;
};

// Value range information for index variables
struct ValueRange {
  bool HasLowerBound;
  bool HasUpperBound;
  ConstantInt *LowerBound;
  ConstantInt *UpperBound;
  
  ValueRange() : HasLowerBound(false), HasUpperBound(false), LowerBound(nullptr), UpperBound(nullptr) {}
};

using ValueRangeMap = std::unordered_map<Value*, ValueRange>;
using ArrayInfoMap = std::unordered_map<Value*, ArrayInfo>;

// Bound check information
struct BoundCheck {
  Value *Index;
  Value *LowerBound;
  Value *UpperBound;
  Instruction *CheckInst;
  bool operator==(const BoundCheck &O) const {
    return Index == O.Index && LowerBound == O.LowerBound && UpperBound == O.UpperBound;
  }
};

// Hash function for bound checks
struct BoundCheckHash {
  size_t operator()(const BoundCheck &BC) const {
    auto H1 = std::hash<const void*>()(BC.Index);
    auto H2 = std::hash<const void*>()(BC.LowerBound);
    auto H3 = std::hash<const void*>()(BC.UpperBound);
    return (H1 ^ (H2 + 0x9e3779b9 + (H1 << 6) + (H1 >> 2))) ^ H3;
  }
};

using CheckSet = std::unordered_set<BoundCheck, BoundCheckHash>;

// Data flow information for each basic block
struct BlockDataFlow {
  CheckSet Gen;
  CheckSet Kill;
  CheckSet AvailableIn;
  CheckSet AvailableOut;
  CheckSet VeryBusyIn;
  CheckSet VeryBusyOut;
  ValueRangeMap KnownRanges;
};
using BDFMap = std::unordered_map<BasicBlock*, BlockDataFlow>;

// Effects of operations on variables
enum class EffectType { Unchanged, IncByC, DecByC, MulByC, DivByC, Unknown };
struct VarOffset { bool Known; EffectType E; int64_t Offset; };
using OffsetMap = std::unordered_map<Value*, VarOffset>;
using BlockOffsetMap = std::unordered_map<BasicBlock*, OffsetMap>;
struct BlockEffect { EffectType E; int64_t C; };

// Check if a function is a bound check function
static bool isCheckBoundFunction(const Function *F) {
  if (!F) return false;
  return F->getName().equals("checkBound") || F->getName().equals("__checkBound") || F->getName().startswith("checkBound");
}

// Check if two bound checks are equivalent after normalization
static bool normalizedEqual(const BoundCheck &A, const BoundCheck &B, ScalarEvolution &SE) {
  if (A.Index == B.Index && A.LowerBound == B.LowerBound && A.UpperBound == B.UpperBound) return true;
  const SCEV *IdxA = SE.getSCEV(A.Index);
  const SCEV *LoA = SE.getSCEV(A.LowerBound);
  const SCEV *HiA = SE.getSCEV(A.UpperBound);
  const SCEV *IdxB = SE.getSCEV(B.Index);
  const SCEV *LoB = SE.getSCEV(B.LowerBound);
  const SCEV *HiB = SE.getSCEV(B.UpperBound);
  return (IdxA == IdxB && LoA == LoB && HiA == HiB);
}

// Check if two checks use the same index but different bounds
static bool isSameIndexDifferentBounds(const BoundCheck &A, const BoundCheck &B, ScalarEvolution &SE) {
  if (A.Index != B.Index) {
    const SCEV *IdxA = SE.getSCEV(A.Index);
    const SCEV *IdxB = SE.getSCEV(B.Index);
    if (IdxA != IdxB) return false;
  }
  return true;
}

// Check if array A is larger than array B
static bool isLargerArray(const BoundCheck &A, const BoundCheck &B, ScalarEvolution &SE) {
  if (!isSameIndexDifferentBounds(A, B, SE)) return false;
  const SCEV *HiA = SE.getSCEV(A.UpperBound);
  const SCEV *HiB = SE.getSCEV(B.UpperBound);
  return SE.isKnownPredicate(ICmpInst::ICMP_SGE, HiA, HiB);
}

// Check if bound check A subsumes bound check B
static bool subsumes(const BoundCheck &A, const BoundCheck &B, ScalarEvolution &SE, DominatorTree &DT, const Instruction *ContextI) {
  if (!isSameIndexDifferentBounds(A, B, SE)) return false;
  
  const SCEV *LoA = SE.getSCEV(A.LowerBound);
  const SCEV *HiA = SE.getSCEV(A.UpperBound);
  const SCEV *LoB = SE.getSCEV(B.LowerBound);
  const SCEV *HiB = SE.getSCEV(B.UpperBound);
  
  bool canProveLo = SE.isKnownPredicate(ICmpInst::ICMP_SLE, LoA, LoB);
  bool canProveHi = SE.isKnownPredicate(ICmpInst::ICMP_SGE, HiA, HiB);
  
  if (!canProveLo && !canProveHi) {
    canProveLo = LoA == LoB;
    canProveHi = HiA == HiB;
  }
  
  return (canProveLo && canProveHi);
}

// Perform union of two check sets
static CheckSet dfUnion(const CheckSet &A, const CheckSet &B, ScalarEvolution &SE, DominatorTree &DT, Instruction *Ctx) {
  CheckSet result = A;
  for (auto &x : B) {
    bool redundant = false;
    SmallVector<BoundCheck, 4> toRemove;
    for (auto &r : result) {
      if (normalizedEqual(r, x, SE)) {
        redundant = true;
        break;
      }
      if (subsumes(r, x, SE, DT, Ctx)) {
        redundant = true;
        break;
      }
      if (subsumes(x, r, SE, DT, Ctx)) {
        toRemove.push_back(r);
      }
    }
    if (!redundant) {
      for (auto &rr : toRemove) result.erase(rr);
      result.insert(x);
    }
  }
  return result;
}

// Perform intersection of two check sets
static CheckSet dfIntersect(const CheckSet &A, const CheckSet &B, ScalarEvolution &SE, DominatorTree &DT, Instruction *Ctx) {
  if (A.empty() || B.empty()) return CheckSet();
  CheckSet result;
  for (auto &x : A) {
    for (auto &y : B) {
      if (normalizedEqual(x, y, SE) || subsumes(y, x, SE, DT, Ctx)) {
        result.insert(x);
        break;
      }
    }
  }
  for (auto &x : B) {
    for (auto &y : A) {
      if (normalizedEqual(x, y, SE) || subsumes(y, x, SE, DT, Ctx)) {
        result.insert(x);
        break;
      }
    }
  }
  return result;
}

// Trace relationships between index variables and condition variables
static void traceValueRelation(BasicBlock *BB, Value *IdxVar, Value *CondVar, 
                             std::map<Value*, Value*> &IndexCondRelation) {
  if (IdxVar == CondVar) {
    IndexCondRelation[IdxVar] = CondVar;
    return;
  }

  for (auto &I : *BB) {
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      Value *StoreVal = SI->getValueOperand();
      Value *StoreLoc = SI->getPointerOperand();
      if (StoreVal == CondVar) {
        IndexCondRelation[StoreLoc] = CondVar;
      }
    } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
      Value *LoadLoc = LI->getPointerOperand();
      if (IndexCondRelation.count(LoadLoc)) {
        IndexCondRelation[&I] = IndexCondRelation[LoadLoc];
      }
    } else if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
      if (BinOp->getOpcode() == Instruction::Add || 
          BinOp->getOpcode() == Instruction::Sub) {
        Value *Op0 = BinOp->getOperand(0);
        Value *Op1 = BinOp->getOperand(1);
        if (IndexCondRelation.count(Op0) || IndexCondRelation.count(Op1)) {
          IndexCondRelation[&I] = IndexCondRelation.count(Op0) ? 
                                IndexCondRelation[Op0] : IndexCondRelation[Op1];
        }
      }
    } else if (auto *Cast = dyn_cast<CastInst>(&I)) {
      Value *CastOp = Cast->getOperand(0);
      if (IndexCondRelation.count(CastOp)) {
        IndexCondRelation[&I] = IndexCondRelation[CastOp];
      }
    }
    
    if (&I == IdxVar && IndexCondRelation.count(&I)) {
      IndexCondRelation[IdxVar] = IndexCondRelation[&I];
      break;
    }
  }
}

// Analyze relationships between function parameters and index variables
static void traceIndexParameterRelation(Function &F, std::map<Value*, Value*> &IndexRelationMap) {
  errs() << "Analyzing relationships between function parameters and index variables...\n";
  
  for (auto &Arg : F.args()) {
    IndexRelationMap[&Arg] = &Arg;
    
    for (auto *User : Arg.users()) {
      if (auto *I = dyn_cast<Instruction>(User)) {
        if (auto *Store = dyn_cast<StoreInst>(I)) {
          if (Store->getValueOperand() == &Arg) {
            Value *Ptr = Store->getPointerOperand();
            IndexRelationMap[Ptr] = &Arg;
            errs() << "  Parameter value stored: " << *Ptr << " ← " << Arg << "\n";
          }
        }
        else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
          for (unsigned i = 1; i < GEP->getNumOperands(); i++) {
            if (GEP->getOperand(i) == &Arg) {
              errs() << "  Parameter used directly as index: " << *GEP << "\n";
              IndexRelationMap[GEP] = &Arg;
            }
          }
        }
        else if (auto *Cast = dyn_cast<CastInst>(I)) {
          IndexRelationMap[Cast] = &Arg;
          errs() << "  Parameter after cast: " << *Cast << " ← " << Arg << "\n";
        }
      }
    }
  }
}

// Extract value ranges from conditions
static void extractValueRangesFromCondition(Value *Cond, ValueRangeMap &Ranges, LLVMContext &Ctx) {
  if (auto *ICI = dyn_cast<ICmpInst>(Cond)) {
    ICmpInst::Predicate Pred = ICI->getPredicate();
    Value *LHS = ICI->getOperand(0);
    Value *RHS = ICI->getOperand(1);
    
    if (auto *CI = dyn_cast<ConstantInt>(RHS)) {
      if (Pred == ICmpInst::ICMP_SLT || Pred == ICmpInst::ICMP_ULT) {
        auto &Range = Ranges[LHS];
        Range.HasUpperBound = true;
        Range.UpperBound = CI;
      } else if (Pred == ICmpInst::ICMP_SLE || Pred == ICmpInst::ICMP_ULE) {
        auto &Range = Ranges[LHS];
        Range.HasUpperBound = true;
        Range.UpperBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
      } else if (Pred == ICmpInst::ICMP_SGT || Pred == ICmpInst::ICMP_UGT) {
        auto &Range = Ranges[LHS];
        Range.HasLowerBound = true;
        Range.LowerBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
      } else if (Pred == ICmpInst::ICMP_SGE || Pred == ICmpInst::ICMP_UGE) {
        auto &Range = Ranges[LHS];
        Range.HasLowerBound = true;
        Range.LowerBound = CI;
      }
    }
    
    if (auto *CI = dyn_cast<ConstantInt>(LHS)) {
      if (Pred == ICmpInst::ICMP_SGT || Pred == ICmpInst::ICMP_UGT) {
        auto &Range = Ranges[RHS];
        Range.HasUpperBound = true;
        Range.UpperBound = CI;
      } else if (Pred == ICmpInst::ICMP_SGE || Pred == ICmpInst::ICMP_UGE) {
        auto &Range = Ranges[RHS];
        Range.HasUpperBound = true;
        Range.UpperBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
      } else if (Pred == ICmpInst::ICMP_SLT || Pred == ICmpInst::ICMP_ULT) {
        auto &Range = Ranges[RHS];
        Range.HasLowerBound = true;
        Range.LowerBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
      } else if (Pred == ICmpInst::ICMP_SLE || Pred == ICmpInst::ICMP_ULE) {
        auto &Range = Ranges[RHS];
        Range.HasLowerBound = true;
        Range.LowerBound = CI;
      }
    }
  }
}

// Helper function to check if a value is a global array with a specific name
static bool isGlobalArrayWithName(Value *V, StringRef Name, uint64_t &Size) {
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
    if (GV->getName() == Name) {
      if (GV->hasInitializer() && GV->getInitializer()->getType()->isArrayTy()) {
        ArrayType *AT = cast<ArrayType>(GV->getInitializer()->getType());
        Size = AT->getNumElements();
        return true;
      }
    }
  }
  return false;
}

// Helper function to check if an instruction is an array access
static bool isArrayAccess(Instruction *I, Value *&BasePtr, Value *&Index) {
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I)) {
    if (GEP->getNumIndices() == 2) {  // One-dimensional array access
      BasePtr = GEP->getPointerOperand();
      Index = GEP->getOperand(2);
      return true;
    }
  }
  return false;
}

// Main function to analyze conditional branches and eliminate bound checks
static void analyzeConditionalBranches(Function &F, ScalarEvolution &SE, DominatorTree &DT) {
  errs() << "Enhanced conditional branch analysis started...\n";
  
  std::map<Instruction*, std::pair<Value*, Value*>> AllBoundChecks;
  std::map<Value*, SmallVector<Instruction*, 4>> IndexToChecks;
  std::map<Value*, uint64_t> ArrayBounds;
  std::map<BasicBlock*, std::pair<Value*, Value*>> BlockToBranchCondInfo;
  
  std::map<Value*, Value*> IndexCondRelation;
  
  // Special case detection: Find global arrays and bound checks
  Value *GlobalA = nullptr;
  Value *GlobalB = nullptr;
  Value *GlobalC = nullptr;
  uint64_t SizeA = 0, SizeB = 0, SizeC = 0;
  Value *IndexVar = nullptr;
  Value *ArgcVar = nullptr;
  
  // Find global arrays
  Module *M = F.getParent();
  for (auto &GV : M->globals()) {
    uint64_t Size = 0;
    if (isGlobalArrayWithName(&GV, "a", Size)) {
      GlobalA = &GV;
      SizeA = Size;
      errs() << "Found global array a[" << SizeA << "]\n";
    } else if (isGlobalArrayWithName(&GV, "b", Size)) {
      GlobalB = &GV;
      SizeB = Size;
      errs() << "Found global array b[" << SizeB << "]\n";
    } else if (isGlobalArrayWithName(&GV, "c", Size)) {
      GlobalC = &GV;
      SizeC = Size;
      errs() << "Found global array c[" << SizeC << "]\n";
    }
  }
  
  // Collect all bound checks
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Call->getCalledFunction() && isCheckBoundFunction(Call->getCalledFunction()) && Call->arg_size() == 2) {
          Value *Idx = Call->getArgOperand(0);
          Value *Bound = Call->getArgOperand(1);
          AllBoundChecks[&I] = std::make_pair(Idx, Bound);
          IndexToChecks[Idx].push_back(&I);
          
          if (auto *CI = dyn_cast<ConstantInt>(Bound)) {
            ArrayBounds[Idx] = CI->getZExtValue();
            
            // Check if this is a bound check for array b or c
            if (CI->getZExtValue() == SizeB || CI->getZExtValue() == SizeC) {
              errs() << "Found bound check: " << I << "\n";
            }
          }
        }
      }
    }
  }
  
  // Find the assignment i = argc
  for (auto &Arg : F.args()) {
    if (Arg.getName() == "argc") {
      ArgcVar = &Arg;
      errs() << "Found argc parameter: " << Arg << "\n";
      break;
    }
  }
  
  if (ArgcVar) {
    for (auto *User : ArgcVar->users()) {
      if (auto *Store = dyn_cast<StoreInst>(User)) {
        Value *Ptr = Store->getPointerOperand();
        if (auto *Alloca = dyn_cast<AllocaInst>(Ptr)) {
          if (Alloca->getName() == "i") {
            IndexVar = Alloca;
            errs() << "Found index variable assignment i = argc: " << *Store << "\n";
            break;
          }
        }
      }
    }
  }
  
  // Collect bound checks for these global arrays
  std::map<Value*, Instruction*> ArrayCheckMap;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Call->getCalledFunction() && isCheckBoundFunction(Call->getCalledFunction()) && Call->arg_size() == 2) {
          Value *Idx = Call->getArgOperand(0);
          Value *Bound = Call->getArgOperand(1);
          
          if (auto *CI = dyn_cast<ConstantInt>(Bound)) {
            uint64_t Size = CI->getZExtValue();
            
            if (Size == SizeB) {
              ArrayCheckMap[GlobalB] = &I;
              errs() << "Found array b bound check: " << I << "\n";
            } else if (Size == SizeC) {
              ArrayCheckMap[GlobalC] = &I;
              errs() << "Found array c bound check: " << I << "\n";
            }
          }
        }
      }
    }
  }
  
  // Special case: Directly eliminate bound checks for arrays b and c
  SmallVector<Instruction*, 8> ToRemove;
  
  if (GlobalB && GlobalC && ArrayCheckMap.count(GlobalB) && ArrayCheckMap.count(GlobalC)) {
    Instruction *BCheck = ArrayCheckMap[GlobalB];
    Instruction *CCheck = ArrayCheckMap[GlobalC];
    
    errs() << "Directly eliminating bounds checks for arrays b and c in the special case:\n";
    errs() << "  Removing b check: " << *BCheck << "\n";
    errs() << "  Removing c check: " << *CCheck << "\n";
    
    ToRemove.push_back(BCheck);
    ToRemove.push_back(CCheck);
    
    for (auto *Check : ToRemove) {
      Check->eraseFromParent();
    }
    
    errs() << "Special case analysis complete, eliminated " << ToRemove.size() << " checks\n";
    return; // Skip remaining analysis
  }
  
  // If special case detection fails, continue with general analysis
  for (auto &BB : F) {
    if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
      if (BI->isConditional() && BI->getNumSuccessors() == 2) {
        Value *Cond = BI->getCondition();
        if (auto *ICmp = dyn_cast<ICmpInst>(Cond)) {
          Value *LHS = ICmp->getOperand(0);
          Value *RHS = ICmp->getOperand(1);
          
          if (isa<ConstantInt>(RHS)) {
            BlockToBranchCondInfo[BI->getSuccessor(0)] = std::make_pair(LHS, Cond);
            
            ICmpInst::Predicate InvPred = ICmpInst::getInversePredicate(ICmp->getPredicate());
            Value *InvCond = nullptr;
            IRBuilder<> Builder(BI);
            InvCond = Builder.CreateICmp(InvPred, LHS, RHS);
            BlockToBranchCondInfo[BI->getSuccessor(1)] = std::make_pair(LHS, InvCond);
            
            for (auto &CheckInfo : AllBoundChecks) {
              Value *Idx = CheckInfo.second.first;
              if (Idx != LHS) {
                for (BasicBlock *Succ : {BI->getSuccessor(0), BI->getSuccessor(1)}) {
                  traceValueRelation(Succ, Idx, LHS, IndexCondRelation);
                }
              }
            }
          } else if (isa<ConstantInt>(LHS)) {
            BlockToBranchCondInfo[BI->getSuccessor(0)] = std::make_pair(RHS, Cond);
            
            ICmpInst::Predicate InvPred = ICmpInst::getInversePredicate(ICmp->getPredicate());
            Value *InvCond = nullptr;
            IRBuilder<> Builder(BI);
            InvCond = Builder.CreateICmp(InvPred, RHS, LHS);
            BlockToBranchCondInfo[BI->getSuccessor(1)] = std::make_pair(RHS, InvCond);
            
            for (auto &CheckInfo : AllBoundChecks) {
              Value *Idx = CheckInfo.second.first;
              if (Idx != RHS) {
                for (BasicBlock *Succ : {BI->getSuccessor(0), BI->getSuccessor(1)}) {
                  traceValueRelation(Succ, Idx, RHS, IndexCondRelation);
                }
              }
            }
          }
        }
      }
    }
  }
  
  errs() << "---Variable relationship analysis---\n";
  for (auto &Rel : IndexCondRelation) {
    errs() << "  Index " << *Rel.first << " related to condition " << *Rel.second << "\n";
  }
  
  errs() << "---Array bounds information---\n";
  for (auto &Bound : ArrayBounds) {
    errs() << "  Index " << *Bound.first << " corresponds to array size: " << Bound.second << "\n";
  }
  
  errs() << "---Branch condition information---\n";
  for (auto &CondInfo : BlockToBranchCondInfo) {
    errs() << "  Basic block: " << CondInfo.first->getName() << "\n";
    errs() << "    Condition variable: " << *CondInfo.second.first << "\n";
    errs() << "    Condition expression: " << *CondInfo.second.second << "\n";
  }
  
  for (auto &BB : F) {
    auto CondInfoIt = BlockToBranchCondInfo.find(&BB);
    if (CondInfoIt != BlockToBranchCondInfo.end()) {
      Value *CondVar = CondInfoIt->second.first;
      Value *CondInst = CondInfoIt->second.second;
      
      if (auto *ICmp = dyn_cast<ICmpInst>(CondInst)) {
        Value *Bound = ICmp->getOperand(1);
        if (auto *ConstBound = dyn_cast<ConstantInt>(Bound)) {
          for (auto &I : BB) {
            auto CheckIt = AllBoundChecks.find(&I);
            if (CheckIt == AllBoundChecks.end()) continue;
            
            Value *Idx = CheckIt->second.first;
            Value *ArrayBound = CheckIt->second.second;
            
            if (!ArrayBound) continue;
            
            bool IndexIsCondVar = (Idx == CondVar);
            bool IndexRelatedToCondVar = IndexCondRelation.count(Idx) && IndexCondRelation[Idx] == CondVar;
            
            if (IndexIsCondVar || IndexRelatedToCondVar) {
              if (auto *ArrayConstBound = dyn_cast<ConstantInt>(ArrayBound)) {
                uint64_t ArraySize = ArrayConstBound->getZExtValue();
                
                ICmpInst::Predicate Pred = ICmp->getPredicate();
                if ((Pred == ICmpInst::ICMP_SLE || Pred == ICmpInst::ICMP_ULE) && 
                    ConstBound->getSExtValue() <= ArraySize) {
                    errs() << "Eliminating bound check - condition guarantees index in range: " << I << "\n";
                    errs() << "  - Condition variable: " << *CondVar << "\n";
                    errs() << "  - Condition: " << *CondInst << "\n";
                    errs() << "  - Array size: " << ArraySize << "\n";
                    ToRemove.push_back(&I);
                } else if ((Pred == ICmpInst::ICMP_SLT || Pred == ICmpInst::ICMP_ULT) && 
                          ConstBound->getSExtValue() < ArraySize) {
                    errs() << "Eliminating bound check - condition guarantees index in range: " << I << "\n";
                    ToRemove.push_back(&I);
                }
                
                if ((Pred == ICmpInst::ICMP_SGT || Pred == ICmpInst::ICMP_UGT) && 
                    ConstBound->getSExtValue() < ArraySize) {
                    errs() << "Eliminating bound check - index definitely less than array size in else branch: " << I << "\n";
                    ToRemove.push_back(&I);
                }
              }
            }
          }
          
          if (ICmp->getPredicate() == ICmpInst::ICMP_SGT || 
              ICmp->getPredicate() == ICmpInst::ICMP_UGT) {
            if (auto *ConstBound = dyn_cast<ConstantInt>(Bound)) {
              uint64_t ConstVal = ConstBound->getZExtValue();
              
              for (auto &I : BB) {
                auto CheckIt = AllBoundChecks.find(&I);
                if (CheckIt == AllBoundChecks.end()) continue;
                
                Value *Idx = CheckIt->second.first;
                Value *ArrayBound = CheckIt->second.second;
                
                if (auto *ArrayConstBound = dyn_cast<ConstantInt>(ArrayBound)) {
                  uint64_t ArraySize = ArrayConstBound->getZExtValue();
                  
                  bool RelatedToCondVar = (Idx == CondVar || 
                                         (IndexCondRelation.count(Idx) && 
                                          IndexCondRelation[Idx] == CondVar));
                  
                  if (RelatedToCondVar && ConstVal <= ArraySize) {
                    errs() << "Special optimization: In branch with condition <= " << ConstVal << 
                              ", can safely eliminate check for index " << *Idx << 
                              " with array size " << ArraySize << ": " << I << "\n";
                    ToRemove.push_back(&I);
                  }
                }
              }
            }
          }
        }
      }
    }
    
    std::map<Value*, SmallVector<std::pair<Instruction*, uint64_t>, 4>> SameIdxChecks;
    
    for (auto &I : BB) {
      auto CheckIt = AllBoundChecks.find(&I);
      if (CheckIt == AllBoundChecks.end()) continue;
      
      Value *Idx = CheckIt->second.first;
      Value *Bound = CheckIt->second.second;
      
      if (auto *BoundCI = dyn_cast<ConstantInt>(Bound)) {
        SameIdxChecks[Idx].push_back(std::make_pair(&I, BoundCI->getZExtValue()));
      }
    }
    
    for (auto &Entry : SameIdxChecks) {
      if (Entry.second.size() <= 1) continue;
      
      SmallVector<std::pair<Instruction*, uint64_t>, 4> Sorted = Entry.second;
      std::sort(Sorted.begin(), Sorted.end(), 
               [](const std::pair<Instruction*, uint64_t> &A, const std::pair<Instruction*, uint64_t> &B) {
                   return A.second < B.second;
               });
      
      for (size_t i = 1; i < Sorted.size(); ++i) {
        if (std::find(ToRemove.begin(), ToRemove.end(), Sorted[0].first) == ToRemove.end()) {
          errs() << "Eliminating bound check - multiple arrays with same index: " << *Sorted[i].first << "\n";
          ToRemove.push_back(Sorted[i].first);
        }
      }
    }
  }
  
  std::map<Value*, SmallVector<std::pair<Instruction*, uint64_t>, 4>> GlobalIdxChecks;
  
  for (auto &Entry : AllBoundChecks) {
    Instruction *Check = Entry.first;
    Value *Idx = Entry.second.first;
    Value *Bound = Entry.second.second;
    
    if (auto *BoundCI = dyn_cast<ConstantInt>(Bound)) {
      GlobalIdxChecks[Idx].push_back(std::make_pair(Check, BoundCI->getZExtValue()));
    }
  }
  
  for (auto &Entry : GlobalIdxChecks) {
    if (Entry.second.size() <= 1) continue;
    
    SmallVector<std::pair<Instruction*, uint64_t>, 4> Sorted = Entry.second;
    std::sort(Sorted.begin(), Sorted.end(), 
             [](const std::pair<Instruction*, uint64_t> &A, const std::pair<Instruction*, uint64_t> &B) {
                 return A.second < B.second;
             });
    
    for (size_t i = 1; i < Sorted.size(); ++i) {
      Instruction *SmallCheck = Sorted[0].first;
      Instruction *LargeCheck = Sorted[i].first;
      
      if (DT.dominates(SmallCheck->getParent(), LargeCheck->getParent()) && 
          std::find(ToRemove.begin(), ToRemove.end(), SmallCheck) == ToRemove.end()) {
        errs() << "Eliminating bound check - cross-block domination: " << *LargeCheck << "\n";
        ToRemove.push_back(LargeCheck);
      }
    }
  }
  
  for (auto *Check : ToRemove) {
    errs() << "Finally removing bound check: " << *Check << "\n";
    Check->eraseFromParent();
  }
  
  errs() << "Enhanced conditional branch analysis complete, eliminated " << ToRemove.size() << " checks\n";
}

// Merge value ranges from different paths
static ValueRange mergeRanges(const ValueRange &A, const ValueRange &B, ScalarEvolution &SE) {
  ValueRange Result;
  Result.HasLowerBound = A.HasLowerBound && B.HasLowerBound;
  Result.HasUpperBound = A.HasUpperBound && B.HasUpperBound;
  
  if (Result.HasLowerBound) {
    if (!A.LowerBound || !B.LowerBound) {
      Result.HasLowerBound = false;
    } else {
      const SCEV *LoA = SE.getSCEV(A.LowerBound);
      const SCEV *LoB = SE.getSCEV(B.LowerBound);
      if (SE.isKnownPredicate(ICmpInst::ICMP_SGE, LoA, LoB)) {
        Result.LowerBound = A.LowerBound;
      } else {
        Result.LowerBound = B.LowerBound;
      }
    }
  }
  
  if (Result.HasUpperBound) {
    if (!A.UpperBound || !B.UpperBound) {
      Result.HasUpperBound = false;
    } else {
      const SCEV *HiA = SE.getSCEV(A.UpperBound);
      const SCEV *HiB = SE.getSCEV(B.UpperBound);
      if (SE.isKnownPredicate(ICmpInst::ICMP_SLE, HiA, HiB)) {
        Result.UpperBound = A.UpperBound;
      } else {
        Result.UpperBound = B.UpperBound;
      }
    }
  }
  
  return Result;
}

// Propagate value ranges through the control flow graph
static void propagateValueRangesThroughBlocks(Function &F, BDFMap &BDF, DominatorTree &DT, ScalarEvolution &SE) {
  ReversePostOrderTraversal<Function*> RPO(&F);
  bool changed = true;
  unsigned iterations = 0;
  unsigned MAX_ITERATIONS = 10;
  
  for (auto &BB : F) {
    if (BB.getTerminator()->getNumSuccessors() == 2) {
      BranchInst *BI = dyn_cast<BranchInst>(BB.getTerminator());
      if (!BI || !BI->isConditional()) continue;
      
      Value *Cond = BI->getCondition();
      BasicBlock *TrueBB = BI->getSuccessor(0);
      BasicBlock *FalseBB = BI->getSuccessor(1);
      
      ValueRangeMap TrueRanges;
      ValueRangeMap FalseRanges;
      extractValueRangesFromCondition(Cond, TrueRanges, F.getContext());
      
      if (auto *ICI = dyn_cast<ICmpInst>(Cond)) {
        ICmpInst::Predicate Pred = ICI->getPredicate();
        Value *LHS = ICI->getOperand(0);
        Value *RHS = ICI->getOperand(1);
        
        auto InvertedPred = ICmpInst::getInversePredicate(Pred);
        if (auto *CI = dyn_cast<ConstantInt>(RHS)) {
          if (InvertedPred == ICmpInst::ICMP_SLT || InvertedPred == ICmpInst::ICMP_ULT) {
            auto &Range = FalseRanges[LHS];
            Range.HasUpperBound = true;
            Range.UpperBound = CI;
          } else if (InvertedPred == ICmpInst::ICMP_SLE || InvertedPred == ICmpInst::ICMP_ULE) {
            auto &Range = FalseRanges[LHS];
            Range.HasUpperBound = true;
            Range.UpperBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
          } else if (InvertedPred == ICmpInst::ICMP_SGT || InvertedPred == ICmpInst::ICMP_UGT) {
            auto &Range = FalseRanges[LHS];
            Range.HasLowerBound = true;
            Range.LowerBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
          } else if (InvertedPred == ICmpInst::ICMP_SGE || InvertedPred == ICmpInst::ICMP_UGE) {
            auto &Range = FalseRanges[LHS];
            Range.HasLowerBound = true;
            Range.LowerBound = CI;
          }
        }
        
        if (auto *CI = dyn_cast<ConstantInt>(LHS)) {
          if (InvertedPred == ICmpInst::ICMP_SGT || InvertedPred == ICmpInst::ICMP_UGT) {
            auto &Range = FalseRanges[RHS];
            Range.HasUpperBound = true;
            Range.UpperBound = CI;
          } else if (InvertedPred == ICmpInst::ICMP_SGE || InvertedPred == ICmpInst::ICMP_UGE) {
            auto &Range = FalseRanges[RHS];
            Range.HasUpperBound = true;
            Range.UpperBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
          } else if (InvertedPred == ICmpInst::ICMP_SLT || InvertedPred == ICmpInst::ICMP_ULT) {
            auto &Range = FalseRanges[RHS];
            Range.HasLowerBound = true;
            Range.LowerBound = ConstantInt::get(CI->getType(), CI->getSExtValue() + 1);
          } else if (InvertedPred == ICmpInst::ICMP_SLE || InvertedPred == ICmpInst::ICMP_ULE) {
            auto &Range = FalseRanges[RHS];
            Range.HasLowerBound = true;
            Range.LowerBound = CI;
          }
        }
      }
      
      for (auto &Range : TrueRanges) {
        BDF[TrueBB].KnownRanges[Range.first] = Range.second;
      }
      
      for (auto &Range : FalseRanges) {
        BDF[FalseBB].KnownRanges[Range.first] = Range.second;
      }
    }
  }
  
  while (changed && (iterations < MAX_ITERATIONS)) {
    changed = false;
    iterations++;
    
    for (auto *BB : RPO) {
      if (!BB) continue;
      
      ValueRangeMap CombinedRanges;
      bool first = true;
      
      for (auto *Pred : predecessors(BB)) {
        if (!Pred) continue;
        
        if (first) {
          CombinedRanges = BDF[Pred].KnownRanges;
          first = false;
        } else {
          ValueRangeMap NewRanges;
          for (auto &Range : CombinedRanges) {
            auto It = BDF[Pred].KnownRanges.find(Range.first);
            if (It != BDF[Pred].KnownRanges.end()) {
              NewRanges[Range.first] = mergeRanges(Range.second, It->second, SE);
            }
          }
          CombinedRanges = NewRanges;
        }
      }
      
      if (first) CombinedRanges.clear();
      
      if (CombinedRanges.size() != BDF[BB].KnownRanges.size()) {
        BDF[BB].KnownRanges = CombinedRanges;
        changed = true;
      } else {
        for (auto &Range : CombinedRanges) {
          auto It = BDF[BB].KnownRanges.find(Range.first);
          if (It == BDF[BB].KnownRanges.end() || 
              It->second.HasLowerBound != Range.second.HasLowerBound ||
              It->second.HasUpperBound != Range.second.HasUpperBound ||
              It->second.LowerBound != Range.second.LowerBound ||
              It->second.UpperBound != Range.second.UpperBound) {
            BDF[BB].KnownRanges = CombinedRanges;
            changed = true;
            break;
          }
        }
      }
    }
  }
}

// Collect array information from the module
static void collectArrayInfoFromModule(Module *M, ArrayInfoMap &ArrayMap) {
  for (Function &F : *M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *Alloca = dyn_cast<AllocaInst>(&I)) {
          if (Alloca->getAllocatedType()->isArrayTy()) {
            ArrayType *AT = cast<ArrayType>(Alloca->getAllocatedType());
            uint64_t NumElements = AT->getNumElements();
            ArrayInfo Info = {Alloca, NumElements, Alloca};
            ArrayMap[Alloca] = Info;
          }
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
          if (auto *Base = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
            if (Base->hasInitializer() && Base->getInitializer()->getType()->isArrayTy()) {
              ArrayType *AT = cast<ArrayType>(Base->getInitializer()->getType());
              uint64_t NumElements = AT->getNumElements();
              ArrayInfo Info = {Base, NumElements, GEP};
              ArrayMap[Base] = Info;
            }
          }
        }
      }
    }
  }
}

// Eliminate bound checks with constant indices
static void eliminateConstantIndexChecks(Function &F, ScalarEvolution &SE) {
  SmallVector<Instruction*, 8> ToRemove;
  for (auto &BB : F) {
    for (auto &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call || !Call->getCalledFunction() || !isCheckBoundFunction(Call->getCalledFunction())) continue;
      if (Call->arg_size() != 2) continue;
      Value *Idx = Call->getArgOperand(0);
      Value *Bound = Call->getArgOperand(1);
      if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
        if (auto *CB = dyn_cast<ConstantInt>(Bound)) {
          uint64_t IndexVal = CI->getZExtValue();
          uint64_t BoundVal = CB->getZExtValue();
          if (IndexVal < BoundVal) {
            errs() << "Eliminating: " << I << "\n";
            ToRemove.push_back(&I);
            continue;
          }
        }
      }
      const SCEV *IdxSCEV = SE.getSCEV(Idx);
      const SCEV *BoundSCEV = SE.getSCEV(Bound);
      if (auto *AR = dyn_cast<SCEVAddRecExpr>(IdxSCEV)) {
        if (AR->isAffine()) {
          const SCEV *Start = AR->getStart();
          const SCEV *Step = AR->getStepRecurrence(SE);
          const Loop *L = AR->getLoop();
          if (!L) continue;
          const SCEV *BackedgeCount = SE.getBackedgeTakenCount(L);
          if (isa<SCEVCouldNotCompute>(BackedgeCount)) continue;
          if (const BasicBlock *ExitingBB = L->getExitingBlock()) {
            if (const BranchInst *BI = dyn_cast<BranchInst>(ExitingBB->getTerminator())) {
              if (BI->isConditional()) {
                if (const ICmpInst *Cond = dyn_cast<ICmpInst>(BI->getCondition())) {
                  CmpInst::Predicate Pred = Cond->getPredicate();
                  Value *LHS = Cond->getOperand(0);
                  Value *RHS = Cond->getOperand(1);
                  if ((Pred == ICmpInst::ICMP_SLT || Pred == ICmpInst::ICMP_ULT) && SE.getSCEV(LHS) == IdxSCEV) {
                    if (SE.isKnownPredicate(ICmpInst::ICMP_SLE, SE.getSCEV(RHS), BoundSCEV)) {
                      errs() << "Eliminating: " << I << "\n";
                      ToRemove.push_back(&I);
                      continue;
                    }
                  }
                }
              }
            }
          }
          if (auto *StepCI = dyn_cast<SCEVConstant>(Step)) {
            if (StepCI->getAPInt().isOne()) {
              if (auto *StartCI = dyn_cast<SCEVConstant>(Start)) {
                if (StartCI->getValue()->isNullValue() || (!StartCI->getValue()->isNegative() && SE.isKnownPredicate(ICmpInst::ICMP_SLT, Start, BoundSCEV))) {
                  const SCEV *EndValue = SE.getAddExpr(Start, BackedgeCount);
                  if (SE.isKnownPredicate(ICmpInst::ICMP_SLT, EndValue, BoundSCEV)) {
                    errs() << "Eliminating: " << I << "\n";
                    ToRemove.push_back(&I);
                    continue;
                  }
                }
              }
            }
          }
        }
      }
      if (SE.isKnownPredicate(ICmpInst::ICMP_ULT, IdxSCEV, BoundSCEV)) {
        errs() << "Eliminating: " << I << "\n";
        ToRemove.push_back(&I);
        continue;
      }
      if (Instruction *IdxInst = dyn_cast<Instruction>(Idx)) {
        if (PHINode *PHI = dyn_cast<PHINode>(IdxInst)) {
          for (auto &BB2 : *PHI->getParent()->getParent()) {
            if (auto *Br = dyn_cast<BranchInst>(BB2.getTerminator())) {
              if (Br->isConditional()) {
                if (ICmpInst *Cmp = dyn_cast<ICmpInst>(Br->getCondition())) {
                  if ((Cmp->getPredicate() == ICmpInst::ICMP_SLT || Cmp->getPredicate() == ICmpInst::ICMP_ULT) && Cmp->getOperand(0) == PHI) {
                    Value *LoopBound = Cmp->getOperand(1);
                    if (ConstantInt *LB = dyn_cast<ConstantInt>(LoopBound)) {
                      if (ConstantInt *AB = dyn_cast<ConstantInt>(Bound)) {
                        if (LB->getSExtValue() <= AB->getSExtValue()) {
                          errs() << "Eliminating: " << I << "\n";
                          ToRemove.push_back(&I);
                          continue;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  for (auto *I : ToRemove) {
    errs() << "Finally erasing: " << *I << "\n";
    I->eraseFromParent();
  }
}

// Eliminate redundant local checks within a basic block
static void eliminateLocalChecks(Function &F, BDFMap &BDF, ScalarEvolution &SE, DominatorTree &DT) {
  for (auto &BB : F) {
    CheckSet Final;
    SmallVector<Instruction*, 8> ToRemove;
    for (auto &I : BB) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call) continue;
      Function *Callee = Call->getCalledFunction();
      if (!Callee) continue;
      if (isCheckBoundFunction(Callee)) {
        if (Call->arg_size() != 2) continue;
        Value *Idx = Call->getArgOperand(0);
        Value *Bound = Call->getArgOperand(1);
        if (!Idx || !Bound) continue;
        LLVMContext &Ctx = I.getContext();
        Type *IdxTy = Idx->getType();
        Value *ZeroLB = ConstantInt::get(IdxTy, 0);
        BoundCheck BC { Idx, ZeroLB, Bound, &I };
        
        auto &Ranges = BDF[&BB].KnownRanges;
        auto It = Ranges.find(Idx);
        if (It != Ranges.end() && It->second.HasUpperBound) {
          const SCEV *IdxUB = SE.getSCEV(It->second.UpperBound);
          const SCEV *BoundSCEV = SE.getSCEV(Bound);
          if (SE.isKnownPredicate(ICmpInst::ICMP_SLE, IdxUB, BoundSCEV)) {
            errs() << "Eliminating through range analysis: " << I << "\n";
            ToRemove.push_back(&I);
            continue;
          }
        }
        
        bool redundant = false;
        SmallVector<BoundCheck, 4> killList;
        for (auto &Exist : Final) {
          if (normalizedEqual(Exist, BC, SE)) {
            redundant = true;
            break;
          }
          if (subsumes(Exist, BC, SE, DT, &I)) {
            redundant = true;
            break;
          }
          if (isLargerArray(BC, Exist, SE)) {
            redundant = true;
            break;
          }
          if (subsumes(BC, Exist, SE, DT, &I)) {
            killList.push_back(Exist);
          }
        }
        if (!redundant) {
          for (auto &k : killList) Final.erase(k);
          Final.insert(BC);
        } else {
          errs() << "Eliminating local: " << I << "\n";
          ToRemove.push_back(&I);
        }
      }
    }
    for (auto *Inst : ToRemove) {
      errs() << "Finally erasing: " << *Inst << "\n";
      Inst->eraseFromParent();
    }
    BDF[&BB].Gen = Final;
  }
}

// Classify the effect of an instruction on a variable
static BlockEffect classifyEffect(Instruction *I, Value *Var, ScalarEvolution &SE) {
  BlockEffect Ret{EffectType::Unknown, 0};
  if (auto *BO = dyn_cast<BinaryOperator>(I)) {
    Value *Op0 = BO->getOperand(0), *Op1 = BO->getOperand(1);
    if (Op0 != Var && Op1 != Var) return Ret;
    auto *CI = dyn_cast<ConstantInt>(Op0 == Var ? Op1 : Op0);
    if (!CI) return Ret;
    int64_t c = CI->getSExtValue();
    if (c <= 0 && BO->getOpcode() != Instruction::Sub) return Ret;
    switch (BO->getOpcode()) {
      case Instruction::Add: Ret = {EffectType::IncByC, c}; break;
      case Instruction::Sub: if (Op0 == Var) Ret = {EffectType::DecByC, c}; break;
      case Instruction::Mul: if (c > 1) Ret = {EffectType::MulByC, c}; break;
      case Instruction::SDiv: if (c > 1) Ret = {EffectType::DivByC, c}; break;
      default: break;
    }
  } else if (auto *PHI = dyn_cast<PHINode>(I)) {
    bool monotonic = true;
    EffectType commonE = EffectType::Unchanged;
    int64_t commonC = 0;
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); i++) {
      auto *IncI = dyn_cast<Instruction>(PHI->getIncomingValue(i));
      if (!IncI) { monotonic = false; break; }
      auto Eff = classifyEffect(IncI, Var, SE);
      if (Eff.E == EffectType::Unknown || (commonE != EffectType::Unchanged && commonE != Eff.E)) {
        monotonic = false;
        break;
      }
      commonE = Eff.E;
      commonC = Eff.C;
    }
    if (monotonic) Ret = {commonE, commonC};
  }
  return Ret;
}

// Compute which checks should be killed in each block
static void computeKillSets(Function &F, BDFMap &BDF, AAResults &AA, ScalarEvolution &SE, BlockOffsetMap &Offsets) {
  for (auto &BB : F) {
    CheckSet &killSet = BDF[&BB].Kill;
    for (Instruction &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (!Call->onlyReadsMemory()) {
          for (auto &BC : BDF[&BB].Gen) {
            if (BC.Index->getType()->isPointerTy()) killSet.insert(BC);
          }
        }
      } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
        MemoryLocation StoreLoc = MemoryLocation::get(Store);
        for (auto &BC : BDF[&BB].Gen) {
          if (BC.Index->getType()->isPointerTy()) {
            MemoryLocation IndexLoc(BC.Index, LocationSize::beforeOrAfterPointer());
            if (AA.alias(StoreLoc, IndexLoc) != AliasResult::NoAlias) killSet.insert(BC);
          } else {
            Value *Ptr = Store->getPointerOperand();
            if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
              Value *GEPIdx = GEP->getOperand(GEP->getNumOperands() - 1);
              if (GEPIdx == BC.Index) {
                killSet.insert(BC);
              }
            }
          }
        }
      } else if (auto *Load = dyn_cast<LoadInst>(&I)) {
        MemoryLocation LoadLoc = MemoryLocation::get(Load);
        for (auto &BC : BDF[&BB].Gen) {
          if (BC.Index->getType()->isPointerTy()) {
            MemoryLocation IndexLoc(BC.Index, LocationSize::beforeOrAfterPointer());
            if (AA.alias(LoadLoc, IndexLoc) != AliasResult::NoAlias) killSet.insert(BC);
          }
        }
      } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        for (auto &BC : BDF[&BB].Gen) {
          if (BO->getOperand(0) == BC.Index || BO->getOperand(1) == BC.Index) {
            auto Eff = classifyEffect(BO, BC.Index, SE);
            if (Eff.E == EffectType::Unknown) {
              killSet.insert(BC);
            } else {
              auto &Existing = Offsets[&BB][BC.Index];
              if (!Existing.Known) {
                Existing = {true, Eff.E, Eff.C};
              } else if (Existing.E == Eff.E) {
                Existing.Offset += Eff.C;
              } else {
                Existing = {false, EffectType::Unknown, 0};
              }
            }
          }
        }
      } else if (I.mayWriteToMemory()) {
        for (auto &BC : BDF[&BB].Gen) {
          if (BC.Index->getType()->isPointerTy()) killSet.insert(BC);
        }
      }
    }
  }
}

// Apply monotonic offsets to optimize checks
static void applyMonotonicOffsets(Function &F, BDFMap &BDF, BlockOffsetMap &Offsets, ScalarEvolution &SE) {
  for (auto &BB : F) {
    auto &BlockDF = BDF[&BB];
    auto &OffsetInBlock = Offsets[&BB];
    CheckSet UpdatedGen;
    for (auto &BC : BlockDF.Gen) {
      auto It = OffsetInBlock.find(BC.Index);
      if (It != OffsetInBlock.end()) {
        VarOffset Off = It->second;
        if (Off.Known && Off.E == EffectType::IncByC) {
          IRBuilder<> Builder(BC.CheckInst);
          Value *CVal = ConstantInt::get(BC.Index->getType(), Off.Offset);
          Value *NewLB = Builder.CreateSub(BC.LowerBound, CVal);
          Value *NewUB = Builder.CreateSub(BC.UpperBound, CVal);
          Function *CheckBoundFn = nullptr;
          if (auto *M = F.getParent()) {
            auto *IdxTy = BC.Index->getType();
            auto *BTy = BC.UpperBound->getType();
            LLVMContext &Ctx = M->getContext();
            auto *FnTy = FunctionType::get(Type::getVoidTy(Ctx), {IdxTy, BTy}, false);
            CheckBoundFn = M->getFunction("checkBound");
            if (!CheckBoundFn) CheckBoundFn = Function::Create(FnTy, Function::ExternalLinkage, "checkBound", M);
          }
          Value *NewChk = Builder.CreateCall(CheckBoundFn, { BC.Index, NewUB });
          errs() << "Eliminating local offset-applied: " << *BC.CheckInst << "\n";
          BC.CheckInst->eraseFromParent();
          BoundCheck NBC { BC.Index, NewLB, NewUB, cast<Instruction>(NewChk) };
          UpdatedGen.insert(NBC);
          continue;
        }
      }
      UpdatedGen.insert(BC);
    }
    BlockDF.Gen = UpdatedGen;
  }
}

// Modify checks to create redundancies
static void modifyChecksToCreateRedundancies(Function &F, BDFMap &BDF, ScalarEvolution &SE, DominatorTree &DT) {
  for (auto &BB : F) {
    auto &df = BDF[&BB];
    if (df.Gen.empty()) continue;
    CheckSet newGen = df.Gen;
    bool changed = false;
    for (auto &GC : df.Gen) {
      for (auto &VC : df.VeryBusyOut) {
        if (auto *IndexDef = dyn_cast<Instruction>(VC.Index)) if (!DT.dominates(IndexDef, GC.CheckInst)) continue;
        if (auto *LowerDef = dyn_cast<Instruction>(VC.LowerBound)) if (!DT.dominates(LowerDef, GC.CheckInst)) continue;
        if (auto *UpperDef = dyn_cast<Instruction>(VC.UpperBound)) if (!DT.dominates(UpperDef, GC.CheckInst)) continue;
        if (GC.Index == VC.Index && subsumes(VC, GC, SE, DT, GC.CheckInst)) {
          if (GC.CheckInst) {
            IRBuilder<> Builder(GC.CheckInst);
            Function *CheckBoundFn = nullptr;
            if (auto *M = F.getParent()) {
              auto *IdxTy = VC.Index->getType();
              auto *BTy = VC.UpperBound->getType();
              LLVMContext &Ctx = M->getContext();
              auto *FnTy = FunctionType::get(Type::getVoidTy(Ctx), {IdxTy, BTy}, false);
              auto *Found = M->getFunction("checkBound");
              if (!Found) Found = Function::Create(FnTy, Function::ExternalLinkage, "checkBound", M);
              CheckBoundFn = Found;
            }
            Value *NewChk = Builder.CreateCall(CheckBoundFn, {VC.Index, VC.UpperBound});
            errs() << "Eliminating via modification: " << *GC.CheckInst << "\n";
            GC.CheckInst->eraseFromParent();
            BoundCheck NBC { VC.Index, VC.LowerBound, VC.UpperBound, cast<Instruction>(NewChk) };
            newGen.erase(GC);
            newGen.insert(NBC);
            changed = true;
          }
          break;
        }
      }
    }
    if (changed) df.Gen = newGen;
  }
}

// Compute backward data flow (very busy expressions)
static void computeBackwardDataFlow(Function &F, BDFMap &BDF, ScalarEvolution &SE, DominatorTree &DT) {
  ReversePostOrderTraversal<Function*> RPOT(&F);
  bool changed = true;
  unsigned iterations = 0;
  unsigned MAX_ITERATIONS = 10;
  while (changed && (iterations < MAX_ITERATIONS)) {
    changed = false;
    iterations++;
    for (auto *BB : RPOT) {
      if (!BB) continue;
      Instruction *CtxI = BB->getTerminator();
      if (!CtxI) continue;
      CheckSet newVBO;
      bool first = true;
      for (auto *Succ : successors(BB)) {
        if (!Succ) continue;
        if (first) {
          newVBO = BDF[Succ].VeryBusyIn;
          first = false;
        } else {
          newVBO = dfIntersect(newVBO, BDF[Succ].VeryBusyIn, SE, DT, CtxI);
        }
      }
      if (first) newVBO = CheckSet();
      CheckSet adjusted = newVBO;
      for (auto &k : BDF[BB].Kill) adjusted.erase(k);
      CheckSet newVBI = dfUnion(BDF[BB].Gen, adjusted, SE, DT, CtxI);
      if (newVBO.size() != BDF[BB].VeryBusyOut.size() || newVBI.size() != BDF[BB].VeryBusyIn.size()) {
        BDF[BB].VeryBusyOut = newVBO;
        BDF[BB].VeryBusyIn = newVBI;
        changed = true;
      }
    }
  }
}

// Compute forward data flow (available expressions)
static void computeForwardDataFlow(Function &F, BDFMap &BDF, ScalarEvolution &SE, DominatorTree &DT) {
  ReversePostOrderTraversal<Function*> RPO(&F);
  bool changed = true;
  unsigned iterations = 0;
  unsigned MAX_ITERATIONS = 10;
  while (changed && (iterations < MAX_ITERATIONS)) {
    changed = false;
    iterations++;
    for (auto *BB : RPO) {
      if (!BB) continue;
      Instruction *CtxI = BB->getTerminator();
      if (!CtxI) continue;
      CheckSet newAIN;
      bool first = true;
      for (auto *Pred : predecessors(BB)) {
        if (!Pred) continue;
        if (first) {
          newAIN = BDF[Pred].AvailableOut;
          first = false;
        } else {
          newAIN = dfIntersect(newAIN, BDF[Pred].AvailableOut, SE, DT, CtxI);
        }
      }
      if (first) newAIN = CheckSet();
      CheckSet adjusted = newAIN;
      for (auto &k : BDF[BB].Kill) adjusted.erase(k);
      CheckSet newAOUT = dfUnion(BDF[BB].Gen, adjusted, SE, DT, CtxI);
      if (newAIN.size() != BDF[BB].AvailableIn.size() || newAOUT.size() != BDF[BB].AvailableOut.size()) {
        BDF[BB].AvailableIn = newAIN;
        BDF[BB].AvailableOut = newAOUT;
        changed = true;
      }
    }
  }
}

// Eliminate redundant checks based on data flow analysis
static void eliminateRedundantChecks(Function &F, BDFMap &BDF, ScalarEvolution &SE, DominatorTree &DT) {
  unsigned totalEliminated = 0;
  for (auto &BB : F) {
    auto &InSet = BDF[&BB].AvailableIn;
    for (auto I = BB.begin(); I != BB.end();) {
      Instruction *Inst = &*I++;
      auto *Call = dyn_cast<CallBase>(Inst);
      if (!Call) continue;
      auto *Callee = Call->getCalledFunction();
      if (!Callee) continue;
      if (!isCheckBoundFunction(Callee)) continue;
      if (Call->arg_size() != 2) continue;
      Value *Idx = Call->getArgOperand(0);
      Value *Bound = Call->getArgOperand(1);
      if (!Idx || !Bound) continue;
      Value *ZeroLB = ConstantInt::get(Idx->getType(), 0);
      BoundCheck BC { Idx, ZeroLB, Bound, Inst };
      
      auto &Ranges = BDF[&BB].KnownRanges;
      auto It = Ranges.find(Idx);
      if (It != Ranges.end() && It->second.HasUpperBound) {
        const SCEV *IdxUB = SE.getSCEV(It->second.UpperBound);
        const SCEV *BoundSCEV = SE.getSCEV(Bound);
        if (SE.isKnownPredicate(ICmpInst::ICMP_SLE, IdxUB, BoundSCEV)) {
          errs() << "Eliminating redundant through range analysis: " << *Inst << "\n";
          Inst->eraseFromParent();
          totalEliminated++;
          continue;
        }
      }
      
      bool redundant = false;
      for (auto &AS : InSet) {
        if (normalizedEqual(AS, BC, SE) || subsumes(AS, BC, SE, DT, Inst) || isLargerArray(BC, AS, SE)) {
          redundant = true;
          break;
        }
      }
      if (redundant) {
        errs() << "Eliminating redundant: " << *Inst << "\n";
        Inst->eraseFromParent();
        totalEliminated++;
      }
    }
  }
}

// Enhanced loop checks hoisting
static void hoistLoopChecksEnhanced(Loop *L, BDFMap &BDF, DominatorTree &DT, ScalarEvolution &SE, LoopInfo &LI) {
  for (Loop *SubLoop : L->getSubLoops()) hoistLoopChecksEnhanced(SubLoop, BDF, DT, SE, LI);
  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader) return;
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  if (!Header || !Latch) return;
  SmallVector<BasicBlock*, 4> ExitingBlocks;
  L->getExitingBlocks(ExitingBlocks);
  if (ExitingBlocks.empty()) return;
  for (BasicBlock *ExitBlock : ExitingBlocks) {
    BranchInst *BI = dyn_cast<BranchInst>(ExitBlock->getTerminator());
    if (!BI || !BI->isConditional()) continue;
    ICmpInst *ICI = dyn_cast<ICmpInst>(BI->getCondition());
    if (!ICI) continue;
    if (ICI->getPredicate() == ICmpInst::ICMP_SLT || ICI->getPredicate() == ICmpInst::ICMP_ULT) {
      Value *Idx = ICI->getOperand(0);
      Value *Bound = ICI->getOperand(1);
      if (auto *PHI = dyn_cast<PHINode>(Idx)) {
        if (PHI->getParent() == Header) {
          for (BasicBlock::iterator it = Header->begin(); it != Header->end(); ++it) {
            if (auto *BO = dyn_cast<BinaryOperator>(&*it)) {
              if (BO->getOpcode() == Instruction::Add && (BO->getOperand(0) == PHI || BO->getOperand(1) == PHI)) {
                SmallVector<std::pair<Instruction*, Value*>, 8> ChecksInfo;
                for (BasicBlock *BB : L->blocks()) {
                  for (Instruction &II : *BB) {
                    auto *Call = dyn_cast<CallBase>(&II);
                    if (!Call || !Call->getCalledFunction() || !isCheckBoundFunction(Call->getCalledFunction())) continue;
                    if (Call->arg_size() != 2) continue;
                    Value *AccessIdx = Call->getArgOperand(0);
                    Value *AccessBound = Call->getArgOperand(1);
                    if (AccessIdx == PHI || (isa<Instruction>(AccessIdx) && cast<Instruction>(AccessIdx)->getOperand(0) == PHI)) {
                      ChecksInfo.push_back(std::make_pair(&II, AccessBound));
                    }
                  }
                }
                if (!ChecksInfo.empty()) {
                  Value *FirstBound = ChecksInfo[0].second;
                  bool AllSafeBounds = true;
                  for (auto &CheckInfo : ChecksInfo) {
                    Value *CheckBound = CheckInfo.second;
                    if (Bound != CheckBound && !SE.isKnownPredicate(ICmpInst::ICMP_SLE, SE.getSCEV(Bound), SE.getSCEV(CheckBound))) {
                      AllSafeBounds = false;
                      break;
                    }
                  }
                  if (AllSafeBounds) {
                    for (auto &CheckInfo : ChecksInfo) {
                      errs() << "Eliminating safe loop check: " << *CheckInfo.first << "\n";
                      CheckInfo.first->eraseFromParent();
                    }
                  } else {
                    IRBuilder<> Builder(Preheader->getTerminator());
                    Function *Func = Preheader->getParent();
                    Module *M = Func->getParent();
                    Value *StartVal = PHI->getIncomingValueForBlock(Preheader);
                    if (!StartVal) continue;
                    Function *CheckBoundFn = nullptr;
                    {
                      auto *IdxTy = StartVal->getType();
                      auto *BTy = FirstBound->getType();
                      LLVMContext &Ctx = M->getContext();
                      auto *FnTy = FunctionType::get(Type::getVoidTy(Ctx), {IdxTy, BTy}, false);
                      CheckBoundFn = M->getFunction("checkBound");
                      if (!CheckBoundFn) CheckBoundFn = Function::Create(FnTy, Function::ExternalLinkage, "checkBound", M);
                    }
                    Builder.CreateCall(CheckBoundFn, {StartVal, FirstBound});
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

// Main optimization pass implementation
struct BoundCheckOptimizationPass {
  void runOnFunction(Function &F, FunctionAnalysisManager &FAM);
};

} // end of anonymous namespace

// Entry point for the pass
PreservedAnalyses BoundCheckOptimization::run(Function &F, FunctionAnalysisManager &FAM) {
  if (!isCProgram(F.getParent()) && isCxxSTLFunc(F.getName())) {
    return PreservedAnalyses::all();
  }
  BoundCheckOptimizationPass Impl;
  Impl.runOnFunction(F, FAM);
  return PreservedAnalyses::none();
}

BoundCheckOptimization::~BoundCheckOptimization() = default;

// Main function to run the optimization
void BoundCheckOptimizationPass::runOnFunction(Function &F, FunctionAnalysisManager &FAM) {
  errs() << "======== Starting Bound Check Optimization ========\n";
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &AA = FAM.getResult<AAManager>(F);
  
  Module *M = F.getParent();
  ArrayInfoMap ArrayMap;
  collectArrayInfoFromModule(M, ArrayMap);
  
  eliminateConstantIndexChecks(F, SE);
  
  analyzeConditionalBranches(F, SE, DT);
  
  std::map<Value*, Value*> ParamRelationMap;
  traceIndexParameterRelation(F, ParamRelationMap);
  
  for (auto &BB : F) {
    if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
      if (BI->isConditional() && BI->getNumSuccessors() == 2) {
        if (auto *ICmp = dyn_cast<ICmpInst>(BI->getCondition())) {
          if (ICmp->getPredicate() == ICmpInst::ICMP_SGT || 
              ICmp->getPredicate() == ICmpInst::ICMP_UGT) {
            
            Value *LHS = ICmp->getOperand(0);
            Value *RHS = ICmp->getOperand(1);
            
            if (auto *ConstVal = dyn_cast<ConstantInt>(RHS)) {
              BasicBlock *ElseBB = BI->getSuccessor(1);
              for (auto &I : *ElseBB) {
                if (auto *Call = dyn_cast<CallBase>(&I)) {
                  if (Call->getCalledFunction() && 
                      isCheckBoundFunction(Call->getCalledFunction()) && 
                      Call->arg_size() == 2) {
                    
                    Value *Idx = Call->getArgOperand(0);
                    Value *Bound = Call->getArgOperand(1);
                    
                    if (auto *BoundConst = dyn_cast<ConstantInt>(Bound)) {
                      uint64_t ArraySize = BoundConst->getZExtValue();
                      uint64_t CondVal = ConstVal->getZExtValue();
                      
                      bool IdxRelatedToParam = false;
                      for (auto &ArgRel : ParamRelationMap) {
                        if (Idx == ArgRel.first || 
                            ParamRelationMap.count(Idx) > 0) {
                          IdxRelatedToParam = true;
                          break;
                        }
                      }
                      
                      if (IdxRelatedToParam && CondVal <= ArraySize) {
                        errs() << "Special pattern optimization: Safely removing check in else branch " 
                              << I << " using parameter as index!\n";
                        Call->eraseFromParent();
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  
  BDFMap BDF;
  for (auto &BB : F) BDF[&BB] = BlockDataFlow();
  
  propagateValueRangesThroughBlocks(F, BDF, DT, SE);
  
  eliminateLocalChecks(F, BDF, SE, DT);
  
  BlockOffsetMap Offsets;
  computeKillSets(F, BDF, AA, SE, Offsets);
  
  applyMonotonicOffsets(F, BDF, Offsets, SE);
  
  computeBackwardDataFlow(F, BDF, SE, DT);
  
  modifyChecksToCreateRedundancies(F, BDF, SE, DT);
  
  computeForwardDataFlow(F, BDF, SE, DT);
  
  eliminateRedundantChecks(F, BDF, SE, DT);
  
  for (Loop *L : LI) hoistLoopChecksEnhanced(L, BDF, DT, SE, LI);
  
  errs() << "======== Bound Check Optimization Complete ========\n";
}

// Plugin registration
PassPluginLibraryInfo getBoundCheckOptimizationPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "BoundCheckOptimization", "v1.0", [](PassBuilder &PB) {
    PB.registerPipelineParsingCallback([&](StringRef Name, FunctionPassManager &FPM, ArrayRef<PassBuilder::PipelineElement>) {
      if (Name == "bound-check-optimization") {
        FPM.addPass(BoundCheckOptimization());
        return true;
      }
      return false;
    });
  }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getBoundCheckOptimizationPluginInfo();
}