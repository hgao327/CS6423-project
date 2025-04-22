#include "BoundCheckInsertion.h"
#include "CommonDef.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

PreservedAnalyses BoundCheckInsertion::run(Function &F, FunctionAnalysisManager &FAM) {
  llvm::errs() << "BoundCheckInsertion running on: " << F.getName() << "\n";
  // Skip if not a C program or is a C++ STL function
  if (!isCProgram(F.getParent()) && isCxxSTLFunc(F.getName())) {
    return PreservedAnalyses::all();
  }

  // Calculate total offset for compound GEP (for constant folding cases)
  auto calculateConstantOffset = [](GetElementPtrInst *GEP) -> std::pair<bool, int64_t> {
    // Check if this is a static global array access pattern: array[constant_offset]
    if (GEP->getNumOperands() < 3) return std::make_pair(false, 0);
    
    // Check if the first operand is a global array
    Value *basePtr = GEP->getOperand(0);
    if (!isa<GlobalVariable>(basePtr)) return std::make_pair(false, 0);
    
    // Try to calculate total offset
    int64_t totalOffset = 0;
    
    // Check if all GEP indices are constants
    bool allConstant = true;
    for (unsigned i = 1; i < GEP->getNumOperands(); i++) {
      Value *idx = GEP->getOperand(i);
      if (auto *CI = dyn_cast<ConstantInt>(idx)) {
        // For multi-dimensional arrays, calculate offset according to level
        if (i == 1) {
          // For the first index, this is usually a multiple of the array size
          Type *ptrElementType = GEP->getSourceElementType();
          if (ptrElementType->isArrayTy()) {
            llvm::ArrayType *arrType = cast<llvm::ArrayType>(ptrElementType);
            uint64_t arraySize = arrType->getNumElements();
            totalOffset += CI->getSExtValue() * arraySize;
          }
        } else {
          // Other indices are added directly
          totalOffset += CI->getSExtValue();
        }
      } else {
        allConstant = false;
        break;
      }
    }
    
    return std::make_pair(allConstant, totalOffset);
  };

  // Helper function: Extract the correct index from GEP instruction (enhanced version)
  auto extractIndexFromGEP = [&calculateConstantOffset](GetElementPtrInst *GEP, StringRef ArrayType) -> std::pair<Value*, bool> {
    errs() << "Analyzing GEP: " << *GEP << "\n";
    
    // First check if this is a constant folded access to a global array
    std::pair<bool, int64_t> constOffsetResult = calculateConstantOffset(GEP);
    bool isConstantOffset = constOffsetResult.first;
    int64_t offset = constOffsetResult.second;
    
    if (isConstantOffset) {
      errs() << "Detected constant folded access with offset: " << offset << "\n";
      // Create a constant value for the offset
      LLVMContext &Ctx = GEP->getContext();
      Type *IndexType = Type::getInt64Ty(Ctx);
      if (GEP->getOperand(1)->getType()->isIntegerTy()) {
        IndexType = GEP->getOperand(1)->getType();
      }
      Value *offsetValue = ConstantInt::get(IndexType, offset);
      return std::make_pair(offsetValue, true); // Second parameter indicates constant offset
    }
    
    // Get index based on array type
    if (ArrayType == "static array" || ArrayType == "static global array" || 
        ArrayType == "1d" || ArrayType == "1D") {
      // For static arrays, we typically focus on the last index
      if (GEP->getNumOperands() >= 3) { // base pointer + 0 + actual index
        // The last operand is typically the array index
        return std::make_pair(GEP->getOperand(GEP->getNumOperands() - 1), false);
      }
    } else if (ArrayType == "2d" || ArrayType == "2D") {
      // For 2D arrays, we may need to extract different indices
      if (GEP->getNumOperands() >= 4) { // base pointer + 0 + row index + column index
        // For simplicity, choose the column index (usually the last operand)
        return std::make_pair(GEP->getOperand(GEP->getNumOperands() - 1), false);
      }
    } else if (ArrayType == "dynamic array") {
      // For dynamic arrays, the index is typically the second operand
      if (GEP->getNumOperands() >= 2) {
        return std::make_pair(GEP->getOperand(1), false);
      }
    } else {
      // Default case: try to use the last operand as the index
      if (GEP->getNumOperands() >= 2) {
        unsigned lastIndex = GEP->getNumOperands() - 1;
        return std::make_pair(GEP->getOperand(lastIndex), false);
      }
    }
    
    return std::make_pair(nullptr, false);
  };

  // Iterate through all instructions in the function
  for (auto &BB : F) {
    for (auto &I : BB) {
      // 1) Check if the instruction has the metadata we need (ACCESS_KEY)
      MDNode *MN = I.getMetadata(ACCESS_KEY);
      if (!MN) {
        // Skip if no metadata
        continue;
      }
      errs() << "Found metadata for: " << I << "\n";

      // 2) Get array type and bound from metadata
      //    operand(0) is a string ("static array"/"dynamic array")
      //    operand(1) is the Value representing array bound
      StringRef ArrayType = cast<MDString>(MN->getOperand(0).get())->getString();
      Value *BoundVal = cast<ValueAsMetadata>(MN->getOperand(1).get())->getValue();
      
      errs() << "Array Type: " << ArrayType << ", Bound: " << *BoundVal << "\n";

      // Dynamic arrays may have a 3rd operand
      CallBase *Allocator = nullptr;
      if (MN->getNumOperands() > 2) {
        Allocator = dyn_cast<CallBase>(
          cast<ValueAsMetadata>(MN->getOperand(2).get())->getValue()
        );
        if (Allocator) {
          errs() << "Allocator Call: " << *Allocator << "\n";
        }
      }
      
      // 3) Find the "index" for this access
      Value *IndexVal = nullptr;
      bool isConstantOffset = false;

      // Process GEP instruction directly
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        std::pair<Value*, bool> indexResult = extractIndexFromGEP(GEP, ArrayType);
        IndexVal = indexResult.first;
        isConstantOffset = indexResult.second;
      } 
      // Process Load instruction - with special handling for GEP operands with constant offsets
      else if (auto *loadI = dyn_cast<LoadInst>(&I)) {
        Value *ptr = loadI->getPointerOperand();
        
        // 1. First check if it's a direct GEP instruction
        if (auto *GEP = dyn_cast<GetElementPtrInst>(ptr)) {
          std::pair<Value*, bool> indexResult = extractIndexFromGEP(GEP, ArrayType);
          IndexVal = indexResult.first;
          isConstantOffset = indexResult.second;
        }
        // 2. Check if it's an inline GEP operation
        else if (auto *GEPOp = dyn_cast<GEPOperator>(ptr)) {
          // Handle constant offset cases (e.g., getelementptr inbounds ([1000 x i32], ptr @array, i64 1, i64 2))
          bool allConstIndices = true;
          int64_t totalOffset = 0;
          
          // Check if the first operand is a global array
          if (isa<GlobalVariable>(GEPOp->getPointerOperand())) {
            // Try to calculate total offset
            for (auto it = GEPOp->idx_begin(); it != GEPOp->idx_end(); ++it) {
              if (auto *CI = dyn_cast<ConstantInt>(*it)) {
                // More complex calculation needed for multi-dimensional arrays, simplified here
                if (it == GEPOp->idx_begin()) {
                  // For 1D arrays, first index is typically multiplied by array size
                  Type *srcType = GEPOp->getSourceElementType();
                  if (srcType->isArrayTy()) {
                    uint64_t arraySize = cast<llvm::ArrayType>(srcType)->getNumElements();
                    totalOffset += CI->getSExtValue() * arraySize;
                  }
                } else {
                  // Other indices are added directly
                  totalOffset += CI->getSExtValue();
                }
              } else {
                allConstIndices = false;
                break;
              }
            }
            
            if (allConstIndices) {
              errs() << "Constant offset in Load: " << totalOffset << "\n";
              LLVMContext &Ctx = loadI->getContext();
              Type *IndexType = Type::getInt32Ty(Ctx);
              IndexVal = ConstantInt::get(IndexType, totalOffset);
              isConstantOffset = true;
            }
          }
        }
      } 
      // Process Store instruction
      else if (auto *storeI = dyn_cast<StoreInst>(&I)) {
        Value *ptr = storeI->getPointerOperand()->stripPointerCasts();
        if (auto *GEP = dyn_cast<GetElementPtrInst>(ptr)) {
          std::pair<Value*, bool> indexResult = extractIndexFromGEP(GEP, ArrayType);
          IndexVal = indexResult.first;
          isConstantOffset = indexResult.second;
        }
      } else {
        errs() << "Unsupported instruction: " << I << "\n";
        // Other access forms can be extended later
      }

      // Skip if no index found
      if (!IndexVal) {
        errs() << "No index found\n";
        continue;
      }

      // 4) Insert call checkBound(index, bound) before the current instruction I
      IRBuilder<> IRB(&I);
      errs() << "Found index: " << *IndexVal << "\n";

      // 4a) Ensure index and bound are i32
      LLVMContext &Ctx = F.getContext();
      Type *I32Ty = Type::getInt32Ty(Ctx);

      // Convert index
      Value *IdxI32 = nullptr;
      if (IndexVal->getType()->isIntegerTy(32)) {
        IdxI32 = IndexVal;
      } else if (IndexVal->getType()->isIntegerTy()) {
        IdxI32 = IRB.CreateIntCast(IndexVal, I32Ty, true /*isSigned*/);
      } else {
        errs() << "Index is not an integer type\n";
        continue;
      }

      // Convert bound
      Value *BoundI32 = nullptr;
      if (BoundVal->getType()->isIntegerTy(32)) {
        BoundI32 = BoundVal;
      } else if (BoundVal->getType()->isIntegerTy()) {
        BoundI32 = IRB.CreateIntCast(BoundVal, I32Ty, false /*unsigned*/);
      } else {
        errs() << "Bound is not an integer type\n";
        continue;
      }

      // 4b) Get or declare checkBound function: void checkBound(i32, i32)
      FunctionCallee CheckFn = F.getParent()->getOrInsertFunction(
        "checkBound",
        FunctionType::get(Type::getVoidTy(Ctx), {I32Ty, I32Ty}, false)
      );

      // 4c) Generate call: call void @checkBound(i32 %idx, i32 %bound)
      CallInst *Call = IRB.CreateCall(CheckFn, {IdxI32, BoundI32});
      errs() << "Inserted bound check: " << *Call << "\n";
    }
  }
  return PreservedAnalyses::none();
}

BoundCheckInsertion::~BoundCheckInsertion() {
}

void BoundCheckInsertion::InstrumentationExample() {
}