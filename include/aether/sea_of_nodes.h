#ifndef AETHER_SEA_OF_NODES_H
#define AETHER_SEA_OF_NODES_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/interpreter.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

namespace aether {

/**
 * @brief Sea-of-Nodes IR Operator Enumeration.
 * Unifies Control, Effect, and Data Flow into a single unified graph.
 */
enum class NodeOp : uint16_t {
    // Control Nodes
    Start,
    End,
    If,
    IfTrue,
    IfFalse,
    Merge,
    Loop,
    Return,

    // Effect Nodes (Chained Memory & Guard Order)
    StartEffect,
    EffectPhi,
    CheckSmi,
    CheckShape,
    LoadField,
    StoreField,

    // Data / Value Nodes
    Parameter,
    ConstantInt32,
    ConstantFloat64,
    ConstantBool,
    ConstantUndefined,
    ConstantNull,
    Phi,

    // Unboxed 32-bit Machine Arithmetic & Logic
    Int32Add,
    Int32Sub,
    Int32Mul,
    Int32Div,
    Int32Mod,
    Int32And,
    Int32Or,
    Int32Xor,

    // Unboxed Comparisons
    Int32CmpLess,
    Int32CmpLessEqual,
    Int32CmpGreater,
    Int32CmpGreaterEqual,
    Int32CmpEqual,
    Int32CmpNotEqual,

    // Speculation Failure Bailout
    Deoptimize
};

const char* NodeOpToString(NodeOp op);

class Graph;

/**
 * @brief Represents a single Node in the Sea-of-Nodes SSA Graph.
 */
class Node {
public:
    Node(uint32_t id, NodeOp op)
        : id_(id), op_(op) {}

    uint32_t Id() const noexcept { return id_; }
    NodeOp Op() const noexcept { return op_; }
    void SetOp(NodeOp op) noexcept { op_ = op; }

    const std::vector<Node*>& Inputs() const noexcept { return inputs_; }
    const std::vector<Node*>& Uses() const noexcept { return uses_; }

    void ClearInputs() {
        for (Node* in : inputs_) {
            if (in) in->RemoveUse(this);
        }
        inputs_.clear();
    }

    Node* Input(size_t index) const {
        assert(index < inputs_.size());
        return inputs_[index];
    }

    void AddInput(Node* input) {
        inputs_.push_back(input);
        if (input) {
            input->uses_.push_back(this);
        }
    }

    void SetInput(size_t index, Node* input) {
        assert(index < inputs_.size());
        if (inputs_[index] == input) return;
        if (inputs_[index]) {
            inputs_[index]->RemoveUse(this);
        }
        inputs_[index] = input;
        if (input) {
            input->uses_.push_back(this);
        }
    }

    void RemoveUse(Node* use) {
        for (auto it = uses_.begin(); it != uses_.end(); ++it) {
            if (*it == use) {
                uses_.erase(it);
                return;
            }
        }
    }

    void ReplaceUsesWith(Node* replacement) {
        for (Node* use : uses_) {
            for (size_t i = 0; i < use->inputs_.size(); ++i) {
                if (use->inputs_[i] == this) {
                    use->inputs_[i] = replacement;
                    replacement->uses_.push_back(use);
                }
            }
        }
        uses_.clear();
    }

    // Node Attributes
    int64_t IntValue() const noexcept { return int_val_; }
    void SetIntValue(int64_t val) noexcept { int_val_ = val; }

    double FloatValue() const noexcept { return float_val_; }
    void SetFloatValue(double val) noexcept { float_val_ = val; }

    uint32_t ParameterIndex() const noexcept { return param_index_; }
    void SetParameterIndex(uint32_t idx) noexcept { param_index_ = idx; }

    bool IsControl() const noexcept;
    bool IsEffect() const noexcept;
    bool IsValue() const noexcept;

    bool IsDead() const noexcept { return is_dead_; }
    void MarkDead() noexcept { is_dead_ = true; }

    uint64_t ComputeHash() const;

private:
    uint32_t id_{0};
    NodeOp op_{NodeOp::Start};
    std::vector<Node*> inputs_;
    std::vector<Node*> uses_;
    int64_t int_val_{0};
    double float_val_{0.0};
    uint32_t param_index_{0};
    bool is_dead_{false};
};

/**
 * @brief Sea-of-Nodes Graph container managing all nodes and GVN hash-consing.
 */
class Graph {
public:
    Graph();

    Node* NewNode(NodeOp op);
    Node* NewConstantInt32(int32_t val);
    Node* NewConstantBool(bool val);
    Node* NewParameter(uint32_t index);

    Node* Start() const noexcept { return start_node_; }
    Node* StartEffect() const noexcept { return start_effect_; }
    Node* ReturnNode() const noexcept { return return_node_; }
    void SetReturnNode(Node* ret) noexcept { return_node_ = ret; }

    const std::vector<std::unique_ptr<Node>>& Nodes() const noexcept { return all_nodes_; }

    // Global Value Numbering (GVN) Hash Consing
    Node* GVN(Node* node);

    std::vector<Node*> Schedule() const;

    void Dump(std::ostream& os = std::cout) const;

private:
    std::vector<std::unique_ptr<Node>> all_nodes_;
    Node* start_node_{nullptr};
    Node* start_effect_{nullptr};
    Node* return_node_{nullptr};
    std::unordered_map<uint64_t, Node*> gvn_map_;
};

/**
 * @brief Sea-of-Nodes Graph Builder: converts BytecodeArray into Sea-of-Nodes Graph.
 */
class SeaOfNodesBuilder {
public:
    SeaOfNodesBuilder(const BytecodeArray& bytecode, TypeFeedbackVector* feedback = nullptr);

    std::unique_ptr<Graph> Build();

private:
    const BytecodeArray& bytecode_;
    TypeFeedbackVector* feedback_{nullptr};
    std::unique_ptr<Graph> graph_;

    // SSA Virtual Register State
    std::vector<Node*> registers_;
    Node* acc_{nullptr};
    Node* cur_control_{nullptr};
    Node* cur_effect_{nullptr};
};

/**
 * @brief Optimization Pipeline for Sea-of-Nodes IR.
 */
class SeaOfNodesOptimizer {
public:
    static void Optimize(Graph& graph);

    static void ConstantFolding(Graph& graph);
    static void DeadCodeElimination(Graph& graph);
    static void LoopInvariantCodeMotion(Graph& graph);
};

} // namespace aether

#endif // AETHER_SEA_OF_NODES_H
