#include "semantics/tensor_extents.hpp"
#include "semantics/effect_summary.hpp"
#include "ir/facts/tensor.hpp"

#include "constant_integer_eval.hpp"
#include "quidra/member_function_names.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace quidra::semantics {
namespace {
using ir::facts::TensorShapeOp;
using ir::facts::TensorShapeTransfer;
using ir::facts::transfer_tensor_shape;

struct CallableShape {
    const TypeName* type{};
    std::map<std::string, ShapeExtent> scalars;
};

struct TensorContract {
    std::vector<std::optional<ShapeExtent>> extents;
    std::optional<std::size_t> ellipsis;
};

bool pattern_rank_matches(long long rank, std::size_t axes,
                          std::optional<std::size_t> ellipsis) {
    if (rank < 0) return false;
    return ellipsis ? rank >= static_cast<long long>(axes)
                    : rank == static_cast<long long>(axes);
}

std::optional<std::size_t> pattern_axis(std::size_t axis, std::size_t axes,
                                      std::optional<std::size_t> ellipsis,
                                      long long rank) {
    if (rank >= 0 && !pattern_rank_matches(rank, axes, ellipsis)) return std::nullopt;
    if (!ellipsis || axis < *ellipsis) return axis;
    if (rank < 0) return std::nullopt;
    return static_cast<std::size_t>(rank) - (axes - axis);
}

ShapeExtent pattern_extent(const Type& value, std::size_t axis, std::size_t axes,
                           std::optional<std::size_t> ellipsis) {
    const auto actual = pattern_axis(axis, axes, ellipsis, value.length);
    return actual ? tensor_extent(value, *actual) : ShapeExtent{};
}

void set_pattern_extent(Type& value, std::size_t axis, std::size_t axes,
                        std::optional<std::size_t> ellipsis, ShapeExtent extent) {
    if (const auto actual = pattern_axis(axis, axes, ellipsis, value.length))
        value.set_extent(*actual, std::move(extent));
}

struct Environment {
    std::map<std::string, Type> tensors;
    std::map<std::string, ShapeExtent> scalars;
    std::unordered_set<std::string> references;
    std::map<std::string, TensorContract> contracts;
    TensorContract result_contract;
    std::map<std::string, std::vector<std::optional<ShapeExtent>>> array_contracts;
    std::map<std::string, CallableShape> callable_shapes;
};

class ExtentAnalysis {
public:
    explicit ExtentAnalysis(CheckedProgram& checked)
        : checked_(checked), original_types_(checked.raw_types), original_bindings_(checked.binding_types) {}

    void run() {
        bool changed;
        do {
            changed = false;
            for (const auto& function : checked_.program.functions) {
                if (!function.external_symbol && !function.is_prototype)
                    changed = analyze(function.name, function) || changed;
            }
            for (const auto& declaration : checked_.program.classes) {
                std::size_t constructors = 0;
                for (const auto& method : declaration.methods) {
                    const auto name = method.is_constructor
                        ? member_function_name::constructor(declaration.name, constructors++)
                        : member_function_name::method(declaration.name, method.name);
                    changed = analyze(name, method) || changed;
                }
            }
        } while (changed);
        reset();
        Environment environment;
        block(checked_.program.statements, environment);
    }

private:
    CheckedProgram& checked_;
    const std::unordered_map<const Expr*, Type> original_types_;
    const std::unordered_map<const Stmt*, Type> original_bindings_;
    std::unordered_map<const Expr*, ShapeExtent> scalar_values_;
    std::string current_summary_;
    std::map<std::string, std::set<std::string>> summary_dependencies_;
    std::map<std::string, std::set<std::size_t>> widened_axes_;

    bool recursive_summary(const std::string& name) const {
        std::set<std::string> visited;
        std::vector<std::string> pending;
        if (const auto found = summary_dependencies_.find(name); found != summary_dependencies_.end())
            pending.assign(found->second.begin(), found->second.end());
        while (!pending.empty()) {
            auto current = std::move(pending.back());
            pending.pop_back();
            if (current == name) return true;
            if (!visited.insert(current).second) continue;
            if (const auto found = summary_dependencies_.find(current); found != summary_dependencies_.end())
                pending.insert(pending.end(), found->second.begin(), found->second.end());
        }
        return false;
    }
    std::int64_t next_symbol_{};
    std::size_t loop_depth_{};
    std::unordered_map<const void*, std::vector<ShapeExtent>> sources_;
    std::unordered_map<const void*, std::vector<ShapeExtent>> mutations_;
    std::unordered_map<const void*, std::unordered_map<std::string, std::vector<ShapeExtent>>> merges_;
    std::unordered_map<std::int64_t, std::size_t> symbol_depths_;
    std::unordered_map<std::int64_t, std::pair<std::size_t, std::size_t>> parameter_axes_;
    std::unordered_map<const Stmt*, Type> returns_;
    std::unordered_map<std::int64_t, std::size_t> parameter_values_;

    void reset() {
        next_symbol_ = 0;
        loop_depth_ = 0;
        sources_.clear();
        mutations_.clear();
        merges_.clear();
        symbol_depths_.clear();
        parameter_axes_.clear();
        parameter_values_.clear();
        returns_.clear();
        scalar_values_.clear();
    }

    ShapeExtent fresh(const void* site, std::size_t axis) {
        auto& values = sources_[site];
        while (values.size() <= axis) {
            const auto id = next_symbol_++;
            values.push_back(ShapeExtent::symbol(id));
            symbol_depths_[id] = loop_depth_;
        }
        return values[axis];
    }

    ShapeExtent mutated(const void* site, std::size_t axis) {
        auto& values = mutations_[site];
        while (values.size() <= axis) {
            const auto id = next_symbol_++;
            values.push_back(ShapeExtent::symbol(id));
            symbol_depths_[id] = loop_depth_;
        }
        return values[axis];
    }

    ShapeExtent merge_symbol(const void* site, const std::string& name, std::size_t axis) {
        auto& values = merges_[site][name];
        while (values.size() <= axis) {
            const auto id = next_symbol_++;
            values.push_back(ShapeExtent::symbol(id));
            symbol_depths_[id] = loop_depth_;
        }
        return values[axis];
    }

    Type seed(Type type, const void* site) {
        if (type.kind != TypeKind::Tensor || type.length < 0) return type;
        type.tensor_known_shape_prefix.resize(static_cast<std::size_t>(type.length));
        for (std::size_t axis = 0; axis < type.tensor_known_shape_prefix.size(); ++axis) {
            auto& extent = type.tensor_known_shape_prefix[axis];
            if (const auto known = tensor_known_extent(type, axis)) extent = *known;
            else if (extent.kind == ShapeExtent::Kind::Unknown) extent = fresh(site, axis);
        }
        return type;
    }

    Type original(const Expr& expression) const {
        if (const auto found = original_types_.find(&expression); found != original_types_.end())
            return found->second;
        if (const auto found = checked_.expr_types.find(&expression); found != checked_.expr_types.end())
            return found->second;
        return Type::simple(TypeKind::Invalid);
    }

    void record(const Expr& expression, const Type& fact) {
        if (fact.kind != TypeKind::Tensor) return;
        const auto enrich = [&](auto& types) {
            if (const auto found = types.find(&expression);
                found != types.end() && found->second.kind == TypeKind::Tensor) {
                found->second.length = fact.length;
                found->second.tensor_known_shape_prefix = fact.tensor_known_shape_prefix;
            }
        };
        enrich(checked_.expr_types);
        enrich(checked_.raw_types);
    }

    ShapeExtent scalar(const Expr& expression, Environment& environment) {
        if (const auto known = constant_eval::integer(expression); known && *known >= 0)
            return *known;
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (!name->this_qualifier) {
                if (const auto found = environment.scalars.find(name->name); found != environment.scalars.end())
                    return found->second;
            }
        }
        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data))
            return extent_arithmetic(binary->op, captured_scalar(*binary->left, environment),
                                     captured_scalar(*binary->right, environment));
        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            const auto value = captured_scalar(*unary->operand, environment);
            if (unary->op == "+") return value;
            if (unary->op == "-") return extent_arithmetic("-", ShapeExtent(0), value);
        }
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            if (call->callee == "nat" && call->args.size() == 1)
                return scalar(*call->args.front().value, environment);
        }
        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            if (index->items.size() == 1 && !index->items.front().slice && index->items.front().index) {
                const auto* shape = std::get_if<MethodCallExpr>(&index->base->data);
                const auto axis = constant_eval::integer(*index->items.front().index);
                if (shape && shape->method == "shape" && axis && *axis >= 0) {
                    const auto tensor = checked_.raw_types.contains(shape->receiver.get())
                        ? checked_.raw_types.at(shape->receiver.get()) : original(*shape->receiver);
                    if (tensor.length >= 0 && *axis < tensor.length)
                        return tensor_extent(tensor, static_cast<std::size_t>(*axis));
                }
            }
        }
        return fresh(&expression, 0);
    }

    ShapeExtent captured_scalar(const Expr& expression, Environment& environment) {
        const auto found = scalar_values_.find(&expression);
        return found == scalar_values_.end() ? scalar(expression, environment) : found->second;
    }

    static std::vector<std::size_t> argument_indices(const std::vector<CallArg>& arguments,
                                                    const FunctionType* signature, std::size_t offset = 0) {
        std::vector<std::size_t> indices;
        std::size_t positional = offset;
        for (const auto& argument : arguments) {
            auto index = positional++;
            if (argument.name && signature) {
                for (std::size_t parameter = offset; parameter < signature->parameters.size(); ++parameter)
                    if (signature->parameters[parameter].name == *argument.name) { index = parameter; break; }
            }
            indices.push_back(index);
        }
        return indices;
    }

    static std::vector<Type> ordered_arguments(const std::vector<Type>& values,
                                               const std::vector<std::size_t>& indices,
                                               const FunctionType* signature,
                                               std::optional<Type> receiver = std::nullopt) {
        std::vector<Type> result(signature ? signature->parameters.size() : values.size() + (receiver ? 1 : 0),
                                 Type::simple(TypeKind::Invalid));
        if (receiver && !result.empty()) result.front() = *receiver;
        for (std::size_t argument = 0; argument < values.size(); ++argument)
            if (indices[argument] < result.size()) result[indices[argument]] = values[argument];
        return result;
    }

    static bool runs_code(const Expr& expression) {
        const auto& data = expression.data;
        if (std::holds_alternative<CallExpr>(data) || std::holds_alternative<MethodCallExpr>(data) ||
            std::holds_alternative<TryExpr>(data) || std::holds_alternative<IfExpr>(data) ||
            std::holds_alternative<ArrayComprehensionExpr>(data)) return true;
        const auto any = [](const ExprPtr& item) { return item && runs_code(*item); };
        if (const auto* member = std::get_if<MemberExpr>(&data)) return any(member->base);
        if (const auto* index = std::get_if<IndexExpr>(&data)) {
            if (any(index->base)) return true;
            for (const auto& item : index->items)
                if (any(item.index) || any(item.start) || any(item.stop) || any(item.step)) return true;
        }
        if (const auto* unary = std::get_if<UnaryExpr>(&data)) return any(unary->operand);
        if (const auto* binary = std::get_if<BinaryExpr>(&data)) return any(binary->left) || any(binary->right);
        if (const auto* text = std::get_if<StringTemplateExpr>(&data))
            for (const auto& item : text->expressions) if (any(item)) return true;
        if (const auto* array = std::get_if<ArrayExpr>(&data))
            for (const auto& item : array->elements) if (any(item)) return true;
        return false;
    }

    void forget_aliases(Environment& environment, const void* site) {
        std::size_t axis = 0;
        for (auto& [name, extent] : environment.scalars) {
            (void)name;
            extent = mutated(site, axis++);
        }
        for (auto& [name, type] : environment.tensors) {
            (void)name;
            type.tensor_known_shape_prefix.clear();
            for (long long dimension = 0; dimension < type.length; ++dimension) {
                if (const auto known = tensor_known_extent(type, static_cast<std::size_t>(dimension)))
                    type.tensor_known_shape_prefix.emplace_back(*known);
                else type.tensor_known_shape_prefix.push_back(mutated(site, axis++));
            }
        }
    }

    void invalidate_reference_targets(const Expr& call,Environment& environment) {
        const auto* summary=reference_write_summary(checked_,call);
        if(!summary || summary->reference_shared_writes.empty())return;
        std::set<std::string> names;
        for(const auto& [expression,_]:checked_.expr_types)
            if(const auto* name=std::get_if<NameExpr>(&expression->data);name && !name->this_qualifier && reference_call_writes(checked_,call,*expression))names.insert(name->name);
        std::size_t axis=0;
        for(const auto& name:names) {
            if(const auto scalar=environment.scalars.find(name);scalar!=environment.scalars.end())scalar->second=mutated(&call,axis++);
            if(const auto tensor=environment.tensors.find(name);tensor!=environment.tensors.end()) {
                tensor->second.tensor_known_shape_prefix.clear();
                for(long long index=0;index<tensor->second.length;++index) {
                    if(const auto known=tensor_known_extent(tensor->second,static_cast<std::size_t>(index)))tensor->second.tensor_known_shape_prefix.emplace_back(*known);
                    else tensor->second.tensor_known_shape_prefix.push_back(mutated(&call,axis++));
                }
            }
        }
    }

    void invalidate(const CallArg& argument, Environment& environment, const void* site,
                    const FunctionType* signature = nullptr, std::size_t parameter = 0) {
        if (!argument.writable || !argument.value) return;
        if (signature && parameter < signature->parameters.size()) {
            const auto& input = signature->parameters[parameter];
            if (input.is_const) return;
            if (!signature->external) {
                const auto effects = signature->reference_effects.find(input.name);
                bool replaces=effects!=signature->reference_effects.end() &&
                    (effects->second.writes.contains("") || effects->second.invalidates.contains(""));
                if(site && checked_.effects)if(const auto* summary=reference_write_summary(checked_,*static_cast<const Expr*>(site));summary && parameter<summary->exposed_writes.size())
                    replaces |= summary->exposed_writes[parameter].contains(StoragePath{StorageRoot::parameter(parameter),{}});
                if(!replaces)return;
            }
        }
        const auto* name = std::get_if<NameExpr>(&argument.value->data);
        if (!name || name->this_qualifier) {
            if (!environment.references.empty()) forget_aliases(environment, site);
            return;
        }
        if (!environment.references.empty()) {
            forget_aliases(environment, site);
            return;
        }
        environment.scalars[name->name] = mutated(site, 0);
        if (const auto found = environment.tensors.find(name->name); found != environment.tensors.end()) {
            found->second.tensor_known_shape_prefix.clear();
            for (long long axis = 0; axis < found->second.length; ++axis) {
                if (const auto known = tensor_known_extent(found->second, static_cast<std::size_t>(axis)))
                    found->second.tensor_known_shape_prefix.emplace_back(*known);
                else found->second.tensor_known_shape_prefix.push_back(mutated(site, static_cast<std::size_t>(axis) + 1));
            }
        }
    }

    const FunctionDecl* function_declaration(const std::string& name) const {
        for (const auto& declaration : checked_.program.functions)
            if (declaration.name == name) return &declaration;
        for (const auto& declaration : checked_.program.classes) {
            std::size_t constructors = 0;
            for (const auto& method : declaration.methods) {
                const auto current = method.is_constructor
                    ? member_function_name::constructor(declaration.name, constructors++)
                    : member_function_name::method(declaration.name, method.name);
                if (current == name) return &method;
            }
        }
        return nullptr;
    }

    ShapeExtent signature_scalar(const Expr& expression, Environment& environment) {
        if (const auto known = constant_eval::integer(expression); known && *known >= 0) return *known;
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            const auto found = environment.scalars.find(name->name);
            return !name->this_qualifier && found != environment.scalars.end() ? found->second : ShapeExtent{};
        }
        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data))
            return extent_arithmetic(binary->op, signature_scalar(*binary->left, environment),
                                     signature_scalar(*binary->right, environment));
        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            const auto operand = signature_scalar(*unary->operand, environment);
            if (unary->op == "+") return operand;
            if (unary->op == "-") return extent_arithmetic("-", ShapeExtent(0), operand);
        }
        if (const auto* call = std::get_if<CallExpr>(&expression.data))
            if (call->callee == "nat" && call->args.size() == 1)
                return signature_scalar(*call->args.front().value, environment);
        return {};
    }

    void call_signature(const Expr& expression, const FunctionDecl& declaration,
                        const FunctionType& signature, const std::vector<Type>& arguments,
                        const std::vector<ShapeExtent>& scalars, Type& result) {
        const auto unit = checked_.signature_shapes.find(&declaration);
        if (unit == checked_.signature_shapes.end()) return;
        const auto& plan = checked_.shape_plans.at(unit->second.source.get());
        const auto parameter_offset = signature.parameters.size() - declaration.parameters.size();
        Environment environment;
        for (std::size_t p = 0; p < signature.parameters.size(); ++p) {
            if (p < arguments.size() && arguments[p].kind == TypeKind::Tensor)
                environment.tensors[signature.parameters[p].name] = arguments[p];
            if (p < scalars.size()) environment.scalars[signature.parameters[p].name] = scalars[p];
        }
        check_call_shape(expression, unit->second, plan, arguments,
                         std::move(environment), parameter_offset, result);
    }

    void check_call_shape(const Expr& expression, const SignatureShapeUnit& unit,
                          const ShapePlan& plan, const std::vector<Type>& arguments,
                          Environment environment, std::size_t parameter_offset, Type& result) {
        for (const auto& part : unit.parts) {
            if (part.parameter == 0) continue;
            const auto p = part.parameter - 1 + parameter_offset;
            for (const auto& variable : plan.variables)
                if (!variable.value && variable.axis >= part.offset && variable.axis < part.offset + part.size)
                    environment.scalars[variable.name] = p < arguments.size()
                        ? pattern_extent(arguments[p], variable.axis - part.offset,
                                         part.size, part.ellipsis) : ShapeExtent{};
        }
        for (const auto& variable : plan.variables)
            if (variable.value && !plan.axes[variable.axis].after_initializer)
                environment.scalars[variable.name] = signature_scalar(*variable.value, environment);
        for (const auto axis : plan.after_order) {
            const auto& constraint = plan.axes[axis];
            if (!constraint.variable.empty() && constraint.variable_index < plan.variables.size() &&
                plan.variables[constraint.variable_index].axis == axis)
                environment.scalars[constraint.variable] = signature_scalar(*constraint.value, environment);
        }
        bool complete = true;
        std::vector<ProvenShapeConstraints> parameters(arguments.size());
        for (const auto& part : unit.parts) {
            const auto parameter = part.parameter == 0 ? 0 : part.parameter - 1 + parameter_offset;
            const Type value = part.parameter == 0 ? result :
                parameter < arguments.size() ? arguments[parameter] : Type::simple(TypeKind::Invalid);
            if (part.parameter != 0) {
                const bool rank_proven = value.kind == TypeKind::Tensor &&
                    pattern_rank_matches(value.length, part.size, part.ellipsis);
                complete = complete && rank_proven;
                if (parameter < parameters.size()) parameters[parameter].rank = rank_proven;
                if (value.kind == TypeKind::Tensor && value.length >= 0 &&
                    !rank_proven)
                    throw CompileError({"TENSOR_SHAPE", "Tensor argument rank does not match the signature.", expression.span});
            } else {
                if (!part.ellipsis) {
                    result.length = static_cast<long long>(part.size);
                    result.tensor_known_shape_prefix.resize(part.size);
                }
            }
            for (std::size_t axis = 0; axis < part.size; ++axis) {
                const auto& constraint = plan.axes[part.offset + axis];
                ShapeExtent expected;
                if (constraint.check == ShapeAxisCheck::Variable)
                    expected = environment.scalars.at(constraint.variable);
                else if (constraint.check == ShapeAxisCheck::Value)
                    expected = signature_scalar(*constraint.value, environment);
                if (part.parameter == 0) {
                    if (expected.kind != ShapeExtent::Kind::Unknown)
                        set_pattern_extent(result, axis, part.size, part.ellipsis, expected);
                    continue;
                }
                if (constraint.check == ShapeAxisCheck::None) {
                    if (parameter < parameters.size()) parameters[parameter].axes.push_back(true);
                    continue;
                }
                const auto actual = pattern_extent(value, axis, part.size, part.ellipsis);
                if (expected.known() && actual.known() && expected.value != actual.value)
                    throw CompileError({"TENSOR_SHAPE", "Tensor argument extent does not match the signature.", expression.span});
                const bool equal = same_extent(expected, actual);
                complete = complete && equal;
                if (parameter < parameters.size()) parameters[parameter].axes.push_back(equal);
            }
        }
        checked_.complete_signature_checks[&expression] = complete;
        checked_.proven_shape_constraints[&expression].parameters = std::move(parameters);
    }

    void capture_callable_shape(const std::string& name, const TypeName& syntax,
                                Environment& environment) {
        if (checked_.function_shape_units.contains(&syntax))
            environment.callable_shapes[name] = {&syntax, environment.scalars};
    }

    Type call_result(const Expr& expression, const std::string& name,
                     const std::vector<Type>& arguments,
                     const std::vector<ShapeExtent>& scalar_arguments = {}) {
        auto result = original(expression);
        const auto found = checked_.functions.find(name);
        if (found == checked_.functions.end()) return seed(result, &expression);
        if (!current_summary_.empty()) summary_dependencies_[current_summary_].insert(name);
        if (const auto declaration = function_declaration(name))
            call_signature(expression, *declaration, found->second, arguments, scalar_arguments, result);
        if (result.kind != TypeKind::Tensor || found->second.result_extents.empty())
            return seed(result, &expression);
        TensorShapeTransfer transfer;
        transfer.operation = TensorShapeOp::CallOrigins;
        transfer.result = result; transfer.inputs = arguments;
        transfer.scalar_inputs = scalar_arguments;
        transfer.result_origins = found->second.result_extents;
        transfer.known_result = original(expression);
        transfer.fresh = [&](std::size_t axis) { return fresh(&expression, axis); };
        return transfer_tensor_shape(transfer);
    }

    static std::vector<ShapeExtent> ordered_scalars(
        const std::vector<CallArg>& arguments, const std::vector<std::size_t>& indices,
        const FunctionType* signature, const std::unordered_map<const Expr*, ShapeExtent>& values) {
        std::vector<ShapeExtent> result(signature ? signature->parameters.size() : arguments.size());
        for (std::size_t argument = 0; argument < arguments.size(); ++argument) {
            const auto found = values.find(arguments[argument].value.get());
            if (indices[argument] < result.size() && found != values.end())
                result[indices[argument]] = found->second;
        }
        return result;
    }

    void bind_signature_facts(const FunctionDecl& declaration, const FunctionType& signature,
                              Environment& environment) {
        const auto unit = checked_.signature_shapes.find(&declaration);
        if (unit == checked_.signature_shapes.end()) return;
        const auto& plan = checked_.shape_plans.at(unit->second.source.get());
        const std::size_t parameter_offset = signature.parameters.size() - declaration.parameters.size();
        for (const auto& part : unit->second.parts) {
            if (part.parameter == 0) continue;
            const auto& input = signature.parameters.at(part.parameter - 1 + parameter_offset);
            for (const auto& variable : plan.variables)
                if (!variable.value && variable.axis >= part.offset && variable.axis < part.offset + part.size)
                    environment.scalars[variable.name] =
                        pattern_extent(environment.tensors.at(input.name), variable.axis - part.offset,
                                       part.size, part.ellipsis);
        }
        for (const auto& variable : plan.variables) {
            if (variable.value && !plan.axes[variable.axis].after_initializer)
                environment.scalars[variable.name] = scalar(*variable.value, environment);
        }
        for (const auto axis : plan.after_order) {
            const auto& constraint = plan.axes[axis];
            if (!constraint.variable.empty() && constraint.variable_index < plan.variables.size() &&
                plan.variables[constraint.variable_index].axis == axis)
                environment.scalars[constraint.variable] = scalar(*constraint.value, environment);
        }
        for (const auto& part : unit->second.parts) {
            std::vector<std::optional<ShapeExtent>> captured(part.size);
            for (std::size_t axis = 0; axis < part.size; ++axis) {
                const auto& constraint = plan.axes[part.offset + axis];
                if (constraint.check == ShapeAxisCheck::Variable)
                    captured[axis] = environment.scalars.at(constraint.variable);
                else if (constraint.check == ShapeAxisCheck::Value)
                    captured[axis] = scalar(*constraint.value, environment);
                else if (!constraint.variable.empty() && environment.scalars.contains(constraint.variable))
                    captured[axis] = environment.scalars.at(constraint.variable);
            }
            if (part.parameter == 0) {
                environment.result_contract = {std::move(captured), part.ellipsis};
                continue;
            }
            const auto& input = signature.parameters.at(part.parameter - 1 + parameter_offset);
            environment.contracts[input.name] = {captured, part.ellipsis};
            auto& tensor = environment.tensors.at(input.name);
            if (!part.ellipsis) {
                tensor.length = static_cast<long long>(part.size);
                tensor.tensor_known_shape_prefix.resize(part.size);
            }
            for (std::size_t axis = 0; axis < part.size; ++axis) {
                const auto& constraint = plan.axes[part.offset + axis];
                if (constraint.check == ShapeAxisCheck::Variable)
                    set_pattern_extent(tensor, axis, part.size, part.ellipsis,
                                       environment.scalars.at(constraint.variable));
                else if (constraint.check == ShapeAxisCheck::Value)
                    set_pattern_extent(tensor, axis, part.size, part.ellipsis,
                                       scalar(*constraint.value, environment));
            }
        }
    }

    Type expression_type(const Expr& expression, Environment& environment) {
        auto result = original(expression);
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (!name->this_qualifier) {
                if (const auto found = environment.tensors.find(name->name); found != environment.tensors.end())
                    result = found->second;
            }
        } else if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
            const auto left = expression_type(*binary->left, environment);
            const auto right = expression_type(*binary->right, environment);
            if (result.kind == TypeKind::Tensor) {
                TensorShapeTransfer transfer;
                const auto comparison = binary->op == "==" || binary->op == "!=" || binary->op == "<" ||
                    binary->op == "<=" || binary->op == ">" || binary->op == ">=";
                transfer.operation = comparison ? TensorShapeOp::Preserve : TensorShapeOp::Broadcast;
                transfer.result = result; transfer.inputs = {left, right};
                result = transfer_tensor_shape(transfer);
            }
        } else if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            const auto value = expression_type(*unary->operand, environment);
            if (result.kind == TypeKind::Tensor) {
                TensorShapeTransfer transfer; transfer.result = result; transfer.inputs = {value};
                result = transfer_tensor_shape(transfer);
            }
        } else if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            std::vector<Type> arguments;
            for (const auto& argument : call->args) arguments.push_back(expression_type(*argument.value, environment));
            for (std::size_t index = 0; index < call->args.size(); ++index) {
                if (!call->args[index].writable) continue;
                const auto* name = std::get_if<NameExpr>(&call->args[index].value->data);
                if (name && !name->this_qualifier && environment.tensors.contains(name->name))
                    arguments[index] = environment.tensors.at(name->name);
            }
            const auto resolution = checked_.call_resolutions.find(&expression);
            const FunctionType* signature = nullptr;
            if (resolution != checked_.call_resolutions.end()) {
                const auto found = checked_.functions.find(resolution->second.target);
                if (found != checked_.functions.end()) signature = &found->second;
            }
            const auto indices = argument_indices(call->args, signature);
            if (resolution != checked_.call_resolutions.end() && resolution->second.kind == CallKind::Function)
                result = call_result(expression, resolution->second.target, ordered_arguments(arguments, indices, signature),
                                     ordered_scalars(call->args, indices, signature, scalar_values_));
            else if (resolution != checked_.call_resolutions.end() &&
                     resolution->second.kind == CallKind::FunctionValue) {
                const auto callable = environment.callable_shapes.find(resolution->second.target);
                if (callable != environment.callable_shapes.end()) {
                    const auto& unit = checked_.function_shape_units.at(callable->second.type);
                    Environment captured;
                    captured.scalars = callable->second.scalars;
                    check_call_shape(expression, unit, checked_.shape_plans.at(unit.source.get()),
                                     arguments, std::move(captured), 0, result);
                }
                result = seed(result, &expression);
            }
            else if (result.kind == TypeKind::Tensor) {
                if (resolution != checked_.call_resolutions.end() && resolution->second.kind == CallKind::NumericCast &&
                    !arguments.empty() && arguments.front().kind == TypeKind::Tensor) {
                    TensorShapeTransfer transfer; transfer.result = result; transfer.inputs = {arguments.front()};
                    result = transfer_tensor_shape(transfer);
                } else if (resolution != checked_.call_resolutions.end() && resolution->second.builtin &&
                           (*resolution->second.builtin == BuiltinCallable::TensorCreate ||
                            *resolution->second.builtin == BuiltinCallable::TensorZeros ||
                            *resolution->second.builtin == BuiltinCallable::TensorOnes)) {
                    for (const auto& argument : call->args) {
                        if (const auto* shape = std::get_if<ArrayExpr>(&argument.value->data)) {
                            TensorShapeTransfer transfer; transfer.operation = TensorShapeOp::ExplicitShape;
                            transfer.result = result; transfer.explicit_shape.emplace();
                            for (const auto& extent : shape->elements)
                                transfer.explicit_shape->push_back(captured_scalar(*extent, environment));
                            result = transfer_tensor_shape(transfer);
                            break;
                        }
                    }
                }
            }
            for (std::size_t index = 0; index < call->args.size(); ++index)
                invalidate(call->args[index], environment, &expression, signature, indices[index]);
            invalidate_reference_targets(expression,environment);
        } else if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
            Type receiver;
            const bool receiver_first = runs_code(*method->receiver);
            if (receiver_first) receiver = expression_type(*method->receiver, environment);
            std::vector<Type> arguments;
            for (const auto& argument : method->args) arguments.push_back(expression_type(*argument.value, environment));
            if (!receiver_first) receiver = expression_type(*method->receiver, environment);
            const auto resolved = checked_.method_calls.find(&expression);
            const FunctionType* signature = nullptr;
            if (resolved != checked_.method_calls.end()) {
                const auto found = checked_.functions.find(resolved->second.internal_name);
                if (found != checked_.functions.end()) signature = &found->second;
            }
            const auto indices = argument_indices(method->args, signature, 1);
            if (resolved != checked_.method_calls.end())
                result = call_result(expression, resolved->second.internal_name,
                                     ordered_arguments(arguments, indices, signature, receiver),
                                     ordered_scalars(method->args, indices, signature, scalar_values_));
            else if (result.kind == TypeKind::Tensor) {
                if (method->method == "reshape" || method->method == "gather" || method->method == "scatter") {
                    const auto index = method->method == "reshape" ? 0U : 1U;
                    if (index < method->args.size()) {
                        if (const auto* shape = std::get_if<ArrayExpr>(&method->args[index].value->data)) {
                            TensorShapeTransfer transfer; transfer.operation = TensorShapeOp::ExplicitShape;
                            transfer.result = result; transfer.explicit_shape.emplace();
                            for (const auto& extent : shape->elements)
                                transfer.explicit_shape->push_back(captured_scalar(*extent, environment));
                            result = transfer_tensor_shape(transfer);
                        }
                    }
                } else {
                    TensorShapeTransfer transfer; transfer.result = result; transfer.inputs = {receiver};
                    if (method->method == "transpose" && method->args.size() == 2) {
                        transfer.operation = TensorShapeOp::Transpose;
                        const auto a = constant_eval::integer(*method->args[0].value);
                        const auto b = constant_eval::integer(*method->args[1].value);
                        if (a && b) transfer.axes = std::pair{*a, *b};
                    }
                    result = transfer_tensor_shape(transfer);
                }
            }
            for (std::size_t index = 0; index < method->args.size(); ++index)
                invalidate(method->args[index], environment, &expression, signature, indices[index]);
            invalidate_reference_targets(expression,environment);
        } else if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            const auto base = expression_type(*index->base, environment);
            if (base.kind == TypeKind::Tensor && result.kind == TypeKind::Tensor && base.length >= 0) {
                TensorShapeTransfer transfer; transfer.operation = TensorShapeOp::Index;
                transfer.result = result; transfer.inputs = {base}; transfer.known_result = original(expression);
                transfer.fresh = [&](std::size_t axis) { return fresh(&expression, axis); };
                for (const auto& item : index->items)
                    transfer.indices.push_back({!item.slice ? TensorShapeTransfer::IndexAxis::Drop :
                        (item.start || item.stop || item.step) ? TensorShapeTransfer::IndexAxis::Slice : TensorShapeTransfer::IndexAxis::Preserve});
                result = transfer_tensor_shape(transfer);
            }
            for (const auto& item : index->items) {
                if (item.index) expression_type(*item.index, environment);
                if (item.start) expression_type(*item.start, environment);
                if (item.stop) expression_type(*item.stop, environment);
                if (item.step) expression_type(*item.step, environment);
            }
        } else if (const auto* array = std::get_if<TupleExpr>(&expression.data)) {
            for (const auto& item : array->elements) expression_type(*item, environment);
        } else if (const auto* array = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
            expression_type(*array->source, environment);
            const auto before = environment;
            const auto source_type = checked_.expr_types.at(array->source.get());
            if (source_type.first && source_type.first->kind == TypeKind::Tensor)
                environment.tensors[array->name] = *source_type.first;
            if (array->condition) expression_type(*array->condition, environment);
            expression_type(*array->value, environment);
            environment = join(&expression, before, {before, environment}, true);
        } else if (const auto* array = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : array->elements) expression_type(*item, environment);
        } else if (const auto* literal = std::get_if<LiteralConstantExpr>(&expression.data)) {
            expression_type(*literal->value, environment);
        } else if (const auto* text = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : text->expressions) expression_type(*item, environment);
        } else if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
            expression_type(*member->base, environment);
        } else if (const auto* tried = std::get_if<TryExpr>(&expression.data)) {
            const auto value = expression_type(*tried->value, environment);
            if (result.kind == TypeKind::Tensor && value.kind == TypeKind::Tensor) {
                result.length = value.length;
                result.tensor_known_shape_prefix = value.tensor_known_shape_prefix;
            }
        } else if (const auto* choice = std::get_if<IfExpr>(&expression.data)) {
            std::vector<Environment> branches;
            std::vector<Type> values;
            for (std::size_t i = 0; i < choice->conditions.size(); ++i) {
                expression_type(*choice->conditions[i], environment);
                auto branch = environment;
                values.push_back(expression_type(*choice->values[i], branch));
                branches.push_back(std::move(branch));
            }
            auto otherwise = environment;
            values.push_back(expression_type(*choice->otherwise, otherwise));
            branches.push_back(std::move(otherwise));
            environment = join(&expression, environment, branches);
            if (result.kind == TypeKind::Tensor) result = join_types(&expression, "$result", result, values);
        }
        result = seed(std::move(result), &expression);
        record(expression, result);
        if (is_integer_family_type(result)) scalar_values_[&expression] = scalar(expression, environment);
        return result;
    }

    bool from_loop(const ShapeExtent& extent) const {
        const auto nested = [&](std::int64_t id) {
            const auto found = symbol_depths_.find(id);
            return found != symbol_depths_.end() && found->second > loop_depth_;
        };
        if (extent.kind == ShapeExtent::Kind::Symbol) return nested(extent.value);
        return (extent.kind == ShapeExtent::Kind::Broadcast || extent.kind == ShapeExtent::Kind::Expr) &&
            std::any_of(extent.symbols.begin(), extent.symbols.end(), nested);
    }

    Type join_types(const void* site, const std::string& name, Type base, const std::vector<Type>& states,
                    bool header = false) {
        return ir::facts::merge_tensor_shapes(std::move(base), states,
            [&](std::size_t axis) { return merge_symbol(site, name, axis); },
            [&](const ShapeExtent& extent) { return !header || !from_loop(extent); });
    }

    Environment join(const void* site, const Environment& before, const std::vector<Environment>& states,
                     bool header = false) {
        auto result = before;
        if (states.empty()) return result;
        for (const auto& state : states)
            result.references.insert(state.references.begin(), state.references.end());
        for (auto& [name, type] : result.tensors) {
            std::vector<Type> values;
            for (const auto& state : states) {
                const auto found = state.tensors.find(name);
                values.push_back(found == state.tensors.end() ? type : found->second);
            }
            type = join_types(site, name, type, values, header);
        }
        for (auto& [name, extent] : result.scalars) {
            if (const auto first = states.front().scalars.find(name); first != states.front().scalars.end())
                extent = first->second;
            bool equal = true;
            for (const auto& state : states) {
                const auto found = state.scalars.find(name);
                equal = equal && found != state.scalars.end() && same_extent(extent, found->second) &&
                    (!header || !from_loop(found->second));
            }
            if (!equal) extent = merge_symbol(site, "$scalar." + name, 0);
        }
        return result;
    }

    static bool equivalent(const Environment& left, const Environment& right) {
        if (left.scalars != right.scalars || left.tensors.size() != right.tensors.size()) return false;
        for (const auto& [name, type] : left.tensors) {
            const auto found = right.tensors.find(name);
            if (found == right.tensors.end() || type.length != found->second.length ||
                type.tensor_known_shape_prefix != found->second.tensor_known_shape_prefix) return false;
        }
        return true;
    }

    void loop(const Stmt& statement, const std::vector<StmtPtr>& body, Environment& environment,
              const Expr* condition = nullptr) {
        const auto entry = environment;
        auto header = environment;
        Environment back;
        for (;;) {
            back = header;
            ++loop_depth_;
            if (condition) expression_type(*condition, back);
            block(body, back);
            --loop_depth_;
            const auto next = join(&statement, entry, {entry, back}, true);
            if (equivalent(header, next)) break;
            header = next;
        }
        environment = join(&statement, entry, {entry, back}, true);
        if (condition) expression_type(*condition, environment);
    }

    std::vector<std::optional<ShapeExtent>> capture_array_contract(
        const TypeName& source, Environment& environment) {
        std::vector<std::optional<ShapeExtent>> captured;
        for (const auto& extent : source.dimension_expressions) {
            if (!extent) { captured.push_back(std::nullopt); continue; }
            expression_type(*extent, environment);
            captured.push_back(captured_scalar(*extent, environment));
        }
        return captured;
    }

    void prove_contract(const Expr& expression, const Type& value,
                        const std::vector<std::optional<ShapeExtent>>& captured,
                        std::optional<std::size_t> ellipsis = std::nullopt) {
        if (captured.empty()) return;
        if (value.kind == TypeKind::Array) {
            auto& proof = checked_.proven_shape_constraints[&expression];
            proof.axes.clear();
            const Type* current = &value;
            for (const auto& expected : captured) {
                const auto actual = current && current->kind == TypeKind::Array && current->length >= 0
                    ? ShapeExtent(current->length) : ShapeExtent{};
                proof.axes.push_back(!expected || same_extent(*expected, actual));
                current = current && current->first ? current->first.get() : nullptr;
            }
            return;
        }
        if (value.kind != TypeKind::Tensor) return;
        auto& proof = checked_.proven_shape_constraints[&expression];
        proof.axes.clear();
        proof.rank = pattern_rank_matches(value.length, captured.size(), ellipsis);
        if (value.length >= 0 && !proof.rank)
            throw CompileError({"TENSOR_SHAPE", "Tensor rank does not match its declared shape.", expression.span});
        for (std::size_t axis = 0; axis < captured.size(); ++axis) {
            const auto actual = pattern_extent(value, axis, captured.size(), ellipsis);
            if (captured[axis] && captured[axis]->known() && actual.known() &&
                captured[axis]->value != actual.value)
                throw CompileError({"TENSOR_SHAPE", "Tensor extent does not match its declared shape.", expression.span});
            proof.axes.push_back(!captured[axis] || same_extent(*captured[axis], actual));
        }
    }

    bool block(const std::vector<StmtPtr>& body, Environment& environment) {
        for (const auto& statement : body) {
            if (const auto* binding = std::get_if<BindingStmt>(&statement->data)) {
                if (binding->reference) environment.references.insert(binding->name);
                capture_callable_shape(binding->name, binding->declared_type, environment);
                const auto plan = checked_.shape_plans.find(&binding->declared_type);
                std::vector<std::optional<ShapeExtent>> captured;
                if (plan != checked_.shape_plans.end()) {
                    captured.resize(plan->second.axes.size());
                    for (std::size_t axis = 0; axis < captured.size(); ++axis) {
                        const auto& constraint = plan->second.axes[axis];
                        if (!constraint.value || constraint.after_initializer) continue;
                        expression_type(*constraint.value, environment);
                        captured[axis] = captured_scalar(*constraint.value, environment);
                    }
                    for (const auto& variable : plan->second.variables)
                        if (variable.value && captured[variable.axis])
                            environment.scalars[variable.name] = *captured[variable.axis];
                }
                auto array_captured = capture_array_contract(binding->declared_type, environment);
                auto type = original_bindings_.contains(statement.get())
                    ? original_bindings_.at(statement.get()) : Type::simple(TypeKind::Invalid);
                if (binding->value) {
                    const auto value = expression_type(*binding->value, environment);
                    if (type.kind == TypeKind::Tensor && value.kind == TypeKind::Tensor) {
                        type.length = value.length;
                        type.tensor_known_shape_prefix = value.tensor_known_shape_prefix;
                    }
                    if (is_integer_family_type(type)) environment.scalars[binding->name] = captured_scalar(*binding->value, environment);
                    if (type.kind == TypeKind::Array) {
                        prove_contract(*binding->value, value, array_captured);
                        environment.array_contracts[binding->name] = std::move(array_captured);
                    }
                }
                if (type.kind == TypeKind::Tensor) {
                    environment.tensors[binding->name] = seed(type, statement.get());
                    if (plan != checked_.shape_plans.end()) {
                        auto& value = environment.tensors[binding->name];
                        if (!plan->second.ellipsis)
                            value.length = static_cast<long long>(plan->second.axes.size());
                        value = seed(value, statement.get());
                        for (const auto& variable : plan->second.variables) {
                            if (variable.value) continue;
                            const auto extent = pattern_extent(value, variable.axis,
                                plan->second.axes.size(), plan->second.ellipsis);
                            environment.scalars[variable.name] = extent;
                            // The declaring axis also captures the initial value;
                            // null would incorrectly prove later stores unconstrained.
                            captured[variable.axis] = extent;
                        }
                        for (const auto axis : plan->second.after_order) {
                            const auto& constraint = plan->second.axes[axis];
                            expression_type(*constraint.value, environment);
                            captured[axis] = captured_scalar(*constraint.value, environment);
                            for (const auto& variable : plan->second.variables)
                                if (variable.axis == axis && variable.value)
                                    environment.scalars[variable.name] = *captured[axis];
                        }
                        for (std::size_t axis = 0; axis < plan->second.axes.size(); ++axis) {
                            const auto& constraint = plan->second.axes[axis];
                            if (constraint.check == ShapeAxisCheck::Variable && environment.scalars.contains(constraint.variable))
                                captured[axis] = environment.scalars.at(constraint.variable);
                        }
                        if (binding->value)
                            prove_contract(*binding->value, type, captured, plan->second.ellipsis);
                        for (std::size_t axis = 0; axis < captured.size(); ++axis)
                            if (captured[axis])
                                set_pattern_extent(value, axis, captured.size(), plan->second.ellipsis,
                                                   *captured[axis]);
                        environment.contracts[binding->name] = {captured, plan->second.ellipsis};
                    }
                    auto& recorded = checked_.binding_types[statement.get()];
                    recorded.length = environment.tensors[binding->name].length;
                    recorded.tensor_known_shape_prefix = environment.tensors[binding->name].tensor_known_shape_prefix;
                }
            } else if (const auto* assignment = std::get_if<AssignStmt>(&statement->data)) {
                const auto previous = expression_type(*assignment->target, environment);
                auto value = expression_type(*assignment->value, environment);
                if (!assignment->compound_op.empty() && previous.kind == TypeKind::Tensor) {
                    const auto right = value;
                    value = previous;
                    if (right.kind == TypeKind::Tensor) {
                        TensorShapeTransfer transfer; transfer.operation = TensorShapeOp::Broadcast;
                        transfer.result = value; transfer.inputs = {previous, right};
                        value = transfer_tensor_shape(transfer);
                    }
                }
                if (const auto* name = std::get_if<NameExpr>(&assignment->target->data); name && !name->this_qualifier) {
                    if (!environment.references.empty()) forget_aliases(environment, statement.get());
                    if (value.kind == TypeKind::Tensor && !environment.references.contains(name->name)) {
                        auto& target = environment.tensors[name->name];
                        target.length = value.length;
                        target.tensor_known_shape_prefix = value.tensor_known_shape_prefix;
                        if (const auto contract = environment.contracts.find(name->name);
                            contract != environment.contracts.end()) {
                            const auto& captured = contract->second.extents;
                            prove_contract(*assignment->value, value, captured, contract->second.ellipsis);
                            if (!contract->second.ellipsis)
                                target.length = static_cast<long long>(captured.size());
                            target = seed(target, statement.get());
                            for (std::size_t axis = 0; axis < captured.size(); ++axis)
                                if (captured[axis])
                                    set_pattern_extent(target, axis, captured.size(), contract->second.ellipsis,
                                                       *captured[axis]);
                        }
                    }
                    else if (value.kind == TypeKind::Array) {
                        if (const auto contract = environment.array_contracts.find(name->name);
                            contract != environment.array_contracts.end())
                            prove_contract(*assignment->value, value, contract->second);
                    }
                    else if (is_integer_family_type(original(*assignment->target)))
                        environment.scalars[name->name] = assignment->compound_op.empty()
                            ? captured_scalar(*assignment->value, environment) : fresh(statement.get(), 0);
                } else if (runs_code(*assignment->target)) {
                    // An address may be resolved again after the RHS; retain no
                    // alias-sensitive relation across an effectful target.
                    forget_aliases(environment, statement.get());
                }
            } else if (const auto* rebound = std::get_if<RebindStmt>(&statement->data)) {
                expression_type(*rebound->target, environment);
                environment.references.insert(rebound->name);
                forget_aliases(environment, statement.get());
            } else if (const auto* returned = std::get_if<ReturnStmt>(&statement->data)) {
                if (returned->value) {
                    const auto value = expression_type(*returned->value, environment);
                    prove_contract(*returned->value, value, environment.result_contract.extents,
                                   environment.result_contract.ellipsis);
                    returns_[statement.get()] = value;
                }
                return false;
            } else if (const auto* expression = std::get_if<ExprStmt>(&statement->data)) {
                expression_type(*expression->value, environment);
            } else if (const auto* branch = std::get_if<IfStmt>(&statement->data)) {
                expression_type(*branch->condition, environment);
                auto yes = environment;
                auto no = environment;
                std::vector<Environment> continuing;
                if (block(branch->then_body, yes)) continuing.push_back(std::move(yes));
                if (block(branch->else_body, no)) continuing.push_back(std::move(no));
                if (continuing.empty()) return false;
                environment = join(statement.get(), environment, continuing);
            } else if (const auto* iteration = std::get_if<WhileStmt>(&statement->data)) {
                loop(*statement, iteration->body, environment, iteration->condition.get());
            } else if (const auto* iteration = std::get_if<ForStmt>(&statement->data)) {
                visit_for_header(*iteration,[&](const Expr& source){expression_type(source,environment);});
                loop(*statement, iteration->body, environment);
            } else if (const auto* match = std::get_if<MatchStmt>(&statement->data)) {
                const auto value = expression_type(*match->value, environment);
                std::vector<Environment> continuing;
                for (const auto& arm : match->cases) {
                    auto branch = environment;
                    auto type = value;
                    if (const auto found = checked_.case_types.find(&arm); found != checked_.case_types.end())
                        type = found->second;
                    if (type.kind == TypeKind::Tensor) {
                        const auto plan = checked_.shape_plans.find(&arm.type);
                        if (value.kind == TypeKind::Tensor) {
                            type.tensor_known_shape_prefix = value.tensor_known_shape_prefix;
                            if (plan != checked_.shape_plans.end() && plan->second.ellipsis &&
                                pattern_rank_matches(value.length, plan->second.axes.size(), plan->second.ellipsis))
                                type.length = value.length;
                        }
                        type = seed(type, &arm);
                        if (arm.binder) branch.tensors[*arm.binder] = type;
                        if (plan != checked_.shape_plans.end())
                            for (const auto& variable : plan->second.variables)
                                branch.scalars[variable.name] = pattern_extent(type, variable.axis,
                                    plan->second.axes.size(), plan->second.ellipsis);
                    }
                    if (block(arm.body, branch)) continuing.push_back(std::move(branch));
                }
                if (continuing.empty()) return false;
                environment = join(statement.get(), environment, continuing);
            } else if (std::holds_alternative<LoopControlStmt>(statement->data)) {
                return false;
            } else if (const auto* guard = std::get_if<MainGuardStmt>(&statement->data)) {
                if (guard->active) block(guard->body, environment);
            }
        }
        return true;
    }

    ExtentOrigin origin(const ShapeExtent& extent) const {
        ExtentOrigin result;
        if (extent.kind == ShapeExtent::Kind::Known) {
            result.kind = ExtentOrigin::Kind::Known;
            result.value = extent.value;
        } else if (extent.kind == ShapeExtent::Kind::Symbol) {
            if (const auto axis = parameter_axes_.find(extent.value); axis != parameter_axes_.end()) {
                result.kind = ExtentOrigin::Kind::ParameterAxis;
                result.parameter = axis->second.first;
                result.axis = axis->second.second;
            } else if (const auto parameter = parameter_values_.find(extent.value); parameter != parameter_values_.end()) {
                result.kind = ExtentOrigin::Kind::ParameterValue;
                result.parameter = parameter->second;
            }
        } else if (extent.kind == ShapeExtent::Kind::Expr ||
                   (extent.kind == ShapeExtent::Kind::Broadcast && !extent.operands.empty())) {
            result.kind = ExtentOrigin::Kind::Expr;
            result.expression = extent;
            for (const auto symbol : extent.symbols) {
                if (const auto axis = parameter_axes_.find(symbol); axis != parameter_axes_.end())
                    result.expression_axes.emplace(symbol, axis->second);
                else if (const auto parameter = parameter_values_.find(symbol); parameter != parameter_values_.end())
                    result.expression_axes.emplace(symbol, std::pair{parameter->second, std::numeric_limits<std::size_t>::max()});
                else result.local = true;
            }
            if (result.local) return ExtentOrigin{};
        } else if (extent.kind == ShapeExtent::Kind::Broadcast) {
            result.kind = ExtentOrigin::Kind::Broadcast;
            for (const auto symbol : extent.symbols) {
                if (const auto axis = parameter_axes_.find(symbol); axis != parameter_axes_.end())
                    result.axes.push_back(axis->second);
                else result.local = true;
            }
            std::sort(result.axes.begin(), result.axes.end());
            result.axes.erase(std::unique(result.axes.begin(), result.axes.end()), result.axes.end());
        }
        return result;
    }

    bool analyze(const std::string& name, const FunctionDecl& declaration) {
        const auto signature = checked_.functions.find(name);
        if (signature == checked_.functions.end()) return false;
        reset();
        current_summary_ = name;
        Environment environment;
        for (std::size_t parameter = 0; parameter < signature->second.parameters.size(); ++parameter) {
            const auto& input = signature->second.parameters[parameter];
            if (input.writable) environment.references.insert(input.name);
            if (input.type.kind == TypeKind::Tensor) {
                auto type = seed(input.type, &input);
                for (std::size_t axis = 0; axis < type.tensor_known_shape_prefix.size(); ++axis) {
                    const auto& extent = type.tensor_known_shape_prefix[axis];
                    if (extent.kind == ShapeExtent::Kind::Symbol) parameter_axes_[extent.value] = {parameter, axis};
                }
                environment.tensors[input.name] = std::move(type);
            } else if (is_integer_family_type(input.type)) {
                auto extent = fresh(&input, 0);
                parameter_values_[extent.value] = parameter;
                environment.scalars[input.name] = std::move(extent);
            }
        }
        bind_signature_facts(declaration, signature->second, environment);
        for (const auto& parameter : declaration.parameters)
            if (!parameter.type.dimension_expressions.empty())
                environment.array_contracts[parameter.name] = capture_array_contract(parameter.type, environment);
        if (signature->second.result.kind == TypeKind::Array)
            environment.result_contract.extents = capture_array_contract(declaration.return_type, environment);
        for (const auto& parameter : declaration.parameters)
            capture_callable_shape(parameter.name, parameter.type, environment);
        block(declaration.body, environment);
        std::vector<ExtentOrigin> summary;
        bool seen = false;
        for (const auto& [statement, type] : returns_) {
            (void)statement;
            if (type.kind != TypeKind::Tensor) continue;
            if (type.length < 0) { summary.clear(); break; }
            std::vector<ExtentOrigin> current;
            for (long long axis = 0; axis < type.length; ++axis)
                current.push_back(origin(tensor_extent(type, static_cast<std::size_t>(axis))));
            if (!seen) {
                summary = std::move(current);
                seen = true;
            } else if (summary.size() != current.size()) {
                summary.clear();
                break;
            } else {
                for (std::size_t axis = 0; axis < summary.size(); ++axis)
                    if (summary[axis] != current[axis]) summary[axis] = {};
            }
        }
        // Recursive expression summaries can grow without a finite bound.
        // Once an axis changes expression, keep its conservative unknown.
        auto& widened = widened_axes_[name];
        if (recursive_summary(name)) {
            const auto& previous = signature->second.result_extents;
            for (std::size_t axis = 0; axis < std::min(previous.size(), summary.size()); ++axis)
                if (previous[axis] != summary[axis] &&
                    (previous[axis].kind == ExtentOrigin::Kind::Expr ||
                     summary[axis].kind == ExtentOrigin::Kind::Expr)) widened.insert(axis);
        }
        for (const auto axis : widened) if (axis < summary.size()) summary[axis] = {};
        current_summary_.clear();
        const bool changed = signature->second.result_extents != summary;
        signature->second.result_extents = std::move(summary);
        return changed;
    }
};

} // namespace

void summarize_tensor_extents(CheckedProgram& checked) {
    ExtentAnalysis(checked).run();
}

} // namespace quidra::semantics
