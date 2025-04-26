#include "optimizer.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cassert>

namespace dbir {

// Helper function to evaluate constant expressions
std::unique_ptr<Constant> evaluateConstantExpression(const Expression& expr) {
    if (expr.type() == ExprType::CONSTANT) {
        return std::unique_ptr<Constant>(dynamic_cast<Constant*>(expr.clone().release()));
    }
    
    if (expr.type() == ExprType::BINARY_OP) {
        const BinaryOp& binop = dynamic_cast<const BinaryOp&>(expr);
        
        if (!binop.left_expr->isConstant() || !binop.right_expr->isConstant()) {
            return nullptr;
        }
        
        auto left_const = evaluateConstantExpression(*binop.left_expr);
        auto right_const = evaluateConstantExpression(*binop.right_expr);
        
        if (!left_const || !right_const) {
            return nullptr;
        }
        
        // Handle different constant types and operations
        if (left_const->value_type == Constant::Type::INTEGER && 
            right_const->value_type == Constant::Type::INTEGER) {
            
            int result;
            switch (binop.op_type) {
                case BinaryOpType::ADD: 
                    result = left_const->int_value + right_const->int_value; 
                    break;
                case BinaryOpType::SUB: 
                    result = left_const->int_value - right_const->int_value; 
                    break;
                case BinaryOpType::MUL: 
                    result = left_const->int_value * right_const->int_value; 
                    break;
                case BinaryOpType::DIV:
                    if (right_const->int_value == 0) return nullptr; // Division by zero
                    result = left_const->int_value / right_const->int_value;
                    break;
                case BinaryOpType::EQ:
                    return std::make_unique<Constant>(left_const->int_value == right_const->int_value);
                case BinaryOpType::NE:
                    return std::make_unique<Constant>(left_const->int_value != right_const->int_value);
                case BinaryOpType::LT:
                    return std::make_unique<Constant>(left_const->int_value < right_const->int_value);
                case BinaryOpType::LE:
                    return std::make_unique<Constant>(left_const->int_value <= right_const->int_value);
                case BinaryOpType::GT:
                    return std::make_unique<Constant>(left_const->int_value > right_const->int_value);
                case BinaryOpType::GE:
                    return std::make_unique<Constant>(left_const->int_value >= right_const->int_value);
                default:
                    return nullptr;
            }
            return std::make_unique<Constant>(result);
        }
        
        // Handle string operations, boolean operations, etc.
        // ...
    }
    
    return nullptr;
}

// ===== Constant Folding =====
class ConstantFoldingVisitor {
public:
    std::unique_ptr<Expression> visitExpression(std::unique_ptr<Expression> expr) {
        if (!expr) return nullptr;
        
        if (expr->isConstant()) {
            return expr;
        }
        
        if (expr->type() == ExprType::BINARY_OP) {
            auto binop = dynamic_cast<BinaryOp*>(expr.get());
            binop->left_expr = visitExpression(std::move(binop->left_expr));
            binop->right_expr = visitExpression(std::move(binop->right_expr));
            
            auto const_result = evaluateConstantExpression(*expr);
            if (const_result) {
                return const_result;
            }
        }
        
        // Handle other expression types...
        
        return expr;
    }
    
    std::unique_ptr<HIRNode> visitNode(std::unique_ptr<HIRNode> node) {
        if (!node) return nullptr;
        
        switch (node->type()) {
            case HIRNodeType::FILTER: {
                auto filter = dynamic_cast<FilterNode*>(node.get());
                filter->child = visitNode(std::move(filter->child));
                filter->condition = visitExpression(std::move(filter->condition));
                
                // If condition is a constant, check if it's always true/false
                if (filter->condition->type() == ExprType::CONSTANT) {
                    auto const_cond = dynamic_cast<Constant*>(filter->condition.get());
                    if (const_cond->value_type == Constant::Type::BOOLEAN) {
                        if (const_cond->bool_value) {
                            // Condition always true, remove filter
                            return std::move(filter->child);
                        } else {
                            // Condition always false, return empty result
                            // For simplicity, we just keep the filter, a real implementation
                            // might replace this with a special "empty" node
                        }
                    }
                }
                break;
            }
            case HIRNodeType::JOIN: {
                auto join = dynamic_cast<JoinNode*>(node.get());
                join->left_child = visitNode(std::move(join->left_child));
                join->right_child = visitNode(std::move(join->right_child));
                join->condition = visitExpression(std::move(join->condition));
                break;
            }
            case HIRNodeType::PROJECT: {
                auto project = dynamic_cast<ProjectNode*>(node.get());
                project->child = visitNode(std::move(project->child));
                for (auto& expr : project->projections) {
                    expr = visitExpression(std::move(expr));
                }
                break;
            }
            // Handle other node types...
        }
        
        return node;
    }
};

std::unique_ptr<HIRNode> foldConstants(std::unique_ptr<HIRNode> root) {
    ConstantFoldingVisitor visitor;
    return visitor.visitNode(std::move(root));
}

// ===== Predicate Pushdown =====
class PredicatePushdownVisitor {
public:
    std::unique_ptr<HIRNode> visit(std::unique_ptr<HIRNode> node, 
                                   std::vector<std::unique_ptr<Expression>>& predicates) {
        if (!node) return nullptr;
        
        switch (node->type()) {
            case HIRNodeType::FILTER: {
                auto filter = dynamic_cast<FilterNode*>(node.get());
                predicates.push_back(std::move(filter->condition));
                
                auto result = visit(std::move(filter->child), predicates);
                return result;
            }
            case HIRNodeType::JOIN: {
                auto join = dynamic_cast<JoinNode*>(node.get());
                
                // Collect predicates from above
                std::vector<std::unique_ptr<Expression>> left_preds;
                std::vector<std::unique_ptr<Expression>> right_preds;
                std::vector<std::unique_ptr<Expression>> join_preds;
                
                // Analyze each predicate to determine where it can be pushed
                for (auto& pred : predicates) {
                    // Logic to determine if predicate references only left tables, only right tables, or both
                    // For simplicity, let's assume we have a helper function for this
                    if (referencesOnlyLeft(*pred, *join)) {
                        left_preds.push_back(pred->clone());
                    } else if (referencesOnlyRight(*pred, *join)) {
                        right_preds.push_back(pred->clone());
                    } else {
                        join_preds.push_back(pred->clone());
                    }
                }
                
                // Add join condition to join predicates
                join_preds.push_back(std::move(join->condition));
                
                // Recursively push predicates down
                auto left = visit(std::move(join->left_child), left_preds);
                auto right = visit(std::move(join->right_child), right_preds);
                
                // Re-construct join with new children
                auto new_join = std::make_unique<JoinNode>(
                    std::move(left),
                    std::move(right),
                    std::move(join_preds[0]), // Use first join pred as condition
                    join->join_type
                );
                
                // Any remaining join predicates need to be applied after the join
                auto result = std::unique_ptr<HIRNode>(std::move(new_join));
                for (size_t i = 1; i < join_preds.size(); ++i) {
                    result = std::make_unique<FilterNode>(
                        std::move(result),
                        std::move(join_preds[i])
                    );
                }
                
                return result;
            }
            case HIRNodeType::SCAN: {
                // Apply all predicates to scan node
                auto result = std::unique_ptr<HIRNode>(node->clone());
                
                // Add all applicable predicates as filters
                for (auto& pred : predicates) {
                    result = std::make_unique<FilterNode>(
                        std::move(result),
                        std::move(pred)
                    );
                }
                
                predicates.clear(); // Predicates have been applied
                return result;
            }
            // Handle other node types...
            default:
                return node; // Just return the node for unhandled types
        }
    }
    
private:
    bool referencesOnlyLeft(const Expression& expr, const JoinNode& join) {
        // Implementation to check if expression references only tables in left subtree
        // ...
        return false; // Placeholder
    }
    
    bool referencesOnlyRight(const Expression& expr, const JoinNode& join) {
        // Implementation to check if expression references only tables in right subtree
        // ...
        return false; // Placeholder
    }
};

std::unique_ptr<HIRNode> pushdownPredicates(std::unique_ptr<HIRNode> root) {
    std::vector<std::unique_ptr<Expression>> predicates;
    PredicatePushdownVisitor visitor;
    return visitor.visit(std::move(root), predicates);
}

// ===== HIR to MIR Conversion =====
class HIRtoMIRConverter {
public:
    HIRtoMIRConverter(std::unordered_map<std::string, size_t> table_stats) 
        : table_statistics(std::move(table_stats)) {}
    
    std::unique_ptr<MIRNode> convert(const HIRNode& node) {
        switch (node.type()) {
            case HIRNodeType::SCAN: {
                const auto& scan = dynamic_cast<const ScanNode&>(node);
                auto it = table_statistics.find(scan.table_name);
                size_t row_estimate = (it != table_statistics.end()) ? it->second : 1000;
                return std::make_unique<ScanOp>(scan.table_name, row_estimate);
            }
            case HIRNodeType::FILTER: {
                const auto& filter = dynamic_cast<const FilterNode&>(node);
                auto child_mir = convert(*filter.child);
                double selectivity = estimateSelectivity(*filter.condition, *child_mir);
                return std::make_unique<FilterOp>(
                    std::move(child_mir),
                    filter.condition->clone(),
                    selectivity
                );
            }
            case HIRNodeType::JOIN: {
                const auto& join = dynamic_cast<const JoinNode&>(node);
                auto left_mir = convert(*join.left_child);
                auto right_mir = convert(*join.right_child);
                
                // Decide between hash join and nested loop based on table sizes
                if (left_mir->estimatedRows() > 100 && right_mir->estimatedRows() > 100) {
                    double join_sel = estimateJoinSelectivity(*join.condition, *left_mir, *right_mir);
                    return std::make_unique<HashJoinOp>(
                        std::move(left_mir),
                        std::move(right_mir),
                        join.condition->clone(),
                        join_sel
                    );
                } else {
                    // Implement NestedLoopJoinOp here
                    // ...
                    return std::make_unique<HashJoinOp>(
                        std::move(left_mir),
                        std::move(right_mir),
                        join.condition->clone(),
                        0.1 // default selectivity
                    );
                }
            }
            // Other node types...
            default:
                return nullptr; // Unhandled node type
        }
    }
    
private:
    std::unordered_map<std::string, size_t> table_statistics;
    
    double estimateSelectivity(const Expression& condition, const MIRNode& input) {
        // Implementation to estimate filter selectivity
        // For simplicity, just return a default value
        return 0.1;
    }
    
    double estimateJoinSelectivity(const Expression& condition, 
                                  const MIRNode& left, 
                                  const MIRNode& right) {
        // Implementation to estimate join selectivity
        // For simplicity, just return a default value
        return 0.1;
    }
};

std::unique_ptr<MIRNode> convertHIRtoMIR(const HIRNode& hir) {
    // Initialize with some default table statistics
    std::unordered_map<std::string, size_t> table_stats = {
        {"orders", 1000000},
        {"customers", 10000},
        {"lineitem", 6000000}
    };
    
    HIRtoMIRConverter converter(table_stats);
    return converter.convert(hir);
}

// ===== MIR Optimizations =====

// Join Reordering
std::unique_ptr<MIRNode> reorderJoins(std::unique_ptr<MIRNode> root) {
    // Simple heuristic: make smaller table the build side of hash joins
    if (root->type() == MIRNodeType::HASH_JOIN) {
        auto join = dynamic_cast<HashJoinOp*>(root.get());
        
        // Recursively reorder joins in children
        join->build_side = reorderJoins(std::move(join->build_side));
        join->probe_side = reorderJoins(std::move(join->probe_side));
        
        // If build side is larger than probe side, swap them
        if (join->build_side->estimatedRows() > join->probe_side->estimatedRows()) {
            std::swap(join->build_side, join->probe_side);
            // Adjust join condition if necessary
            // ...
        }
    }
    
    return root;
}

// Operator Fusion
std::unique_ptr<MIRNode> fuseOperators(std::unique_ptr<MIRNode> root) {
    // TODO: Implement operator fusion for Filter+Project
    return root;
}

// Combined MIR optimization
std::unique_ptr<MIRNode> optimizeMIR(std::unique_ptr<MIRNode> root) {
    root = reorderJoins(std::move(root));
    root = fuseOperators(std::move(root));
    return root;
}

// Combined HIR optimization
std::unique_ptr<HIRNode> optimizeHIR(std::unique_ptr<HIRNode> root) {
    root = foldConstants(std::move(root));
    root = pushdownPredicates(std::move(root));
    root = eliminateUnreachableExpressions(std::move(root));
    return root;
}

// Unreachable expression elimination
std::unique_ptr<HIRNode> eliminateUnreachableExpressions(std::unique_ptr<HIRNode> root) {
    // TODO: Implement dataflow analysis to identify and eliminate unreachable expressions
    return root;
}

} // namespace dbir