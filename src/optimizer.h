#pragma once
#include "ir.h"

namespace dbir {

// HIR Optimizations
std::unique_ptr<HIRNode> optimizeHIR(std::unique_ptr<HIRNode> root);
std::unique_ptr<HIRNode> pushdownPredicates(std::unique_ptr<HIRNode> root);
std::unique_ptr<HIRNode> foldConstants(std::unique_ptr<HIRNode> root);
std::unique_ptr<HIRNode> eliminateUnreachableExpressions(std::unique_ptr<HIRNode> root);

// HIR to MIR Conversion
std::unique_ptr<MIRNode> convertHIRtoMIR(const HIRNode& hir);

// MIR Optimizations
std::unique_ptr<MIRNode> optimizeMIR(std::unique_ptr<MIRNode> root);
std::unique_ptr<MIRNode> reorderJoins(std::unique_ptr<MIRNode> root);
std::unique_ptr<MIRNode> fuseOperators(std::unique_ptr<MIRNode> root);

} // namespace dbir