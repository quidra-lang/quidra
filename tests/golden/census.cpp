// The `census` view; see census.hpp.
#include "census.hpp"

#include "census_keys.hpp"
#include "ir_full.hpp"
#include "ir/instruction_text.hpp"

#include <array>
#include <cstddef>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::golden {
namespace {

using Counts = std::array<std::size_t, std::variant_size_v<ir::Instruction>>;
// Ordered maps: the view is compared byte for byte.
using Signatures = std::map<std::pair<std::string, std::string>, std::size_t>;

std::string bit(bool value) { return value ? "1" : "0"; }

// The kind structure of a type, without names or shapes.
std::string kind_tree(const Type& type) {
    // type_name() recurses into the element of compound kinds, so those are
    // spelled here.
    std::string out;
    switch (type.kind) {
        case TypeKind::Array: out = "array"; break;
        case TypeKind::Tensor: out = "tensor"; break;
        case TypeKind::Function: out = "fn"; break;
        case TypeKind::Class: out = "class"; break;
        case TypeKind::Union: out = type.union_name.empty() ? "union" : "enum"; break;
        default: out = type_name(type); break;
    }
    std::vector<const Type*> children;
    if (type.first) children.push_back(type.first.get());
    for (const auto& item : type.cases) children.push_back(&item);
    for (const auto& item : type.parameters) children.push_back(&item);
    if (children.empty()) return out;
    out += '(';
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i) out += ',';
        out += kind_tree(*children[i]);
    }
    out += ')';
    return out;
}

std::string tensor_side(const Type& type) { return type.kind == TypeKind::Tensor ? "T" : "S"; }

std::string element_name(const Type& left, const Type& right) {
    const Type& tensor = left.kind == TypeKind::Tensor ? left : right;
    if (tensor.kind == TypeKind::Tensor && tensor.first) return type_name(*tensor.first);
    return type_name(tensor);
}

// Value types known from the defining instruction, for container attributes.
class ValueTypes {
public:
    void record(const ir::Instruction& instruction) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ir::LoadLocal> || std::is_same_v<T, ir::LoadAddress> ||
                          std::is_same_v<T, ir::LoadReference> || std::is_same_v<T, ir::ArrayMake> ||
                          std::is_same_v<T, ir::ArrayAlloc> || std::is_same_v<T, ir::Clone> ||
                          std::is_same_v<T, ir::Retain>) {
                types_[n.out] = &n.type;
            } else if constexpr (std::is_same_v<T, ir::FieldGet>) {
                types_[n.out] = &n.field_type;
            } else if constexpr (std::is_same_v<T, ir::ArrayGet>) {
                types_[n.out] = &n.element_type;
            } else if constexpr (std::is_same_v<T, ir::Call>) {
                types_[n.out] = &n.result;
            }
        }, instruction);
    }

    std::string container(ir::ValueId value) const {
        const auto found = types_.find(value);
        if (found == types_.end() || found->second->kind != TypeKind::Array) return "?";
        return found->second->length >= 0 ? "fixed" : "dynamic";
    }

private:
    std::unordered_map<ir::ValueId, const Type*> types_;
};

std::string array_access_key(const ValueTypes& types, ir::ValueId array, const Type& element,
                             bool initialization_proven, bool bounds_proven,
                             bool initialization_guard, bool bounds_guard) {
    const bool inline_child = element.kind == TypeKind::Array && element.length >= 0;
    return "container=" + types.container(array) + "|inline_child=" + bit(inline_child) +
           "|element=" + kind_tree(element) + "|init_proven=" + bit(initialization_proven) +
           "|bounds_proven=" + bit(bounds_proven) + "|init_guard=" + bit(initialization_guard) +
           "|bounds_guard=" + bit(bounds_guard);
}

void count_module(const ir::Module& module, Counts& counts, Signatures& signatures) {
    std::unordered_set<std::string> external;
    for (const auto& function : module.functions) {
        if (function.external_symbol) external.insert(function.name);
    }
    for (const auto& function : module.functions) {
        ValueTypes types;
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                ++counts[instruction.index()];
                types.record(instruction);
                const std::string name(instruction_names[instruction.index()]);
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    std::string key;
                    if constexpr (std::is_same_v<T, ir::NumericConvert>) {
                        key = type_name(n.source_type) + "|" + type_name(n.target_type) +
                              "|checked=" + bit(n.checked_range);
                    } else if constexpr (std::is_same_v<T, ir::FallibleNumericConvert>) {
                        key = type_name(n.source_type) + "|" + type_name(n.target_type);
                    } else if constexpr (std::is_same_v<T, ir::TensorBinary> ||
                                         std::is_same_v<T, ir::TensorCompare>) {
                        key = n.op + "|" + element_name(n.left_type, n.right_type) + "|" +
                              tensor_side(n.left_type) + tensor_side(n.right_type);
                    } else if constexpr (std::is_same_v<T, ir::ArrayGet>) {
                        key = array_access_key(types, n.array, n.element_type,
                                               n.initialization_proven, n.bounds_proven,
                                               n.initialization_guard.has_value(),
                                               n.bounds_guard.has_value());
                    } else if constexpr (std::is_same_v<T, ir::ArraySet>) {
                        key = array_access_key(types, n.array, n.element_type,
                                               n.initialization_proven, n.bounds_proven,
                                               n.initialization_guard.has_value(),
                                               n.bounds_guard.has_value());
                    } else if constexpr (std::is_same_v<T, ir::Call>) {
                        bool writable = false;
                        for (const auto& argument : n.args) {
                            writable = writable || argument.writable_address.has_value();
                        }
                        key = "external=" + bit(external.count(n.callee) != 0) +
                              "|self=" + bit(n.callee == function.name) +
                              "|writable=" + bit(writable) +
                              "|no_normal_return=" + bit(n.no_normal_return);
                    } else if constexpr (std::is_same_v<T, ir::ReplDisplay>) {
                        key = kind_tree(n.type);
                    } else if constexpr (std::is_same_v<T, ir::TensorCreate>) {
                        key = "fill_mode=" + std::to_string(n.fill_mode) +
                              "|gpu=" + bit(n.gpu.has_value());
                    } else if constexpr (std::is_same_v<T, ir::TensorTrack>) {
                        key = "mode=" + std::to_string(n.mode);
                    } else if constexpr (std::is_same_v<T, ir::Binary>) {
                        key = n.op + "|" + type_name(n.operand_type) +
                              "|overflow_proven=" + bit(n.overflow_proven);
                    } else if constexpr (std::is_same_v<T, ir::Unary>) {
                        key = n.op + "|" + type_name(n.type);
                    } else {
                        return;
                    }
                    ++signatures[{name, key}];
                }, instruction);
            }
        }
    }
}

// Callee multiset of a module.
std::map<std::string, long long> callees(const ir::Module& module) {
    std::map<std::string, long long> result;
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* call = std::get_if<ir::Call>(&instruction)) ++result[call->callee];
            }
        }
    }
    return result;
}

std::size_t borrowed_stores(const ir::Module& module) {
    std::size_t result = 0;
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* store = std::get_if<ir::StoreLocal>(&instruction)) {
                    result += store->borrowed ? 1 : 0;
                }
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Checks, loops and IR lists (the `count` and `key` tables)

// The values an instruction's text names (%N): the IR has no def-use
// information, so uses are read from the text, as the optimizer reads them
// (optimizer/textual_uses.hpp).
std::vector<ir::ValueId> mentioned_values(const ir::Instruction& instruction) {
    std::vector<ir::ValueId> values;
    const auto rendered = ir::instruction_text(instruction);
    for (std::size_t at = rendered.find('%'); at != std::string::npos; at = rendered.find('%', at + 1)) {
        std::size_t end = at + 1;
        ir::ValueId value = 0;
        while (end < rendered.size() && rendered[end] >= '0' && rendered[end] <= '9') {
            value = value * 10 + static_cast<ir::ValueId>(rendered[end] - '0');
            ++end;
        }
        if (end > at + 1) values.push_back(value);
    }
    return values;
}

// The blocks of a function that lie on a cycle of its control-flow graph
// (a strongly connected component with more than one block, or a block that
// jumps to itself): the bodies, conditions and steps of its loops.
std::vector<bool> blocks_on_cycles(const ir::Function& function) {
    const std::size_t n = function.blocks.size();
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t i = 0; i < n; ++i) index_of.emplace(function.blocks[i].label, i);
    std::vector<std::vector<std::size_t>> successors(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& instruction : function.blocks[i].instructions) {
            const auto add = [&](const std::string& label) {
                if (const auto found = index_of.find(label); found != index_of.end())
                    successors[i].push_back(found->second);
            };
            if (const auto* jump = std::get_if<ir::Jump>(&instruction)) add(jump->target);
            if (const auto* branch = std::get_if<ir::Branch>(&instruction)) {
                add(branch->if_true);
                add(branch->if_false);
            }
        }
    }
    // Tarjan's algorithm, iterative so that deep graphs cannot exhaust the stack.
    std::vector<long long> order(n, -1), low(n, 0);
    std::vector<bool> on_stack(n, false), cyclic(n, false);
    std::vector<std::size_t> stack;
    long long counter = 0;
    for (std::size_t root = 0; root < n; ++root) {
        if (order[root] >= 0) continue;
        std::vector<std::pair<std::size_t, std::size_t>> work{{root, 0}};
        order[root] = low[root] = counter++;
        stack.push_back(root);
        on_stack[root] = true;
        while (!work.empty()) {
            auto& [node, next] = work.back();
            if (next < successors[node].size()) {
                const auto successor = successors[node][next++];
                if (order[successor] < 0) {
                    order[successor] = low[successor] = counter++;
                    stack.push_back(successor);
                    on_stack[successor] = true;
                    work.emplace_back(successor, 0);
                } else if (on_stack[successor]) {
                    low[node] = std::min(low[node], order[successor]);
                }
                continue;
            }
            const auto finished = node;
            work.pop_back();
            if (!work.empty()) low[work.back().first] = std::min(low[work.back().first], low[finished]);
            if (low[finished] != order[finished]) continue;
            std::vector<std::size_t> component;
            for (;;) {
                const auto member = stack.back();
                stack.pop_back();
                on_stack[member] = false;
                component.push_back(member);
                if (member == finished) break;
            }
            const bool self_loop = std::find(successors[finished].begin(),
                                             successors[finished].end(),
                                             finished) != successors[finished].end();
            if (component.size() > 1 || self_loop)
                for (const auto member : component) cyclic[member] = true;
        }
    }
    return cyclic;
}

bool arithmetic(const std::string& op) { return op == "+" || op == "-" || op == "*"; }

// Per-check and per-loop counts of one module, added to `counts`.
void count_checks(const ir::Module& module, std::map<std::string, std::size_t>& counts) {
    for (const auto& function : module.functions) {
        const auto cyclic = blocks_on_cycles(function);
        std::unordered_map<ir::ValueId, std::string> loaded;
        std::unordered_set<ir::ValueId> constants;
        for (std::size_t b = 0; b < function.blocks.size(); ++b) {
            const auto& block = function.blocks[b];
            const bool in_loop = cyclic[b];
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& instruction = block.instructions[i];
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, ir::LoadLocal>) {
                        loaded[n.out] = n.name;
                    } else if constexpr (std::is_same_v<T, ir::ConstantInt>) {
                        constants.insert(n.out);
                    } else if constexpr (std::is_same_v<T, ir::ShapedConstraintCheck>) {
                        ++counts["check.shape"];
                    } else if constexpr (std::is_same_v<T, ir::ExtentEqualCheck>) {
                        ++counts["check.extent"];
                    } else if constexpr (std::is_same_v<T, ir::RangeCheckStep>) {
                        ++counts["check.range-step"];
                    } else if constexpr (std::is_same_v<T, ir::TestAssert>) {
                        ++counts["check.assert"];
                    } else if constexpr (std::is_same_v<T, ir::ArrayGet> ||
                                         std::is_same_v<T, ir::ArraySet>) {
                        if (!n.bounds_proven) ++counts["check.bounds.array"];
                        if (!n.initialization_proven) ++counts["check.initialized.array"];
                    } else if constexpr (std::is_same_v<T, ir::AddressElement>) {
                        ++counts["check.bounds.address"];
                    } else if constexpr (std::is_same_v<T, ir::BinGet> ||
                                         std::is_same_v<T, ir::BinSet>) {
                        if (!n.bounds_proven) ++counts["check.bounds.bin"];
                    } else if constexpr (std::is_same_v<T, ir::StringIndex> ||
                                         std::is_same_v<T, ir::StringIndexAsciiCompare>) {
                        ++counts["check.bounds.string"];
                    } else if constexpr (std::is_same_v<T, ir::StringSlice> ||
                                         std::is_same_v<T, ir::BinSlice>) {
                        ++counts["check.slice"];
                    } else if constexpr (std::is_same_v<T, ir::NumericConvert>) {
                        if (n.checked_range) ++counts["check.conversion"];
                    } else if constexpr (std::is_same_v<T, ir::FallibleNumericConvert> ||
                                         std::is_same_v<T, ir::ArrayNumericCast> ||
                                         std::is_same_v<T, ir::BinConvert>) {
                        ++counts["check.conversion"];
                    } else if constexpr (std::is_same_v<T, ir::Binary>) {
                        const bool fixed = is_fixed_integer(n.operand_type);
                        const bool checked = fixed && arithmetic(n.op) && !n.overflow_proven;
                        if (checked) ++counts["check.overflow"];
                        if (is_integer_family_type(n.operand_type) && (n.op == "/" || n.op == "%"))
                            ++counts["check.divide"];
                        if (!in_loop) return;
                        if (checked) ++counts["loop.overflow-checked"];
                        if (is_bare_integer(n.operand_type) &&
                            (arithmetic(n.op) || n.op == "/" || n.op == "%" || n.op == "^"))
                            ++counts["loop.bigint-arithmetic"];
                        // An induction step: a local's value plus or minus a
                        // constant, stored back into the same local.
                        if (!checked || (n.op != "+" && n.op != "-")) return;
                        std::string variable;
                        if (const auto found = loaded.find(n.left);
                            found != loaded.end() && constants.contains(n.right))
                            variable = found->second;
                        else if (const auto other = loaded.find(n.right);
                                 other != loaded.end() && n.op == "+" && constants.contains(n.left))
                            variable = other->second;
                        if (variable.empty()) return;
                        for (std::size_t j = i + 1; j < block.instructions.size(); ++j) {
                            const auto* store = std::get_if<ir::StoreLocal>(&block.instructions[j]);
                            if (store && store->value == n.out) {
                                if (store->name == variable) ++counts["loop.overflow-checked-step"];
                                break;
                            }
                        }
                    }
                }, instruction);
            }
        }
    }
}

// Numeric operations per kind: arithmetic, comparisons and conversions.
void count_operations(const ir::Module& module, std::map<std::string, std::size_t>& counts) {
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* n = std::get_if<ir::Binary>(&instruction)) {
                    if (numeric_info(n->operand_type)) ++counts[type_name(n->operand_type) + " " + n->op];
                } else if (const auto* u = std::get_if<ir::Unary>(&instruction)) {
                    if (numeric_info(u->type)) ++counts[type_name(u->type) + " unary" + u->op];
                } else if (const auto* c = std::get_if<ir::NumericConvert>(&instruction)) {
                    ++counts[type_name(c->source_type) + " as." + type_name(c->target_type)];
                } else if (const auto* f = std::get_if<ir::FallibleNumericConvert>(&instruction)) {
                    ++counts[type_name(f->source_type) + " as." + type_name(f->target_type)];
                }
            }
        }
    }
}

// A source position as the census prints it: FILE:LINE:COLUMN.
std::string position(const std::string& file, std::uint32_t line, std::uint32_t column) {
    return (file.empty() ? std::string("-") : file) + ":" + std::to_string(line) + ":" +
           std::to_string(column);
}

// The IR-derived lists: compound assignments to an element or a field whose
// right-hand side contains a call, and the value collection loops whose
// lowering keeps their own source (B0.2's hidden source local).
void ir_keys(const ir::Module& module, CensusKeys& keys) {
    auto& compound = keys["compound-store-across-call"];
    auto& loops = keys["collection-loop-writes-iterable"];
    for (const auto& function : module.functions) {
        struct Definition { const ir::Instruction* instruction; std::size_t at; };
        std::unordered_map<ir::ValueId, Definition> definitions;
        std::vector<std::size_t> calls;
        std::set<std::string> sources;
        std::string here = position(function.source_file, function.source_line,
                                    function.source_column);
        std::size_t at = 0;
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                ++at;
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, ir::SourceLocation>) {
                        here = position(n.source_file, n.line, n.column);
                    } else if constexpr (std::is_same_v<T, ir::Call> ||
                                         std::is_same_v<T, ir::IndirectCall>) {
                        calls.push_back(at);
                    } else if constexpr (std::is_same_v<T, ir::StoreLocal>) {
                        if (n.name.rfind("$for.source.", 0) == 0 && sources.insert(n.name).second)
                            loops.push_back(here);
                    } else if constexpr (std::is_same_v<T, ir::StoreAddress>) {
                        const auto address = definitions.find(n.address);
                        const auto value = definitions.find(n.value);
                        if (address == definitions.end() || value == definitions.end()) return;
                        const auto* element = std::get_if<ir::AddressElement>(address->second.instruction);
                        const auto* field = std::get_if<ir::AddressField>(address->second.instruction);
                        if (!element && !field) return;
                        // The stored value is computed from a read of the same
                        // place: the address itself, or one resolved again
                        // with the same index (a compound assignment whose
                        // store re-resolves its target).
                        for (auto loaded : mentioned_values(*value->second.instruction)) {
                            auto definition = definitions.find(loaded);
                            // A managed old value is read through its retain.
                            if (definition != definitions.end()) {
                                if (const auto* retain = std::get_if<ir::Retain>(definition->second.instruction))
                                    definition = definitions.find(retain->value);
                            }
                            if (definition == definitions.end()) continue;
                            const auto* load = std::get_if<ir::LoadAddress>(definition->second.instruction);
                            if (!load) continue;
                            const auto source = definitions.find(load->address);
                            if (source == definitions.end()) continue;
                            const auto* read_element = std::get_if<ir::AddressElement>(source->second.instruction);
                            const auto* read_field = std::get_if<ir::AddressField>(source->second.instruction);
                            const bool same = load->address == n.address ||
                                              (element && read_element && read_element->index == element->index) ||
                                              (field && read_field && read_field->index == field->index);
                            if (!same) continue;
                            const auto read_at = definition->second.at;
                            const auto first_call = std::upper_bound(calls.begin(), calls.end(), read_at);
                            if (first_call != calls.end() && *first_call < at) {
                                compound.push_back(here);
                                return;
                            }
                        }
                    }
                    if constexpr (requires { n.out; }) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(n.out)>, ir::ValueId>)
                            definitions[n.out] = Definition{&instruction, at};
                    }
                }, instruction);
            }
        }
    }
}

} // namespace

std::string census_view(const ir::Module* lowered, const ir::Module* optimized,
                        const CheckedProgram* checked) {
    std::vector<const ir::Module*> lowered_modules;
    std::vector<const ir::Module*> optimized_modules;
    if (lowered) lowered_modules.push_back(lowered);
    if (optimized) optimized_modules.push_back(optimized);
    std::vector<const CheckedProgram*> programs;
    if (checked) programs.push_back(checked);
    return census_view(lowered_modules, optimized_modules, programs);
}

std::string census_view(const std::vector<const ir::Module*>& lowered,
                        const std::vector<const ir::Module*>& optimized,
                        const std::vector<const CheckedProgram*>& checked) {
    Counts lowered_counts{};
    Counts optimized_counts{};
    Signatures lowered_signatures;
    Signatures optimized_signatures;
    for (const auto* module : lowered) count_module(*module, lowered_counts, lowered_signatures);
    for (const auto* module : optimized) {
        count_module(*module, optimized_counts, optimized_signatures);
    }

    std::string out;
    out += "stages lowered=" + std::to_string(lowered.size()) +
           " optimized=" + std::to_string(optimized.size()) + "\n";
    for (std::size_t i = 0; i < instruction_names.size(); ++i) {
        out += "alt " + std::to_string(i) + ":" + std::string(instruction_names[i]) + " " +
               std::to_string(lowered_counts[i]) + " " + std::to_string(optimized_counts[i]) + "\n";
    }

    std::map<std::pair<std::string, std::string>, std::pair<std::size_t, std::size_t>> keys;
    for (const auto& [key, count] : lowered_signatures) keys[key].first = count;
    for (const auto& [key, count] : optimized_signatures) keys[key].second = count;
    for (const auto& [key, count] : keys) {
        out += "sig " + key.first + " " + key.second + " " + std::to_string(count.first) + " " +
               std::to_string(count.second) + "\n";
    }

    if (!lowered.empty() && !optimized.empty()) {
        const auto delta = [&](std::string_view name) {
            for (std::size_t i = 0; i < instruction_names.size(); ++i) {
                if (instruction_names[i] == name) {
                    return static_cast<long long>(optimized_counts[i]) -
                           static_cast<long long>(lowered_counts[i]);
                }
            }
            return 0LL;
        };
        out += "event clone_delta - " + std::to_string(delta("Clone")) + "\n";
        out += "event retain_delta - " + std::to_string(delta("Retain")) + "\n";
        out += "event release_delta - " + std::to_string(delta("Release")) + "\n";
        long long borrowed = 0;
        for (const auto* module : optimized) borrowed += static_cast<long long>(borrowed_stores(*module));
        for (const auto* module : lowered) borrowed -= static_cast<long long>(borrowed_stores(*module));
        out += "event borrowed_store_delta - " + std::to_string(borrowed) + "\n";
        std::map<std::string, long long> changed;
        for (const auto* module : optimized) {
            for (const auto& [callee, count] : callees(*module)) changed[callee] += count;
        }
        for (const auto* module : lowered) {
            for (const auto& [callee, count] : callees(*module)) changed[callee] -= count;
        }
        for (const auto& [callee, count] : changed) {
            if (count > 0) out += "event callee_added " + quote(callee) + " " + std::to_string(count) + "\n";
            if (count < 0) out += "event callee_removed " + quote(callee) + " " + std::to_string(-count) + "\n";
        }
        std::size_t regions = 0;
        std::size_t candidates = 0;
        std::size_t tables = 0;
        std::size_t backward = 0;
        for (const auto* module : optimized) {
            for (const auto& function : module->functions) {
                regions += function.tensor_regions.size();
                for (const auto& region : function.tensor_regions) {
                    candidates += region.compiler_fusion_candidates.size();
                    tables += region.compiler_extension_tables.size();
                    backward += region.reaches_backward ? 1 : 0;
                }
            }
        }
        out += "event regions - " + std::to_string(regions) + "\n";
        out += "event region_backward - " + std::to_string(backward) + "\n";
        out += "event fusion_candidates - " + std::to_string(candidates) + "\n";
        out += "event extension_tables - " + std::to_string(tables) + "\n";
    }

    // Checks by kind, loop operations and numeric operations per kind.
    std::map<std::string, std::pair<std::size_t, std::size_t>> counts;
    std::map<std::string, std::pair<std::size_t, std::size_t>> operations;
    for (const auto* module : lowered) {
        std::map<std::string, std::size_t> stage;
        count_checks(*module, stage);
        for (const auto& [name, count] : stage) counts[name].first += count;
        stage.clear();
        count_operations(*module, stage);
        for (const auto& [name, count] : stage) operations[name].first += count;
    }
    for (const auto* module : optimized) {
        std::map<std::string, std::size_t> stage;
        count_checks(*module, stage);
        for (const auto& [name, count] : stage) counts[name].second += count;
        stage.clear();
        count_operations(*module, stage);
        for (const auto& [name, count] : stage) operations[name].second += count;
    }
    for (const auto& name : census_count_names) counts.try_emplace(std::string(name));
    for (const auto& [name, count] : counts) {
        out += "count " + name + " " + std::to_string(count.first) + " " +
               std::to_string(count.second) + "\n";
    }
    for (const auto& [name, count] : operations) {
        out += "op " + name + " " + std::to_string(count.first) + " " +
               std::to_string(count.second) + "\n";
    }

    // Numeric kinds after checking, and the listed sites of every key: the
    // IR lists from the lowered module (the optimized one when no lowered
    // module exists), the source lists from the checked programs.
    std::map<std::string, std::size_t> kinds;
    CensusKeys listed;
    for (const auto& name : census_key_names) listed.try_emplace(std::string(name));
    const auto& ir_modules = lowered.empty() ? optimized : lowered;
    for (const auto* module : ir_modules) ir_keys(*module, listed);
    for (const auto* program : checked) source_keys(*program, listed, kinds);
    for (const auto& [kind, count] : kinds) {
        out += "kind " + kind + " " + std::to_string(count) + "\n";
    }
    for (const auto& [key, sites] : listed) {
        out += "key " + key + " " + std::to_string(sites.size()) + "\n";
    }
    for (const auto& [key, sites] : listed) {
        auto sorted = sites;
        std::sort(sorted.begin(), sorted.end());
        for (const auto& site : sorted) out += "site " + key + " " + site + "\n";
    }
    return out;
}

} // namespace quidra::golden
