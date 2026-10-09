// StaticTensorFactsAnalysis (static_tensor_facts.hpp).
#include "optimizer/static_tensor_facts.hpp"

#include <string>
#include <type_traits>
#include <variant>

namespace quidra::optimizer {

using ir::Clone;
using ir::Function;
using ir::LoadLocal;
using ir::StoreLocal;
using ir::TensorCast;
using ir::TensorContiguous;
using ir::TensorCreate;
using ir::TensorGather;
using ir::TensorReshape;
using ir::TensorScatter;
using ir::TensorTrack;
using ir::TensorTransfer;
using ir::TensorTranspose;
using ir::ValueId;

std::unordered_map<ValueId, StaticTensorFacts> StaticTensorFactsAnalysis::run(
    const Function& function) {
    std::unordered_map<ValueId, StaticTensorFacts> facts;
    const auto inherited =
        [&](ValueId value) -> StaticTensorFacts {
            const auto found = facts.find(value);
            return found == facts.end()
                ? StaticTensorFacts{}
                : found->second;
        };
    for (const auto& block : function.blocks) {
        // Keep local flow facts block-local. Crossing a CFG edge would
        // require dominance/merge reasoning; unknown is safer than a
        // speculative specialization.
        std::unordered_map<std::string, StaticTensorFacts> locals;
        for (const auto& instruction : block.instructions) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, StoreLocal>) {
                        if (node.type.kind != TypeKind::Tensor) return;
                        const auto found = facts.find(node.value);
                        if (found == facts.end())
                            locals.erase(node.name);
                        else
                            locals[node.name] = found->second;
                    } else if constexpr (
                        std::is_same_v<T, LoadLocal>) {
                        if (node.type.kind != TypeKind::Tensor) return;
                        StaticTensorFacts fact;
                        if (const auto found = locals.find(node.name);
                            found != locals.end()) {
                            fact = found->second;
                        }
                        fact.type = node.type;
                        fact.has_type = true;
                        // A local owns its value; loading the local does
                        // not transfer that ownership to the SSA value.
                        // Treat the load as an alias unless an explicit
                        // Clone creates independent storage.
                        fact.owns_storage = false;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, Clone>) {
                        if (node.type.kind != TypeKind::Tensor) return;
                        auto fact = inherited(node.value);
                        fact.type = node.type;
                        fact.has_type = true;
                        // Tensor values are independent at the language
                        // level, but Clone currently shares the underlying
                        // TensorStorage (the runtime increments its owner
                        // count). Never use a cloned descriptor as proof
                        // that package code may mutate storage in place.
                        fact.owns_storage = false;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorCreate>) {
                        StaticTensorFacts fact;
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.device = node.gpu
                            ? StaticDevice::Gpu
                            : StaticDevice::Cpu;
                        fact.contiguous = true;
                        fact.tracked = false;
                        fact.owns_storage = true;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorTransfer>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.device = node.gpu
                            ? StaticDevice::Gpu
                            : StaticDevice::Cpu;
                        // Transfers materialize a fresh dense tensor
                        // on the destination backend. The runtime rebuilds
                        // canonical contiguous strides, so backend rules
                        // may rely on this layout fact.
                        fact.contiguous = true;
                        fact.owns_storage = true;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorContiguous>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.contiguous = true;
                        // contiguous() may return a value-semantic
                        // clone that shares already-contiguous storage.
                        // Without a compile-time uniqueness proof this
                        // result cannot authorize package in-place
                        // memory reuse.
                        fact.owns_storage = false;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorReshape>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.owns_storage = false;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorTranspose>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.contiguous = false;
                        fact.owns_storage = false;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorGather> ||
                        std::is_same_v<T, TensorScatter>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.contiguous = true;
                        fact.owns_storage = true;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorTrack>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.type;
                        fact.has_type =
                            node.type.kind == TypeKind::Tensor;
                        fact.tracked = node.mode != ir::tensor_track_mode::untrack;
                        fact.owns_storage = false;
                        facts[node.out] = std::move(fact);
                    } else if constexpr (
                        std::is_same_v<T, TensorCast>) {
                        auto fact = inherited(node.tensor);
                        fact.type = node.target_type;
                        fact.has_type =
                            node.target_type.kind == TypeKind::Tensor;
                        fact.contiguous = true;
                        fact.tracked = false;
                        fact.owns_storage = true;
                        facts[node.out] = std::move(fact);
                    }
                },
                instruction);
        }
    }
    return facts;
}

} // namespace quidra::optimizer
