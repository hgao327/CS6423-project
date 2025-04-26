#pragma once
#include "ir.h"
#include <memory>
#include <string>

// Forward declarations for LLVM
namespace llvm {
class LLVMContext;
class Module;
class IRBuilder;
class Function;
class Value;
namespace orc { class LLJIT; }
}

namespace dbir {

class CodeGenerator {
public:
    CodeGenerator();
    ~CodeGenerator();
    
    void* generateAndCompile(const MIRNode& root);
    
private:
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    
    llvm::Function* generateFunction(const MIRNode& node);
    llvm::Function* generateScanOp(const ScanOp& scan);
    llvm::Function* generateFilterOp(const FilterOp& filter);
    llvm::Function* generateHashJoinOp(const HashJoinOp& join);
    
    llvm::Value* generateExpression(const Expression& expr);
};

} // namespace dbir