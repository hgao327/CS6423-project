#pragma once
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

namespace dbir {

// Forward declarations
class Expression;

// ===== Expression Classes =====
enum class ExprType { COLUMN, CONSTANT, BINARY_OP, UNARY_OP, FUNCTION };

class Expression {
public:
    virtual ~Expression() = default;
    virtual ExprType type() const = 0;
    virtual bool isConstant() const = 0;
    virtual std::string toString() const = 0;
    virtual std::unique_ptr<Expression> clone() const = 0;
};

class ColumnRef : public Expression {
public:
    ColumnRef(std::string table, std::string column) 
        : table_name(std::move(table)), column_name(std::move(column)) {}
    
    ExprType type() const override { return ExprType::COLUMN; }
    bool isConstant() const override { return false; }
    std::string toString() const override { return table_name + "." + column_name; }
    std::unique_ptr<Expression> clone() const override { 
        return std::make_unique<ColumnRef>(table_name, column_name); 
    }
    
    std::string table_name;
    std::string column_name;
};

class Constant : public Expression {
public:
    enum class Type { INTEGER, FLOAT, STRING, BOOLEAN, NULL_VALUE };
    
    Constant(int value) : value_type(Type::INTEGER), int_value(value) {}
    Constant(double value) : value_type(Type::FLOAT), float_value(value) {}
    Constant(std::string value) : value_type(Type::STRING), string_value(std::move(value)) {}
    Constant(bool value) : value_type(Type::BOOLEAN), bool_value(value) {}
    
    ExprType type() const override { return ExprType::CONSTANT; }
    bool isConstant() const override { return true; }
    
    std::string toString() const override {
        switch (value_type) {
            case Type::INTEGER: return std::to_string(int_value);
            case Type::FLOAT: return std::to_string(float_value);
            case Type::STRING: return "'" + string_value + "'";
            case Type::BOOLEAN: return bool_value ? "TRUE" : "FALSE";
            case Type::NULL_VALUE: return "NULL";
        }
        return "UNKNOWN";
    }
    
    std::unique_ptr<Expression> clone() const override {
        switch (value_type) {
            case Type::INTEGER: return std::make_unique<Constant>(int_value);
            case Type::FLOAT: return std::make_unique<Constant>(float_value);
            case Type::STRING: return std::make_unique<Constant>(string_value);
            case Type::BOOLEAN: return std::make_unique<Constant>(bool_value);
            case Type::NULL_VALUE: return std::make_unique<Constant>(Type::NULL_VALUE);
        }
        return nullptr;
    }
    
    Type value_type;
    union {
        int int_value;
        double float_value;
        bool bool_value;
    };
    std::string string_value;
};

enum class BinaryOpType { 
    ADD, SUB, MUL, DIV, MOD, 
    EQ, NE, LT, LE, GT, GE,
    AND, OR, LIKE, IN
};

class BinaryOp : public Expression {
public:
    BinaryOp(BinaryOpType op, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right)
        : op_type(op), left_expr(std::move(left)), right_expr(std::move(right)) {}
    
    ExprType type() const override { return ExprType::BINARY_OP; }
    bool isConstant() const override { 
        return left_expr->isConstant() && right_expr->isConstant(); 
    }
    
    std::string toString() const override {
        std::string op_str;
        switch (op_type) {
            case BinaryOpType::ADD: op_str = "+"; break;
            case BinaryOpType::SUB: op_str = "-"; break;
            case BinaryOpType::MUL: op_str = "*"; break;
            case BinaryOpType::DIV: op_str = "/"; break;
            case BinaryOpType::EQ: op_str = "="; break;
            case BinaryOpType::NE: op_str = "!="; break;
            case BinaryOpType::LT: op_str = "<"; break;
            case BinaryOpType::LE: op_str = "<="; break;
            case BinaryOpType::GT: op_str = ">"; break;
            case BinaryOpType::GE: op_str = ">="; break;
            case BinaryOpType::AND: op_str = "AND"; break;
            case BinaryOpType::OR: op_str = "OR"; break;
            default: op_str = "?"; break;
        }
        return "(" + left_expr->toString() + " " + op_str + " " + right_expr->toString() + ")";
    }
    
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<BinaryOp>(
            op_type, 
            left_expr->clone(), 
            right_expr->clone()
        );
    }
    
    BinaryOpType op_type;
    std::unique_ptr<Expression> left_expr;
    std::unique_ptr<Expression> right_expr;
};

// ===== HIGH-LEVEL IR (HIR) =====
enum class HIRNodeType {
    SELECT, PROJECT, JOIN, FILTER, SCAN, GROUPBY, AGGREGATE
};

class HIRNode {
public:
    virtual ~HIRNode() = default;
    virtual HIRNodeType type() const = 0;
    virtual std::unique_ptr<HIRNode> clone() const = 0;
};

class ScanNode : public HIRNode {
public:
    ScanNode(std::string table) : table_name(std::move(table)) {}
    
    HIRNodeType type() const override { return HIRNodeType::SCAN; }
    std::unique_ptr<HIRNode> clone() const override {
        return std::make_unique<ScanNode>(table_name);
    }
    
    std::string table_name;
    std::vector<std::string> column_names;
};

class FilterNode : public HIRNode {
public:
    FilterNode(std::unique_ptr<HIRNode> input, std::unique_ptr<Expression> pred)
        : child(std::move(input)), condition(std::move(pred)) {}
    
    HIRNodeType type() const override { return HIRNodeType::FILTER; }
    std::unique_ptr<HIRNode> clone() const override {
        return std::make_unique<FilterNode>(
            child->clone(),
            condition->clone()
        );
    }
    
    std::unique_ptr<HIRNode> child;
    std::unique_ptr<Expression> condition;
};

class JoinNode : public HIRNode {
public:
    enum class JoinType { INNER, LEFT, RIGHT, FULL };
    
    JoinNode(std::unique_ptr<HIRNode> left, std::unique_ptr<HIRNode> right,
            std::unique_ptr<Expression> cond, JoinType jt = JoinType::INNER)
        : left_child(std::move(left)), right_child(std::move(right)),
          condition(std::move(cond)), join_type(jt) {}
    
    HIRNodeType type() const override { return HIRNodeType::JOIN; }
    std::unique_ptr<HIRNode> clone() const override {
        return std::make_unique<JoinNode>(
            left_child->clone(),
            right_child->clone(),
            condition->clone(),
            join_type
        );
    }
    
    std::unique_ptr<HIRNode> left_child;
    std::unique_ptr<HIRNode> right_child;
    std::unique_ptr<Expression> condition;
    JoinType join_type;
};

class ProjectNode : public HIRNode {
public:
    ProjectNode(std::unique_ptr<HIRNode> input,
               std::vector<std::unique_ptr<Expression>> projs,
               std::vector<std::string> names)
        : child(std::move(input)), output_names(std::move(names)) {
        for (auto& proj : projs) {
            projections.push_back(std::move(proj));
        }
    }
    
    HIRNodeType type() const override { return HIRNodeType::PROJECT; }
    std::unique_ptr<HIRNode> clone() const override {
        std::vector<std::unique_ptr<Expression>> projs_copy;
        for (const auto& proj : projections) {
            projs_copy.push_back(proj->clone());
        }
        return std::make_unique<ProjectNode>(
            child->clone(),
            std::move(projs_copy),
            output_names
        );
    }
    
    std::unique_ptr<HIRNode> child;
    std::vector<std::unique_ptr<Expression>> projections;
    std::vector<std::string> output_names;
};

// ===== MID-LEVEL IR (MIR) =====
enum class MIRNodeType {
    HASH_JOIN, NESTED_LOOP_JOIN, HASH_AGGREGATE, SCAN, 
    FILTER, PROJECT, FILTER_PROJECT
};

class MIRNode {
public:
    virtual ~MIRNode() = default;
    virtual MIRNodeType type() const = 0;
    virtual size_t estimatedCost() const = 0;
    virtual size_t estimatedRows() const = 0;
    virtual std::unique_ptr<MIRNode> clone() const = 0;
};

class ScanOp : public MIRNode {
public:
    ScanOp(std::string table, size_t est_rows)
        : table_name(std::move(table)), est_output_rows(est_rows) {}
    
    MIRNodeType type() const override { return MIRNodeType::SCAN; }
    size_t estimatedCost() const override { return est_output_rows; }
    size_t estimatedRows() const override { return est_output_rows; }
    
    std::unique_ptr<MIRNode> clone() const override {
        return std::make_unique<ScanOp>(table_name, est_output_rows);
    }
    
    std::string table_name;
    size_t est_output_rows;
};

class FilterOp : public MIRNode {
public:
    FilterOp(std::unique_ptr<MIRNode> input, std::unique_ptr<Expression> pred,
            double selectivity = 0.1)
        : child(std::move(input)), condition(std::move(pred)),
          filter_selectivity(selectivity) {}
    
    MIRNodeType type() const override { return MIRNodeType::FILTER; }
    
    size_t estimatedRows() const override {
        return static_cast<size_t>(child->estimatedRows() * filter_selectivity);
    }
    
    size_t estimatedCost() const override {
        return child->estimatedCost() + child->estimatedRows();
    }
    
    std::unique_ptr<MIRNode> clone() const override {
        return std::make_unique<FilterOp>(
            child->clone(),
            condition->clone(),
            filter_selectivity
        );
    }
    
    std::unique_ptr<MIRNode> child;
    std::unique_ptr<Expression> condition;
    double filter_selectivity;
};

class HashJoinOp : public MIRNode {
public:
    HashJoinOp(std::unique_ptr<MIRNode> build, std::unique_ptr<MIRNode> probe,
              std::unique_ptr<Expression> cond, double join_sel = 0.1)
        : build_side(std::move(build)), probe_side(std::move(probe)),
          condition(std::move(cond)), join_selectivity(join_sel) {}
    
    MIRNodeType type() const override { return MIRNodeType::HASH_JOIN; }
    
    size_t estimatedRows() const override {
        return static_cast<size_t>(build_side->estimatedRows() * 
                                  probe_side->estimatedRows() * 
                                  join_selectivity);
    }
    
    size_t estimatedCost() const override {
        return build_side->estimatedCost() + probe_side->estimatedCost() + 
               build_side->estimatedRows() + probe_side->estimatedRows();
    }
    
    std::unique_ptr<MIRNode> clone() const override {
        return std::make_unique<HashJoinOp>(
            build_side->clone(),
            probe_side->clone(),
            condition->clone(),
            join_selectivity
        );
    }
    
    std::unique_ptr<MIRNode> build_side;
    std::unique_ptr<MIRNode> probe_side;
    std::unique_ptr<Expression> condition;
    double join_selectivity;
};


} // namespace dbir