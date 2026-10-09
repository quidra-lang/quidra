// DataflowComponentRegions (region_formation.hpp). The components are found
// by a depth-first walk in node order, and every list a region holds is
// sorted, so the regions do not depend on hash order.
#include "optimizer/region_formation.hpp"

#include "optimizer/rule_registry.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quidra::optimizer {

using ir::Call;
using ir::ConstantBool;
using ir::Function;
using ir::Instruction;
using ir::Module;
using ir::TensorBackward;
using ir::TensorBinary;
using ir::TensorCast;
using ir::TensorCompare;
using ir::TensorContiguous;
using ir::TensorGather;
using ir::TensorIndex;
using ir::TensorRegion;
using ir::TensorRegionLocation;
using ir::TensorReshape;
using ir::TensorScatter;
using ir::TensorTranspose;
using ir::ValueId;

namespace {

struct TensorRegionNode {
    TensorRegionLocation location;
    ValueId out{};
    std::vector<ValueId> inputs;
    std::vector<std::string> compiler_extensions;
    std::vector<std::string> compiler_operations;
};

std::optional<TensorRegionNode> tensor_region_node(
    const Instruction& instruction, std::size_t block_index,
    std::size_t instruction_index,
    const std::unordered_map<std::string, std::vector<std::string>>&
        call_extensions,
    const std::unordered_map<std::string, std::vector<std::string>>&
        call_operations) {
    return std::visit(
        [&](const auto& n) -> std::optional<TensorRegionNode> {
            using T = std::decay_t<decltype(n)>;
            TensorRegionNode node{
                TensorRegionLocation{block_index, instruction_index},
                0, {}, {}, {}};

            if constexpr (std::is_same_v<T, TensorReshape>) {
                node.out = n.out;
                node.inputs = {n.tensor, n.shape};
            } else if constexpr (std::is_same_v<T, TensorTranspose>) {
                node.out = n.out;
                node.inputs = {n.tensor, n.axis0, n.axis1};
            } else if constexpr (std::is_same_v<T, TensorContiguous>) {
                node.out = n.out;
                node.inputs = {n.tensor};
            } else if constexpr (std::is_same_v<T, TensorGather>) {
                node.out = n.out;
                node.inputs = {n.tensor, n.indices, n.shape};
            } else if constexpr (std::is_same_v<T, TensorScatter>) {
                node.out = n.out;
                node.inputs = {n.tensor, n.indices, n.shape};            } else if constexpr (std::is_same_v<T, TensorCast>) {
                node.out = n.out;
                node.inputs = {n.tensor};
            } else if constexpr (std::is_same_v<T, TensorBinary>) {
                node.out = n.out;
                node.inputs = {n.left, n.right};
            } else if constexpr (std::is_same_v<T, TensorCompare>) {
                node.out = n.out;
                node.inputs = {n.left, n.right};
            } else if constexpr (std::is_same_v<T, TensorIndex>) {
                node.out = n.out;
                node.inputs.push_back(n.tensor);
                for (const auto& item : n.items) {
                    if (item.index) node.inputs.push_back(*item.index);
                    if (item.start) node.inputs.push_back(*item.start);
                    if (item.stop) node.inputs.push_back(*item.stop);
                    if (item.step) node.inputs.push_back(*item.step);
                }
            } else if constexpr (std::is_same_v<T, Call>) {
                const auto extension = call_extensions.find(n.callee);
                if (n.result.kind != TypeKind::Tensor ||
                    extension == call_extensions.end() ||
                    std::any_of(
                        n.args.begin(), n.args.end(),
                        [](const auto& argument) {
                            return argument.writable_address.has_value();
                        })) {
                    return std::nullopt;
                }
                node.out = n.out;
                for (const auto& argument : n.args) {
                    if (argument.value != 0) node.inputs.push_back(argument.value);
                }
                node.compiler_extensions = extension->second;
                if (const auto operations = call_operations.find(n.callee);
                    operations != call_operations.end()) {
                    node.compiler_operations = operations->second;
                }
            } else {
                return std::nullopt;
            }
            return node;
        },
        instruction);
}

// Replaces the regions of function. call_extensions and call_operations
// map a function name to the extensions and operations it implements.
void form(
    Function& function,
    const std::unordered_map<std::string, std::vector<std::string>>&
        call_extensions,
    const std::unordered_map<std::string, std::vector<std::string>>&
        call_operations) {
    function.tensor_regions.clear();

    std::unordered_map<ValueId, bool> bool_constants;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (const auto* constant = std::get_if<ConstantBool>(&instruction))
                bool_constants.emplace(constant->out, constant->value);
        }
    }

    std::vector<TensorRegionNode> nodes;
    std::unordered_map<ValueId, std::size_t> producers;
    for (std::size_t block_index = 0; block_index < function.blocks.size();
         ++block_index) {
        const auto& block = function.blocks[block_index];
        for (std::size_t instruction_index = 0;
             instruction_index < block.instructions.size();
             ++instruction_index) {
            auto node = tensor_region_node(
                block.instructions[instruction_index], block_index,
                instruction_index, call_extensions, call_operations);
            if (!node) continue;
            producers[node->out] = nodes.size();
            nodes.push_back(std::move(*node));
        }
    }
    if (nodes.empty()) return;

    std::vector<std::vector<std::size_t>> adjacency(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        for (const auto input : nodes[index].inputs) {
            const auto found = producers.find(input);
            if (found == producers.end()) continue;
            adjacency[index].push_back(found->second);
            adjacency[found->second].push_back(index);
        }
    }

    std::vector<int> component(nodes.size(), -1);
    int component_count = 0;
    for (std::size_t start = 0; start < nodes.size(); ++start) {
        if (component[start] >= 0) continue;
        std::vector<std::size_t> stack{start};
        component[start] = component_count;
        while (!stack.empty()) {
            const auto current = stack.back();
            stack.pop_back();
            for (const auto next : adjacency[current]) {
                if (component[next] >= 0) continue;
                component[next] = component_count;
                stack.push_back(next);
            }
        }
        ++component_count;
    }

    std::vector<std::vector<std::size_t>> members(
        static_cast<std::size_t>(component_count));
    for (std::size_t index = 0; index < nodes.size(); ++index)
        members[static_cast<std::size_t>(component[index])].push_back(index);

    for (auto& group : members) {
        std::sort(
            group.begin(), group.end(),
            [&](std::size_t left, std::size_t right) {
                const auto& a = nodes[left].location;
                const auto& b = nodes[right].location;
                if (a.block != b.block) return a.block < b.block;
                return a.instruction < b.instruction;
            });
        bool has_package_extension = false;
        for (const auto index : group) {
            if (!nodes[index].compiler_extensions.empty()) {
                has_package_extension = true;
                break;
            }
        }
        if (group.size() < 2 && !has_package_extension) continue;

        TensorRegion region;
        std::unordered_set<ValueId> region_values;
        std::unordered_set<std::string> region_extensions;
        for (const auto index : group) {
            region.instructions.push_back(nodes[index].location);
            region.values.push_back(nodes[index].out);
            region_values.insert(nodes[index].out);
            region_extensions.insert(
                nodes[index].compiler_extensions.begin(),
                nodes[index].compiler_extensions.end());
            region.compiler_operations.insert(
                region.compiler_operations.end(),
                nodes[index].compiler_operations.begin(),
                nodes[index].compiler_operations.end());
        }
        region.compiler_extensions.assign(
            region_extensions.begin(), region_extensions.end());

        std::unordered_set<ValueId> external_inputs;
        for (const auto index : group) {
            for (const auto input : nodes[index].inputs) {
                if (input != 0 && !region_values.contains(input))
                    external_inputs.insert(input);
            }
        }
        region.external_inputs.assign(
            external_inputs.begin(), external_inputs.end());

        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* backward =
                        std::get_if<TensorBackward>(&instruction);
                    backward && region_values.contains(backward->tensor)) {
                    region.reaches_backward = true;
                    const auto tracking = bool_constants.find(backward->track);
                    if (tracking == bool_constants.end() || tracking->second)
                        region.may_require_higher_order = true;
                }
            }
        }

        std::sort(
            region.instructions.begin(), region.instructions.end(),
            [](const TensorRegionLocation& left,
               const TensorRegionLocation& right) {
                if (left.block != right.block)
                    return left.block < right.block;
                return left.instruction < right.instruction;
            });
        std::sort(region.external_inputs.begin(), region.external_inputs.end());
        std::sort(region.values.begin(), region.values.end());
        std::sort(
            region.compiler_extensions.begin(),
            region.compiler_extensions.end());
        function.tensor_regions.push_back(std::move(region));
    }
}

} // namespace

bool DataflowComponentRegions::run(Module& module, const PassContext& context) {
    const auto& call_extensions = context.registry.bindings().call_extensions;
    const auto& call_operations = context.registry.bindings().call_operations;
    for (auto& function : module.functions) {
        form(function, call_extensions, call_operations);
        for (const auto& extension : module.compiler_extensions) {
            if (extension.phase != "tensor-region" ||
                !source_belongs_to_package(
                    function.source_file, extension.package_root))
                continue;
            const auto identity =
                extension.package + "." + extension.name;
            for (auto& region : function.tensor_regions) {
                if (std::find(
                        region.compiler_extensions.begin(),
                        region.compiler_extensions.end(),
                        identity) == region.compiler_extensions.end()) {
                    region.compiler_extensions.push_back(identity);
                    std::sort(
                        region.compiler_extensions.begin(),
                        region.compiler_extensions.end());
                }
            }
        }
    }
    return false;
}

} // namespace quidra::optimizer
