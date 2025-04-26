#include "codegen.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>
#include <iostream>

namespace dbir {

CodeGenerator::CodeGenerator() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("DBIRModule", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);
    
    auto jit_builder = llvm::orc::LLJITBuilder();
    auto jit_or_error = jit_builder.create();
    if (auto err = jit_or_error.takeError()) {
        llvm::errs() << "JIT creation failed: " << err << "\n";
        return;
    }
    jit = std::move(*jit_or_error);
}

CodeGenerator::~CodeGenerator() {
    // LLVM cleanup happens automatically
}

void* CodeGenerator::generateAndCompile(const MIRNode& root) {
    llvm::Function* func = generateFunction(root);
    if (!func) {
        std::cerr << "Failed to generate function" << std::endl;
        return nullptr;
    }
    
    // JIT compile the function
    auto error = jit->addIRModule(llvm::orc::ThreadSafeModule(
        std::move(module), std::move(context)));
    
    if (error) {
        std::cerr << "Error adding module to JIT: " << error << std::endl;
        return nullptr;
    }
    
    auto symbol = jit->lookup(func->getName().str());
    if (!symbol) {
        std::cerr << "Symbol not found: " << symbol.takeError() << std::endl;
        return nullptr;
    }
    
    return reinterpret_cast<void*>(symbol->getAddress());
}

llvm::Function* CodeGenerator::generateFunction(const MIRNode& node) {
    switch (node.type()) {
        case MIRNodeType::SCAN:
            return generateScanOp(dynamic_cast<const ScanOp&>(node));
        case MIRNodeType::FILTER:
            return generateFilterOp(dynamic_cast<const FilterOp&>(node));
        case MIRNodeType::HASH_JOIN:
            return generateHashJoinOp(dynamic_cast<const HashJoinOp&>(node));
        default:
            std::cerr << "Unsupported node type for code generation" << std::endl;
            return nullptr;
    }
}

llvm::Function* CodeGenerator::generateScanOp(const ScanOp& scan) {
    // Create function prototype for scan operator
    auto fn_type = llvm::FunctionType::get(
        llvm::Type::getInt8PtrTy(*context),  // Return type: pointer to result set
        { llvm::Type::getInt8PtrTy(*context) }, // Args: pointer to SQLite db
        false
    );
    
    auto func = llvm::Function::Create(
        fn_type,
        llvm::Function::ExternalLinkage,
        "scan_" + scan.table_name,
        *module
    );
    
    // Create entry basic block
    auto entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);
    builder->CreateRet(llvm::ConstantPointerNull::get(
        llvm::Type::getInt8PtrTy(*context)));
    
    return func;
}

llvm::Function* CodeGenerator::generateFilterOp(const FilterOp& filter) {
    // Generate function for child operator
    auto child_func = generateFunction(*filter.child);
    if (!child_func) return nullptr;
    
    // Create function prototype for filter operator
    auto fn_type = llvm::FunctionType::get(
        llvm::Type::getInt8PtrTy(*context),  // Return type: pointer to result set
        { llvm::Type::getInt8PtrTy(*context) }, // Args: pointer to SQLite db
        false
    );
    
    auto func = llvm::Function::Create(
        fn_type,
        llvm::Function::ExternalLinkage,
        "filter",
        *module
    );
    
    // Create entry basic block
    auto entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);
    
    // Call child function to get input rows
    auto db_arg = func->arg_begin();
    auto child_result = builder->CreateCall(child_func, { db_arg });
    
    // Return result set
    builder->CreateRet(llvm::ConstantPointerNull::get(
        llvm::Type::getInt8PtrTy(*context)));
    
    return func;
}

llvm::Function* CodeGenerator::generateHashJoinOp(const HashJoinOp& join) {
    // Generate functions for build and probe sides
    auto build_func = generateFunction(*join.build_side);
    auto probe_func = generateFunction(*join.probe_side);
    
    if (!build_func || !probe_func) return nullptr;
    
    // Create function prototype for hash join operator
    auto fn_type = llvm::FunctionType::get(
        llvm::Type::getInt8PtrTy(*context),  // Return type: pointer to result set
        { llvm::Type::getInt8PtrTy(*context) }, // Args: pointer to SQLite db
        false
    );
    
    auto func = llvm::Function::Create(
        fn_type,
        llvm::Function::ExternalLinkage,
        "hash_join",
        *module
    );
    
    // Create entry basic block
    auto entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);
    
    // Call build function to get build-side rows
    auto db_arg = func->arg_begin();
    auto build_result = builder->CreateCall(build_func, { db_arg });
    // Call probe function to get probe-side rows
    auto probe_result = builder->CreateCall(probe_func, { db_arg });
    
    // Return result set
    builder->CreateRet(llvm::ConstantPointerNull::get(
        llvm::Type::getInt8PtrTy(*context)));
    
    return func;
}

llvm::Value* CodeGenerator::generateExpression(const Expression& expr) {
    // Expression code generation based on expression type
    switch (expr.type()) {
        case ExprType::CONSTANT: {
            const auto& constant = dynamic_cast<const Constant&>(expr);
            if (constant.value_type == Constant::Type::INTEGER) {
                return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(*context), 
                    constant.int_value
                );
            } else if (constant.value_type == Constant::Type::FLOAT) {
                return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(*context), 
                    constant.float_value
                );
            } else if (constant.value_type == Constant::Type::BOOLEAN) {
                return llvm::ConstantInt::get(
                    llvm::Type::getInt1Ty(*context), 
                    constant.bool_value
                );
            }
            // Handle other constant types...
            break;
        }
        case ExprType::BINARY_OP: {
            const auto& binop = dynamic_cast<const BinaryOp&>(expr);
            auto left_val = generateExpression(*binop.left_expr);
            auto right_val = generateExpression(*binop.right_expr);
            
            if (!left_val || !right_val) return nullptr;
            
            // Generate appropriate operation based on operator type
            switch (binop.op_type) {
                case BinaryOpType::ADD:
                    return builder->CreateAdd(left_val, right_val, "add");
                case BinaryOpType::SUB:
                    return builder->CreateSub(left_val, right_val, "sub");
                case BinaryOpType::MUL:
                    return builder->CreateMul(left_val, right_val, "mul");
                case BinaryOpType::DIV:
                    return builder->CreateSDiv(left_val, right_val, "div");
                case BinaryOpType::EQ:
                    return builder->CreateICmpEQ(left_val, right_val, "eq");
                case BinaryOpType::NE:
                    return builder->CreateICmpNE(left_val, right_val, "ne");
                case BinaryOpType::LT:
                    return builder->CreateICmpSLT(left_val, right_val, "lt");
                case BinaryOpType::LE:
                    return builder->CreateICmpSLE(left_val, right_val, "le");
                case BinaryOpType::GT:
                    return builder->CreateICmpSGT(left_val, right_val, "gt");
                case BinaryOpType::GE:
                    return builder->CreateICmpSGE(left_val, right_val, "ge");
                // Handle other operations...
                default:
                    return nullptr;
            }
        }
        default:
            return nullptr;
    }
    
    return nullptr;
}

} // namespace dbir