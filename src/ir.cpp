#include "quidra/ir.hpp"
#include "quidra/language.hpp"
#include "operator_policy.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace quidra::ir {
namespace {

struct Lowerer {
    const CheckedProgram& checked;
    Module module;
    Function* fn{};
    Block* block{};
    ValueId next_value{1};
    std::uint32_t next_label{};
    std::uint32_t next_hidden{};
    std::unordered_map<std::string, Type> locals;
    std::unordered_map<std::string, std::string> local_names;
    std::unordered_map<std::string, std::string> reference_names;
    std::unordered_map<std::string, Type> references;
    std::vector<std::pair<std::string,std::string>> loop_targets;
    std::string current_class;
    const Expr* repl_expression{};
    std::size_t repl_replay_prefix_offset{};
    std::unordered_map<std::string, std::unordered_set<std::size_t>> borrowed_parameters;
    std::unordered_set<std::string> fully_initialized_array_locals;
    std::unordered_map<std::string, std::vector<std::optional<std::string>>> shaped_constraints;
    std::unordered_map<std::string, std::vector<std::optional<std::string>>> array_constraints;
    std::unordered_map<const Expr*, std::vector<std::optional<std::string>>> contextual_tensor_shapes;
    std::vector<std::optional<std::string>> return_shaped_constraints;
    std::vector<std::optional<std::string>> return_array_constraints;

    explicit Lowerer(
        const CheckedProgram& c, const Expr* repl = nullptr,
        std::size_t replay_prefix_offset = 0)
        : checked(c), repl_expression(repl),
          repl_replay_prefix_offset(replay_prefix_offset) {
        classify_borrowed_parameters();
    }

    bool array_expression_fully_initialized(const Expr& expression) const {
        const auto type = type_of(expression);
        if (type.kind != TypeKind::Array) return false;
        if (std::holds_alternative<ArrayExpr>(expression.data)) return true;
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (checked.field_accesses.contains(&expression) || is_source_reference(name->name)) {
                return false;
            }
            return fully_initialized_array_locals.contains(name->name);
        }
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            const auto found = checked.call_resolutions.find(&expression);
            return found != checked.call_resolutions.end() &&
                   found->second.kind == CallKind::Builtin &&
                   found->second.builtin == BuiltinCallable::Array &&
                   call->args.size() == 2;
        }
        if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
            const auto receiver_type = type_of(*method->receiver);
            if (receiver_type.kind == TypeKind::Array &&
                (method->method == "append" || method->method == "concat" ||
                 method->method == "sorted")) {
                return true;
            }
            if (receiver_type.kind == TypeKind::String &&
                (method->method == "split" || method->method == "codepoints")) {
                return true;
            }
        }
        return false;
    }

    std::optional<std::string> full_array_written_by_for(const ForStmt& loop) const {
        if (loop.writable || loop.body.size() != 1) return std::nullopt;

        const auto* range = std::get_if<CallExpr>(&loop.iterable->data);
        const auto range_resolution = checked.call_resolutions.find(loop.iterable.get());
        if (!range || range_resolution == checked.call_resolutions.end() ||
            range_resolution->second.kind != CallKind::Builtin ||
            range_resolution->second.builtin != BuiltinCallable::Range) {
            return std::nullopt;
        }
        for (const auto& argument : range->args) {
            if (argument.name || argument.writable) return std::nullopt;
        }

        const Expr* limit = nullptr;
        if (range->args.size() == 1) {
            limit = range->args[0].value.get();
        } else if (range->args.size() == 2) {
            const auto* zero = std::get_if<IntegerExpr>(&range->args[0].value->data);
            if (!zero || zero->value != 0) return std::nullopt;
            limit = range->args[1].value.get();
        } else {
            return std::nullopt;
        }

        const auto* assignment = std::get_if<AssignStmt>(&loop.body.front()->data);
        if (!assignment || !assignment->compound_op.empty()) return std::nullopt;
        const auto* indexed = std::get_if<IndexExpr>(&assignment->target->data);
        if (!indexed || indexed->items.size() != 1 || !indexed->items[0].index ||
            indexed->items[0].start || indexed->items[0].stop || indexed->items[0].step) {
            return std::nullopt;
        }
        const auto* array_name = std::get_if<NameExpr>(&indexed->base->data);
        const auto* index_name = std::get_if<NameExpr>(&indexed->items[0].index->data);
        if (!array_name || !index_name || index_name->name != loop.name ||
            !local_names.contains(array_name->name) ||
            is_source_reference(array_name->name) ||
            type_of(*indexed->base).kind != TypeKind::Array ||
            expression_contains_writable_argument(*assignment->value)) {
            return std::nullopt;
        }

        const auto* len_call = std::get_if<CallExpr>(&limit->data);
        const auto len_resolution = checked.call_resolutions.find(limit);
        if (!len_call || len_call->args.size() != 1 ||
            len_call->args[0].name || len_call->args[0].writable ||
            len_resolution == checked.call_resolutions.end() ||
            len_resolution->second.kind != CallKind::Builtin ||
            len_resolution->second.builtin != BuiltinCallable::Len) {
            return std::nullopt;
        }
        const auto* len_name = std::get_if<NameExpr>(&len_call->args[0].value->data);
        if (!len_name || len_name->name != array_name->name) return std::nullopt;
        return array_name->name;
    }

    bool expression_contains_writable_argument(const Expr& expression) const {
        if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            for (const auto& arg : node->args) {
                if (arg.writable || expression_contains_writable_argument(*arg.value)) return true;
            }
            return false;
        }
        if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            if (expression_contains_writable_argument(*node->receiver)) return true;
            for (const auto& arg : node->args) {
                if (arg.writable || expression_contains_writable_argument(*arg.value)) return true;
            }
            return false;
        }
        if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
            return expression_contains_writable_argument(*node->left) ||
                   expression_contains_writable_argument(*node->right);
        }
        if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
            return expression_contains_writable_argument(*node->operand);
        }
        if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
            if (expression_contains_writable_argument(*node->base)) return true;
            for (const auto& item : node->items) {
                if ((item.index && expression_contains_writable_argument(*item.index)) ||
                    (item.start && expression_contains_writable_argument(*item.start)) ||
                    (item.stop && expression_contains_writable_argument(*item.stop)) ||
                    (item.step && expression_contains_writable_argument(*item.step))) return true;
            }
            return false;
        }
        if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
            return expression_contains_writable_argument(*node->base);
        }
        if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : node->elements)
                if (expression_contains_writable_argument(*item)) return true;
            return false;
        }
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions)
                if (expression_contains_writable_argument(*item)) return true;
            return false;
        }
        if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            return expression_contains_writable_argument(*node->value);
        }
        return false;
    }

    static std::unordered_set<std::string> intersect_full_arrays(
        const std::unordered_set<std::string>& left,
        const std::unordered_set<std::string>& right) {
        std::unordered_set<std::string> result;
        const auto& small = left.size() <= right.size() ? left : right;
        const auto& large = left.size() <= right.size() ? right : left;
        for (const auto& name : small) if (large.contains(name)) result.insert(name);
        return result;
    }

    bool storage_root_is(const Expr& expression, const std::string& name) const {
        if (const auto* node = std::get_if<NameExpr>(&expression.data)) return node->name == name;
        if (const auto* node = std::get_if<MemberExpr>(&expression.data)) return storage_root_is(*node->base, name);
        if (const auto* node = std::get_if<IndexExpr>(&expression.data)) return storage_root_is(*node->base, name);
        return false;
    }

    bool expression_mutates_parameter(const Expr& expression, const std::string& name) const {
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) if (expression_mutates_parameter(*item, name)) return true;
            return false;
        }
        if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : node->elements) if (expression_mutates_parameter(*item, name)) return true;
            return false;
        }
        if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
            if (expression_mutates_parameter(*node->base, name)) return true;
            for (const auto& item : node->items) {
                if (item.index && expression_mutates_parameter(*item.index, name)) return true;
                if (item.start && expression_mutates_parameter(*item.start, name)) return true;
                if (item.stop && expression_mutates_parameter(*item.stop, name)) return true;
                if (item.step && expression_mutates_parameter(*item.step, name)) return true;
            }
            return false;
        }
        if (const auto* node = std::get_if<MemberExpr>(&expression.data))
            return expression_mutates_parameter(*node->base, name);
        if (const auto* node = std::get_if<UnaryExpr>(&expression.data))
            return expression_mutates_parameter(*node->operand, name);
        if (const auto* node = std::get_if<BinaryExpr>(&expression.data))
            return expression_mutates_parameter(*node->left, name) || expression_mutates_parameter(*node->right, name);
        if (const auto* node = std::get_if<TryExpr>(&expression.data))
            return expression_mutates_parameter(*node->value, name);
        if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            for (const auto& argument : node->args) {
                if (argument.writable && storage_root_is(*argument.value, name)) return true;
                if (expression_mutates_parameter(*argument.value, name)) return true;
            }
            return false;
        }
        if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            if (storage_root_is(*node->receiver, name)) {
                if (const auto call = checked.method_calls.find(&expression); call != checked.method_calls.end()) {
                    const auto signature = checked.functions.find(call->second.internal_name);
                    if (signature != checked.functions.end()) {
                        const auto& effect = signature->second.receiver_effect;
                        if (!effect.writes.empty() || !effect.initializes.empty() || !effect.invalidates.empty()) return true;
                    }
                }
            }
            if (expression_mutates_parameter(*node->receiver, name)) return true;
            for (const auto& argument : node->args) {
                if (argument.writable && storage_root_is(*argument.value, name)) return true;
                if (expression_mutates_parameter(*argument.value, name)) return true;
            }
        }
        return false;
    }

    bool block_mutates_parameter(const std::vector<StmtPtr>& body, const std::string& name) const {
        for (const auto& statement : body) {
            const auto& data = statement->data;
            if (const auto* node = std::get_if<BindingStmt>(&data)) {
                if (node->value) {
                    if ((node->reference || node->reference_initializer) && storage_root_is(*node->value, name)) return true;
                    if (expression_mutates_parameter(*node->value, name)) return true;
                }
            } else if (const auto* node = std::get_if<AssignStmt>(&data)) {
                if (storage_root_is(*node->target, name)) return true;
                if (expression_mutates_parameter(*node->target, name) || expression_mutates_parameter(*node->value, name)) return true;
            } else if (const auto* node = std::get_if<RebindStmt>(&data)) {
                if (storage_root_is(*node->target, name) || expression_mutates_parameter(*node->target, name)) return true;
            } else if (const auto* node = std::get_if<ReturnStmt>(&data)) {
                if (node->value && expression_mutates_parameter(*node->value, name)) return true;
            } else if (const auto* node = std::get_if<ExprStmt>(&data)) {
                if (expression_mutates_parameter(*node->value, name)) return true;
            } else if (const auto* node = std::get_if<IfStmt>(&data)) {
                if (expression_mutates_parameter(*node->condition, name) || block_mutates_parameter(node->then_body, name) || block_mutates_parameter(node->else_body, name)) return true;
            } else if (const auto* node = std::get_if<WhileStmt>(&data)) {
                if (expression_mutates_parameter(*node->condition, name) || block_mutates_parameter(node->body, name)) return true;
            } else if (const auto* node = std::get_if<ForStmt>(&data)) {
                if ((node->writable && storage_root_is(*node->iterable, name)) || expression_mutates_parameter(*node->iterable, name) || block_mutates_parameter(node->body, name)) return true;
            } else if (const auto* node = std::get_if<MatchStmt>(&data)) {
                if (expression_mutates_parameter(*node->value, name)) return true;
                for (const auto& match_case : node->cases) if (block_mutates_parameter(match_case.body, name)) return true;
            }
        }
        return false;
    }

    void classify_borrowed_parameters() {
        const auto classify = [&](const std::string& internal_name, const FunctionDecl& declaration, std::size_t offset) {
            const auto signature = checked.functions.find(internal_name);
            if (signature == checked.functions.end()) return;
            for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
                const auto signature_index = i + offset;
                if (signature_index >= signature->second.parameters.size()) continue;
                const auto& parameter = signature->second.parameters[signature_index];
                if (parameter.writable || !requires_value_clone(parameter.type)) continue;
                if (!block_mutates_parameter(declaration.body, declaration.parameters[i].name))
                    borrowed_parameters[internal_name].insert(signature_index);
            }
        };
        for (const auto& function : checked.program.functions) classify(function.name, function, 0);
        for (const auto& class_decl : checked.program.classes)
            for (const auto& method : class_decl.methods)
                classify("$method." + class_decl.name + "." + method.name, method, 1);
    }

    bool parameter_is_borrowed(const std::string& callee, std::size_t index) const {
        const auto found = borrowed_parameters.find(callee);
        return found != borrowed_parameters.end() && found->second.contains(index);
    }
    std::string label(std::string_view p) { return std::string(p)+"."+std::to_string(next_label++); }
    std::string hidden(std::string_view p) { return "$"+std::string(p)+"."+std::to_string(next_hidden++); }
    std::string bind_source_local(const std::string& source_name, const Type& type) {
        const auto ir_name = hidden("local." + source_name);
        local_names[source_name] = ir_name;
        locals[ir_name] = type;
        return ir_name;
    }
    const std::string& source_local(const std::string& source_name) const { return local_names.at(source_name); }
    const Type& source_local_type(const std::string& source_name) const { return locals.at(source_local(source_name)); }
    std::string bind_source_reference(const std::string& source_name, const Type& type) {
        const auto ir_name = hidden("ref." + source_name);
        reference_names[source_name] = ir_name;
        references[ir_name] = type;
        return ir_name;
    }
    bool is_source_reference(const std::string& source_name) const { return reference_names.contains(source_name); }
    const std::string& source_reference(const std::string& source_name) const { return reference_names.at(source_name); }
    ValueId fresh() { return next_value++; }
    Type type_of(const Expr& e) const { return checked.expr_types.at(&e); }
    bool terminated() const {
        if (!block || block->instructions.empty()) return false;
        const auto& i=block->instructions.back();
        if (std::holds_alternative<Return>(i)||std::holds_alternative<ReturnVoid>(i)||std::holds_alternative<Exit>(i)||std::holds_alternative<Jump>(i)||std::holds_alternative<Branch>(i)) return true;
        if (const auto* c=std::get_if<Call>(&i)) return c->result.kind==TypeKind::Never;
        return false;
    }
    Block& add_block(std::string name) { fn->blocks.push_back(Block{std::move(name),{}}); return fn->blocks.back(); }
    ValueId const_int(long long n) { auto v=fresh(); block->instructions.push_back(ConstantInt{v,std::to_string(n),Type::simple(TypeKind::Int)}); return v; }
    ValueId const_float(double x) { auto v=fresh(); block->instructions.push_back(ConstantFloat{v,x,Type::simple(TypeKind::Float)}); return v; }
    ValueId const_bool(bool b) { auto v=fresh(); block->instructions.push_back(ConstantBool{v,b}); return v; }

    ValueId extent_value(const Expr& expression) {
        auto value = expr(expression);
        const auto source = type_of(expression);
        const auto target = Type::simple(TypeKind::Int);
        if (source == target) return value;
        auto out = fresh();
        const bool checked_range =
            numeric_conversion_policy(source, target) ==
            NumericConversionPolicy::ExplicitRangeCheck;
        block->instructions.push_back(NumericConvert{
            out, value, source, target, checked_range,
            static_cast<std::uint32_t>(expression.span.start.line),
            static_cast<std::uint32_t>(expression.span.start.column)});
        return out;
    }

    std::vector<std::optional<std::string>> capture_extents(
        const std::vector<std::shared_ptr<Expr>>& expressions,
        std::string_view prefix) {
        std::vector<std::optional<std::string>> captured;
        captured.reserve(expressions.size());
        for (const auto& expression : expressions) {
            if (!expression) {
                captured.push_back(std::nullopt);
                continue;
            }
            const auto name = hidden(prefix);
            locals[name] = Type::simple(TypeKind::Int);
            block->instructions.push_back(
                DeclareLocal{name, Type::simple(TypeKind::Int)});
            const auto value = extent_value(*expression);
            block->instructions.push_back(
                StoreLocal{name, value, Type::simple(TypeKind::Int)});
            captured.push_back(name);
        }
        return captured;
    }

    std::vector<std::optional<ValueId>> load_captured_extents(
        const std::vector<std::optional<std::string>>& captured) {
        std::vector<std::optional<ValueId>> values;
        values.reserve(captured.size());
        for (const auto& name : captured) {
            if (!name) {
                values.push_back(std::nullopt);
                continue;
            }
            auto value = fresh();
            block->instructions.push_back(
                LoadLocal{value, *name, Type::simple(TypeKind::Int)});
            values.push_back(value);
        }
        return values;
    }

    void emit_shaped_constraint(
        ValueId value, TypeKind kind,
        const std::vector<std::optional<std::string>>& captured,
        SourceSpan span) {
        if (captured.empty()) return;
        block->instructions.push_back(ShapedConstraintCheck{
            value, kind, load_captured_extents(captured),
            static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column)});
    }

    bool deeper_array_constraint(
        const std::vector<std::optional<std::string>>& captured,
        std::size_t axis) const {
        for (std::size_t i = axis; i < captured.size(); ++i) {
            if (captured[i]) return true;
        }
        return false;
    }

    void emit_array_constraints(
        ValueId array, const Type& array_type,
        const std::vector<std::optional<std::string>>& captured,
        std::size_t axis, SourceSpan span) {
        if (axis >= captured.size() || array_type.kind != TypeKind::Array) return;

        if (captured[axis]) {
            auto actual = fresh();
            block->instructions.push_back(ArrayLength{actual, array});
            auto expected = fresh();
            block->instructions.push_back(
                LoadLocal{expected, *captured[axis], Type::simple(TypeKind::Int)});
            block->instructions.push_back(ExtentEqualCheck{
                actual, expected,
                static_cast<std::uint32_t>(span.start.line),
                static_cast<std::uint32_t>(span.start.column)});
        }

        if (axis + 1 >= captured.size() || !array_type.first ||
            array_type.first->kind != TypeKind::Array ||
            !deeper_array_constraint(captured, axis + 1)) {
            return;
        }

        auto count = fresh();
        block->instructions.push_back(ArrayLength{count, array});
        const auto index_name = hidden("shape.index");
        locals[index_name] = Type::simple(TypeKind::Int);
        block->instructions.push_back(
            DeclareLocal{index_name, Type::simple(TypeKind::Int)});
        block->instructions.push_back(
            StoreLocal{index_name, const_int(0), Type::simple(TypeKind::Int)});

        const auto cond = label("shape.cond");
        const auto body = label("shape.body");
        const auto done = label("shape.done");
        block->instructions.push_back(Jump{cond});

        block = &add_block(cond);
        auto index = fresh();
        block->instructions.push_back(
            LoadLocal{index, index_name, Type::simple(TypeKind::Int)});
        auto cmp = fresh();
        block->instructions.push_back(Binary{
            cmp, "<", index, count, Type::simple(TypeKind::Int),
            Type::simple(TypeKind::Bool),
            static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column)});
        block->instructions.push_back(Branch{cmp, body, done});

        block = &add_block(body);
        auto child = fresh();
        block->instructions.push_back(ArrayGet{
            child, array, index, *array_type.first,
            static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column),
            false, true});
        emit_array_constraints(
            child, *array_type.first, captured, axis + 1, span);
        auto next = fresh();
        block->instructions.push_back(Binary{
            next, "+", index, const_int(1), Type::simple(TypeKind::Int),
            Type::simple(TypeKind::Int),
            static_cast<std::uint32_t>(span.start.line),
            static_cast<std::uint32_t>(span.start.column)});
        block->instructions.push_back(
            StoreLocal{index_name, next, Type::simple(TypeKind::Int)});
        block->instructions.push_back(Jump{cond});

        block = &add_block(done);
    }

    ValueId generated_shape_array(
        const std::vector<std::optional<std::string>>& captured,
        SourceSpan span) {
        const auto shape_type = Type::array(Type::simple(TypeKind::Int));
        auto storage = fresh();
        auto length = const_int(static_cast<long long>(captured.size()));
        block->instructions.push_back(
            ArrayAlloc{storage, length, shape_type, false});
        for (std::size_t i = 0; i < captured.size(); ++i) {
            if (!captured[i]) {
                throw std::logic_error(
                    "contextual tensor allocation contains an unconstrained axis");
            }
            auto extent = fresh();
            block->instructions.push_back(
                LoadLocal{extent, *captured[i], Type::simple(TypeKind::Int)});
            block->instructions.push_back(ArraySet{
                storage, const_int(static_cast<long long>(i)), extent,
                Type::simple(TypeKind::Int),
                static_cast<std::uint32_t>(span.start.line),
                static_cast<std::uint32_t>(span.start.column),
                false, true});
        }
        return storage;
    }
    void collect_neural_parameters(
        ValueId object,const Type& type,const std::string& path,
        std::vector<NeuralParameterRef>& out,
        std::unordered_set<std::string>& active) {
        if(type.kind!=TypeKind::Class) return;
        if(type.class_name.rfind("__quidra_gc__std_neural_Parameter_",0)==0){
            if(path.empty()) throw std::logic_error("Parameter path cannot be empty");
            out.push_back(NeuralParameterRef{path,object});
            return;
        }
        if(active.contains(type.class_name)) return;
        const auto ci=checked.classes.find(type.class_name);
        if(ci==checked.classes.end()) return;
        active.insert(type.class_name);
        for(const auto& field:ci->second.fields){
            if(field.type.kind!=TypeKind::Class) continue;
            auto child=fresh();
            block->instructions.push_back(FieldGet{child,object,field.index,field.type});
            const auto child_path=path.empty()?field.name:path+"."+field.name;
            collect_neural_parameters(child,field.type,child_path,out,active);
        }
        active.erase(type.class_name);
    }

    static bool neural_state_leaf(const Type& type) {
        return is_numeric(type) || type.kind==TypeKind::Bool ||
               type.kind==TypeKind::String || type.kind==TypeKind::Bin ||
               type.kind==TypeKind::Tensor;
    }

    void collect_neural_state_values(
        ValueId object,const Type& type,const std::string& path,
        std::vector<NeuralStateValue>& out,std::unordered_set<std::string>& active) {
        if(neural_state_leaf(type)){
            out.push_back(NeuralStateValue{path,object,type});
            return;
        }
        if(type.kind!=TypeKind::Class) throw std::logic_error("unsupported neural state type");
        if(!active.insert(type.class_name).second)
            throw std::logic_error("recursive neural state type");
        const auto ci=checked.classes.find(type.class_name);
        if(ci==checked.classes.end()) throw std::logic_error("missing neural state class");
        for(const auto& field:ci->second.fields){
            auto child=fresh();
            block->instructions.push_back(FieldGet{child,object,field.index,field.type});
            const auto child_path=path.empty()?field.name:path+"."+field.name;
            collect_neural_state_values(child,field.type,child_path,out,active);
        }
        active.erase(type.class_name);
    }

    void collect_neural_state_targets(
        ValueId address,const Type& type,const std::string& path,
        std::vector<NeuralStateTarget>& out,std::unordered_set<std::string>& active) {
        if(neural_state_leaf(type)){
            out.push_back(NeuralStateTarget{path,address,type});
            return;
        }
        if(type.kind!=TypeKind::Class) throw std::logic_error("unsupported neural state type");
        if(!active.insert(type.class_name).second)
            throw std::logic_error("recursive neural state type");
        const auto ci=checked.classes.find(type.class_name);
        if(ci==checked.classes.end()) throw std::logic_error("missing neural state class");
        auto object=fresh();
        block->instructions.push_back(LoadAddress{object,address,type});
        for(const auto& field:ci->second.fields){
            auto child_address=fresh();
            block->instructions.push_back(AddressField{child_address,object,field.index});
            const auto child_path=path.empty()?field.name:path+"."+field.name;
            collect_neural_state_targets(child_address,field.type,child_path,out,active);
        }
        active.erase(type.class_name);
    }

    static std::string neural_state_schema_prefix(
        const std::vector<std::pair<std::string,Type>>& roots) {
        std::string schema="quidra.quistate.v1";
        for(const auto& [name,type]:roots)
            schema+="|root:"+name+":"+type_name(type);
        return schema;
    }

    static std::string neural_state_schema(
        const std::vector<std::pair<std::string,Type>>& roots,
        const std::vector<NeuralStateValue>& values) {
        auto schema=neural_state_schema_prefix(roots);
        for(const auto& value:values)
            schema+="|"+value.path+":"+type_name(value.type);
        return schema;
    }

    static std::string neural_state_schema(
        const std::vector<std::pair<std::string,Type>>& roots,
        const std::vector<NeuralStateTarget>& targets) {
        auto schema=neural_state_schema_prefix(roots);
        for(const auto& target:targets)
            schema+="|"+target.path+":"+type_name(target.type);
        return schema;
    }
    ValueId copy_value(ValueId v,const Type& t) {
        if (requires_value_clone(t)) {
            auto out=fresh();
            block->instructions.push_back(Clone{out,v,t});
            return out;
        }
        if (uses_shared_immutable_storage(t)) {
            auto out=fresh();
            block->instructions.push_back(Retain{out,v,t});
            return out;
        }
        return v;
    }

    bool expression_owns_result(const Expr& expression) const {
        const auto type = type_of(expression);
        if (!requires_lifetime_management(type)) return false;
        if (std::holds_alternative<StringExpr>(expression.data)) {
            return false;
        }
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            // Ordinary names borrow local/reference storage, but exact standard
            // real constants materialize a fresh managed bigreal value.
            return type.kind == TypeKind::BigReal &&
                   standard_float_constant(name->name).has_value();
        }
        if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
            return expression_owns_result(*member->base);
        }
        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            const auto base_kind = type_of(*index->base).kind;
            if (base_kind == TypeKind::Tensor || base_kind == TypeKind::String ||
                base_kind == TypeKind::Bin) return true;
            return expression_owns_result(*index->base);
        }
        if (const auto* tried = std::get_if<TryExpr>(&expression.data)) {
            return expression_owns_result(*tried->value);
        }
        if (const auto* method = std::get_if<MethodCallExpr>(&expression.data)) {
            const auto receiver_kind = type_of(*method->receiver).kind;
            if ((receiver_kind == TypeKind::String || receiver_kind == TypeKind::Error) &&
                method->method == "string") {
                return expression_owns_result(*method->receiver);
            }
            return true;
        }
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            const auto it = checked.call_resolutions.find(&expression);
            if (it != checked.call_resolutions.end() &&
                it->second.kind == CallKind::Constructor &&
                it->second.type.kind == TypeKind::Error &&
                !call->args.empty()) {
                return expression_owns_result(*call->args[0].value);
            }
            return true;
        }
        return true;
    }

    void release_temporary(const Expr& expression, ValueId value) {
        if (value != 0 && expression_owns_result(expression)) {
            block->instructions.push_back(Release{value,type_of(expression)});
        }
    }

    static bool array_shape_conversion(const Type& from, const Type& to) {
        return from.kind == TypeKind::Array && to.kind == TypeKind::Array && from != to;
    }

    ValueId destination_value(const Expr& expression, const Type& target) {
        auto value = raw_expr(expression);
        const auto source = checked.raw_types.at(&expression);
        const bool owned = expression_owns_result(expression);
        if (owned && source.kind == TypeKind::Union && source != target) {
            auto converted = convert(value, source, target, true);
            block->instructions.push_back(Release{value, source});
            return converted;
        }
        auto converted = convert(value, source, target, !owned);
        if (owned && array_shape_conversion(source, target)) {
            block->instructions.push_back(Release{value, source});
        }
        return converted;
    }

    ValueId receiver_value() {
        auto out=fresh();
        const auto type=Type::class_type(current_class);
        block->instructions.push_back(LoadLocal{out,"$receiver",type});
        return out;
    }

    ValueId convert_array_shape(ValueId source,const Type& from,const Type& to) {
        auto length=fresh();
        if(from.length>=0){
            block->instructions.push_back(
                ConstantInt{length,std::to_string(from.length),Type::simple(TypeKind::Int)});
        }else{
            block->instructions.push_back(ArrayLength{length,source});
        }

        auto output=fresh();
        block->instructions.push_back(ArrayAlloc{output,length,to});

        const auto index_name=hidden("array.convert.index");
        locals[index_name]=Type::simple(TypeKind::Int);
        const auto zero=const_int(0);
        block->instructions.push_back(StoreLocal{index_name,zero,locals[index_name]});

        const auto cond=label("array.convert.cond");
        const auto body=label("array.convert.body");
        const auto done=label("array.convert.done");
        block->instructions.push_back(Jump{cond});

        block=&add_block(cond);
        auto index=fresh(),more=fresh();
        block->instructions.push_back(LoadLocal{index,index_name,locals[index_name]});
        block->instructions.push_back(Binary{
            more,"<",index,length,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)});
        block->instructions.push_back(Branch{more,body,done});

        block=&add_block(body);
        auto item=fresh();
        block->instructions.push_back(ArrayGet{item,source,index,*from.first});
        auto converted=convert(item,*from.first,*to.first,true);
        block->instructions.push_back(ArraySet{output,index,converted,*to.first});

        const auto one=const_int(1);
        auto next=fresh();
        block->instructions.push_back(Binary{
            next,"+",index,one,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)});
        block->instructions.push_back(StoreLocal{index_name,next,locals[index_name]});
        block->instructions.push_back(Jump{cond});

        block=&add_block(done);
        return output;
    }

    ValueId convert(ValueId v,const Type& from,const Type& to,bool copy=false) {
        if(from.kind==TypeKind::Never)return v;
        if(array_shape_conversion(from,to)) return convert_array_shape(v,from,to);
        if(from.kind==TypeKind::Union && (from!=to||copy)) {
            auto tag=fresh();
            block->instructions.push_back(VariantTag{tag,v});
            const auto done=label("union.end");
            const bool has_value=to.kind!=TypeKind::Void && to.kind!=TypeKind::None;
            std::string result_name;
            if(has_value){
                result_name=hidden("union.convert.result");
                locals[result_name]=to;
            }
            for(std::size_t i=0;i<from.cases.size();++i){
                const auto yes=label("union.case");
                const auto next=label("union.next");
                auto n=const_int(i),cmp=fresh();
                block->instructions.push_back(Binary{
                    cmp,"==",tag,n,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)});
                block->instructions.push_back(Branch{cmp,yes,next});
                block=&add_block(yes);
                const auto& ct=from.cases[i];
                auto payload=fresh();
                block->instructions.push_back(VariantPayload{payload,v,ct});
                auto result=convert(payload,ct,to,copy);
                if(has_value) block->instructions.push_back(StoreLocal{result_name,result,to,true});
                block->instructions.push_back(Jump{done});
                block=&add_block(next);
            }
            block=&add_block(done);
            if(!has_value) return 0;
            auto out=fresh();
            block->instructions.push_back(LoadLocal{out,result_name,to});
            return out;
        }
        if(to.kind==TypeKind::Union && from.kind!=TypeKind::Union){if(copy)v=copy_value(v,from);auto out=fresh();block->instructions.push_back(VariantMake{out,compatible_case_index(to,from),v,to,from});return out;}
        if(from!=to && is_numeric(from) && is_numeric(to)){
            auto out=fresh();
            block->instructions.push_back(NumericConvert{out,v,from,to,false});
            v=out;
        }
        if(copy)return copy_value(v,to);
        return v;
    }

    ValueId expr(const Expr& e) {
        auto v=raw_expr(e);
        const auto from=checked.raw_types.at(&e);
        const auto to=type_of(e);
        auto converted=convert(v,from,to);
        if(expression_owns_result(e)&&array_shape_conversion(from,to))
            block->instructions.push_back(Release{v,from});
        return converted;
    }

    ValueId address_of(const Expr& e, bool may_write = true) {
        if (const auto* n=std::get_if<NameExpr>(&e.data)) {
            if (const auto it=checked.field_accesses.find(&e);it!=checked.field_accesses.end()) {
                auto object=receiver_value(),out=fresh();
                block->instructions.push_back(AddressField{out,object,it->second.index});
                return out;
            }
            auto out=fresh();
            if (is_source_reference(n->name)) {
                block->instructions.push_back(ReferenceAddress{out,source_reference(n->name)});
            } else {
                if (may_write && source_local_type(n->name).kind == TypeKind::Array) {
                    fully_initialized_array_locals.erase(n->name);
                }
                block->instructions.push_back(AddressLocal{out,source_local(n->name)});
            }
            return out;
        }
        if (const auto* n=std::get_if<MemberExpr>(&e.data)) {
            auto object=expr(*n->base),out=fresh();
            const auto& info=checked.field_accesses.at(&e);
            block->instructions.push_back(AddressField{out,object,info.index});
            return out;
        }
        if (const auto* n=std::get_if<IndexExpr>(&e.data)) {
            const auto array_type=type_of(*n->base);
            if(array_type.kind==TypeKind::Tensor){
                throw std::logic_error("tensor elements do not expose raw addresses");
            }
            auto array=expr(*n->base),index=expr(*n->items.front().index),out=fresh();
            const auto element_type=array_type.kind==TypeKind::Bin
                ? Type::simple(TypeKind::UInt8)
                : *array_type.first;
            block->instructions.push_back(AddressElement{
                out,array,index,array_type,element_type,array_type.kind==TypeKind::Bin,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return out;
        }
        throw std::runtime_error("invalid address target");
    }

    struct LoweredCallArguments {
        std::vector<CallArgument> args;
        std::vector<std::pair<ValueId, Type>> borrowed_temporaries;
    };

    LoweredCallArguments lower_call_arguments(const std::vector<CallArg>& source_args,
                                               const FunctionType& sig,
                                               std::size_t offset,
                                               const std::string& callee) {
        std::unordered_map<std::string,std::size_t> index;
        for(std::size_t i=offset;i<sig.parameters.size();++i) index[sig.parameters[i].name]=i;
        LoweredCallArguments lowered;
        lowered.args.resize(sig.parameters.size());
        std::vector<bool> filled(sig.parameters.size());
        for(std::size_t i=0;i<offset;++i) filled[i]=true;
        std::size_t positional=offset;
        const auto lower_value = [&](const Expr& expression, const FunctionParameterType& parameter, std::size_t target) {
            if (!parameter_is_borrowed(callee, target)) return destination_value(expression, parameter.type);
            const bool exact = checked.raw_types.at(&expression) == parameter.type && type_of(expression) == parameter.type;
            const auto value = exact ? expr(expression) : destination_value(expression, parameter.type);
            if (!exact || expression_owns_result(expression)) lowered.borrowed_temporaries.push_back({value, parameter.type});
            return value;
        };
        for(const auto& arg:source_args){
            std::size_t target;
            if(arg.name) target=index.at(*arg.name);
            else { while(positional<filled.size()&&filled[positional]) ++positional; target=positional++; }
            filled[target]=true; const auto& param=sig.parameters[target];
            if(param.writable) lowered.args[target]=CallArgument{0,address_of(*arg.value, !param.is_const)};
            else lowered.args[target]=CallArgument{lower_value(*arg.value,param,target),std::nullopt};
        }
        for(std::size_t i=offset;i<filled.size();++i) if(!filled[i]){
            const auto& parameter=sig.parameters[i];
            lowered.args[i]=CallArgument{lower_value(*parameter.default_value,parameter,i),std::nullopt};
        }
        return lowered;
    }

    void release_borrowed_temporaries(const LoweredCallArguments& lowered) {
        for (const auto& [value, type] : lowered.borrowed_temporaries)
            block->instructions.push_back(Release{value, type});
    }

    ValueId raw_expr(const Expr& e) {
        if (const auto* n=std::get_if<IntegerExpr>(&e.data)) {
            auto out=fresh();
            const auto type=checked.raw_types.at(&e);
            const auto spelling=n->spelling.empty()?std::to_string(n->value):n->spelling;
            if(type.kind==TypeKind::BigInt||type.kind==TypeKind::BigReal) {
                block->instructions.push_back(ConstantExact{out,spelling,type});
            } else if(is_float(type)) {
                block->instructions.push_back(ConstantFloat{
                    out,static_cast<double>(n->value),type});
            } else {
                block->instructions.push_back(ConstantInt{out,spelling,type});
            }
            return out;
        }
        if (const auto* n=std::get_if<FloatExpr>(&e.data)) {
            auto out=fresh(); const auto type=checked.raw_types.at(&e);
            if(type.kind==TypeKind::BigReal)
                block->instructions.push_back(ConstantExact{
                    out,n->spelling.empty()?std::to_string(n->value):n->spelling,type});
            else
                block->instructions.push_back(ConstantFloat{out,n->value,type});
            return out;
        }
        if (const auto* n=std::get_if<BoolExpr>(&e.data)) { auto out=fresh(); block->instructions.push_back(ConstantBool{out,n->value}); return out; }
        if (const auto* n=std::get_if<StringExpr>(&e.data)) { auto out=fresh(); block->instructions.push_back(ConstantString{out,n->value}); return out; }
        if (std::holds_alternative<VoidExpr>(e.data) || std::holds_alternative<NoneExpr>(e.data)) return 0;
        if (const auto* n=std::get_if<StringTemplateExpr>(&e.data)) {
            auto current=fresh();
            block->instructions.push_back(ConstantString{current,n->literals.front()});
            bool current_owned=false;
            for(std::size_t i=0;i<n->expressions.size();++i){
                auto part=expr(*n->expressions[i]);
                const auto pt=type_of(*n->expressions[i]);
                bool part_owned=expression_owns_result(*n->expressions[i]);
                if(n->formats[i].active()){
                    auto converted=fresh();
                    const auto& format=n->formats[i];
                    block->instructions.push_back(FormatNumber{converted,part,pt,format.integer_width,
                        format.fractional_digits,format.significant_digits,format.zero});
                    part=converted;
                    part_owned=true;
                }else if(pt.kind!=TypeKind::String && pt.kind!=TypeKind::Error){
                    auto converted=fresh();
                    block->instructions.push_back(ToString{converted,part,pt});
                    part=converted;
                    part_owned=true;
                }
                auto joined=fresh();
                block->instructions.push_back(Binary{
                    joined,"+",current,part,Type::simple(TypeKind::String),
                    Type::simple(TypeKind::String)});
                if(current_owned) block->instructions.push_back(
                    Release{current,Type::simple(TypeKind::String)});
                if(part_owned) block->instructions.push_back(
                    Release{part,Type::simple(TypeKind::String)});
                current=joined;
                current_owned=true;
                if(!n->literals[i+1].empty()){
                    auto lit=fresh();
                    block->instructions.push_back(ConstantString{lit,n->literals[i+1]});
                    joined=fresh();
                    block->instructions.push_back(Binary{
                        joined,"+",current,lit,Type::simple(TypeKind::String),
                        Type::simple(TypeKind::String)});
                    block->instructions.push_back(
                        Release{current,Type::simple(TypeKind::String)});
                    current=joined;
                }
            }
            return current;
        }
        if (const auto* n=std::get_if<NameExpr>(&e.data)) {
            if(is_builtin_text_constant(n->name)){auto out=fresh();block->instructions.push_back(ConstantString{out,std::string(builtin_text_constant(n->name))});return out;}
            if(const auto constant=standard_float_constant(n->name)){
                auto out=fresh(); const auto type=checked.raw_types.at(&e);
                if(type.kind==TypeKind::BigReal)
                    block->instructions.push_back(ConstantExact{
                        out,n->name=="$std.math.pi"?"$pi":"$e",type});
                else
                    block->instructions.push_back(ConstantFloat{out,*constant,type});
                return out;
            }
            if(const auto it=checked.field_accesses.find(&e);it!=checked.field_accesses.end()){
                auto object=receiver_value(),out=fresh();block->instructions.push_back(FieldGet{out,object,it->second.index,it->second.type});return out;
            }
            auto out=fresh(); const auto raw=checked.raw_types.at(&e);
            if(is_source_reference(n->name)){
                const auto& ir_name=source_reference(n->name); const auto t=references.at(ir_name);
                block->instructions.push_back(LoadReference{out,ir_name,t});
                if(t.kind==TypeKind::Union&&raw.kind!=TypeKind::Union){auto pv=fresh();block->instructions.push_back(VariantPayload{pv,out,raw});return pv;}
                return out;
            }
            const auto& ir_name=source_local(n->name); const auto t=locals.at(ir_name); block->instructions.push_back(LoadLocal{out,ir_name,t}); if(t.kind==TypeKind::Union&&raw.kind!=TypeKind::Union){auto pv=fresh();block->instructions.push_back(VariantPayload{pv,out,raw});return pv;}return out;
        }
        if (const auto* n=std::get_if<MemberExpr>(&e.data)) {
            const bool base_owned=expression_owns_result(*n->base);
            auto object=expr(*n->base),out=fresh();
            const auto& info=checked.field_accesses.at(&e);
            block->instructions.push_back(FieldGet{out,object,info.index,info.type});
            if(base_owned){
                if(requires_lifetime_management(info.type)) out=copy_value(out,info.type);
                block->instructions.push_back(Release{object,type_of(*n->base)});
            }
            return out;
        }
        if (const auto* n=std::get_if<ArrayExpr>(&e.data)) {
            std::vector<ValueId> values; values.reserve(n->elements.size());
            for(const auto& x:n->elements) values.push_back(destination_value(*x,type_of(*x)));
            auto out=fresh(); block->instructions.push_back(ArrayMake{out,std::move(values),checked.raw_types.at(&e)}); return out;
        }
        if (const auto* n=std::get_if<IndexExpr>(&e.data)) {
            const bool base_owned=expression_owns_result(*n->base);
            const auto base_type=type_of(*n->base);
            const bool initialization_proven =
                base_type.kind == TypeKind::Array &&
                array_expression_fully_initialized(*n->base);
            auto a=expr(*n->base), out=fresh();
            if(base_type.kind==TypeKind::Tensor){
                std::vector<TensorIndexPart> items;
                items.reserve(n->items.size());
                for(const auto& item:n->items){
                    TensorIndexPart lowered;
                    lowered.slice=item.slice;
                    if(item.index) lowered.index=expr(*item.index);
                    if(item.start) lowered.start=expr(*item.start);
                    if(item.stop) lowered.stop=expr(*item.stop);
                    if(item.step) lowered.step=expr(*item.step);
                    items.push_back(std::move(lowered));
                }
                block->instructions.push_back(TensorIndex{
                    out,a,std::move(items),base_type,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
            }else{
                if(base_type.kind==TypeKind::Bin) {
                    const auto& item=n->items.front();
                    if(item.slice){
                        auto start=item.start?expr(*item.start):const_int(0);
                        ValueId stop;
                        if(item.stop) stop=expr(*item.stop);
                        else { stop=fresh(); block->instructions.push_back(BinLength{stop,a}); }
                        block->instructions.push_back(BinSlice{out,a,start,stop});
                    }else{
                        auto i=expr(*item.index);
                        block->instructions.push_back(BinGet{
                            out,a,i,static_cast<std::uint32_t>(e.span.start.line),
                            static_cast<std::uint32_t>(e.span.start.column),
                            checked.bounds_proven.contains(item.index.get())});
                    }
                } else if(base_type.kind==TypeKind::String) {
                    auto i=expr(*n->items.front().index);
                    block->instructions.push_back(StringIndex{
                        out,a,i,static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                } else {
                    auto i=expr(*n->items.front().index);
                    block->instructions.push_back(ArrayGet{
                        out,a,i,checked.raw_types.at(&e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column),
                        initialization_proven,
                        checked.bounds_proven.contains(n->items.front().index.get())});
                    if(base_owned && requires_lifetime_management(checked.raw_types.at(&e)))
                        out=copy_value(out,checked.raw_types.at(&e));
                }
            }
            if(base_owned) block->instructions.push_back(Release{a,base_type});
            return out;
        }
        if (const auto* n=std::get_if<UnaryExpr>(&e.data)) {
            if(n->op=="-"){
                if(const auto* literal=std::get_if<IntegerExpr>(&n->operand->data);
                   literal && is_integer(type_of(e))){
                    auto out=fresh();
                    block->instructions.push_back(ConstantInt{
                        out,"-"+std::to_string(literal->value),type_of(e)});
                    return out;
                }
            }
            auto v=expr(*n->operand), out=fresh();
            block->instructions.push_back(Unary{
                out,n->op,v,type_of(e),static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return out;
        }
        if (const auto* n=std::get_if<BinaryExpr>(&e.data)) {
            if(n->op=="+" && type_of(e).kind==TypeKind::String){
                std::vector<const Expr*> parts;
                std::vector<const Expr*> pending_concat{&e};
                while(!pending_concat.empty()){
                    const auto* current=pending_concat.back();
                    pending_concat.pop_back();
                    const auto* binary=std::get_if<BinaryExpr>(&current->data);
                    if(binary && binary->op=="+" && type_of(*current).kind==TypeKind::String){
                        pending_concat.push_back(binary->right.get());
                        pending_concat.push_back(binary->left.get());
                    }else{
                        parts.push_back(current);
                    }
                }
                std::vector<ValueId> values;
                values.reserve(parts.size());
                for(const auto* part:parts) values.push_back(expr(*part));
                auto out=fresh();
                block->instructions.push_back(StringConcat{out,values});
                for(std::size_t i=0;i<parts.size();++i)
                    release_temporary(*parts[i],values[i]);
                return out;
            }
            if(n->op=="and"||n->op=="or"){
                const auto bool_type=Type::simple(TypeKind::Bool);
                const auto result_name=hidden(n->op=="and"?"and.result":"or.result");
                locals[result_name]=bool_type;
                auto left=expr(*n->left);
                const auto rhs=label(n->op=="and"?"and.rhs":"or.rhs");
                const auto merge=label(n->op=="and"?"and.end":"or.end");
                auto shortcut=const_bool(n->op=="or");
                block->instructions.push_back(StoreLocal{result_name,shortcut,bool_type,true});
                block->instructions.push_back(
                    n->op=="and"?Instruction{Branch{left,rhs,merge}}
                                :Instruction{Branch{left,merge,rhs}});
                auto& rb=add_block(rhs);
                block=&rb;
                auto right=expr(*n->right);
                block->instructions.push_back(StoreLocal{result_name,right,bool_type,true});
                block->instructions.push_back(Jump{merge});
                auto& mb=add_block(merge);
                block=&mb;
                auto out=fresh();
                block->instructions.push_back(LoadLocal{out,result_name,bool_type});
                return out;
            }

            const auto eager_binary = [](const Expr& expression) -> const BinaryExpr* {
                const auto* binary=std::get_if<BinaryExpr>(&expression.data);
                if(!binary || operator_policy::is_short_circuit(binary->op)) return nullptr;
                return binary;
            };

            // Operator trees are frequently left-deep. Lower every eager binary
            // node iteratively so scalar, tensor, and neural expressions do not
            // depend on the host compiler process stack size.
            std::vector<std::pair<const Expr*,bool>> pending;
            std::unordered_map<const Expr*,ValueId> values;
            pending.push_back({&e,false});
            while(!pending.empty()){
                const auto [current,visited]=pending.back();
                pending.pop_back();
                const auto* binary=eager_binary(*current);
                if(!binary){
                    values[current]=expr(*current);
                    continue;
                }
                if(!visited){
                    pending.push_back({current,true});
                    pending.push_back({binary->right.get(),false});
                    pending.push_back({binary->left.get(),false});
                    continue;
                }

                const auto left_value=values.at(binary->left.get());
                const auto right_value=values.at(binary->right.get());
                auto left=left_value;
                auto right=right_value;
                auto left_type=type_of(*binary->left);
                auto right_type=type_of(*binary->right);
                auto out=fresh();

                if(left_type.kind==TypeKind::Neural || right_type.kind==TypeKind::Neural){
                    block->instructions.push_back(NeuralBinary{
                        out,binary->op,left,right,left_type,right_type,type_of(*current),
                        static_cast<std::uint32_t>(current->span.start.line),
                        static_cast<std::uint32_t>(current->span.start.column)});
                }else if(left_type.kind==TypeKind::Tensor || right_type.kind==TypeKind::Tensor){
                    const auto tensor_type=
                        left_type.kind==TypeKind::Tensor?left_type:right_type;
                    const auto element=*tensor_type.first;
                    if(left_type.kind!=TypeKind::Tensor && left_type!=element){
                        left=convert(left,left_type,element);
                        left_type=element;
                    }
                    if(right_type.kind!=TypeKind::Tensor && right_type!=element){
                        right=convert(right,right_type,element);
                        right_type=element;
                    }
                    block->instructions.push_back(TensorBinary{
                        out,binary->op,left,right,left_type,right_type,type_of(*current),
                        static_cast<std::uint32_t>(current->span.start.line),
                        static_cast<std::uint32_t>(current->span.start.column)});
                }else{
                    block->instructions.push_back(Binary{
                        out,binary->op,left,right,left_type,type_of(*current),
                        static_cast<std::uint32_t>(current->span.start.line),
                        static_cast<std::uint32_t>(current->span.start.column)});
                }

                // Source operands are evaluated once. Release the original values;
                // tensor scalar conversions above are non-owning numeric values.
                release_temporary(*binary->left,left_value);
                release_temporary(*binary->right,right_value);

                ValueId result=out;
                if(current!=&e){
                    const auto from=checked.raw_types.at(current);
                    const auto to=type_of(*current);
                    result=convert(out,from,to);
                    if(expression_owns_result(*current)&&array_shape_conversion(from,to))
                        block->instructions.push_back(Release{out,from});
                }
                values[current]=result;
            }
            return values.at(&e);
        }
        if (const auto* n=std::get_if<TryExpr>(&e.data)) {
            const bool container_owned=expression_owns_result(*n->value);
            auto container=expr(*n->value),tag=fresh();
            block->instructions.push_back(VariantTag{tag,container});
            auto src=type_of(*n->value),result=checked.raw_types.at(&e);
            auto error_type=Type::simple(TypeKind::Error);
            auto errtag=const_int(case_index(src,error_type)),cmp=fresh();
            block->instructions.push_back(Binary{
                cmp,"==",tag,errtag,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)});
            auto bad=label("try.error"),ok=label("try.ok");
            block->instructions.push_back(Branch{cmp,bad,ok});
            block=&add_block(bad);
            auto payload=fresh();
            block->instructions.push_back(VariantPayload{payload,container,error_type});
            auto propagated=convert(payload,error_type,fn->result,container_owned);
            if(container_owned) block->instructions.push_back(Release{container,src});
            block->instructions.push_back(Return{propagated,fn->result});
            block=&add_block(ok);
            if(result.kind!=TypeKind::Union){
                auto out=fresh();
                block->instructions.push_back(VariantPayload{out,container,result});
                if(container_owned){
                    if(requires_lifetime_management(result)) out=copy_value(out,result);
                    block->instructions.push_back(Release{container,src});
                }
                return out;
            }
            const auto done=label("try.end");
            const auto result_name=hidden("try.result");
            locals[result_name]=result;
            for(auto& ct:result.cases){
                auto yes=label("try.case"),next=label("try.next");
                auto num=const_int(case_index(src,ct)),test=fresh();
                block->instructions.push_back(Binary{
                    test,"==",tag,num,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)});
                block->instructions.push_back(Branch{test,yes,next});
                block=&add_block(yes);
                auto pv=fresh();
                block->instructions.push_back(VariantPayload{pv,container,ct});
                auto cv=convert(pv,ct,result,container_owned);
                if(container_owned) block->instructions.push_back(Release{container,src});
                block->instructions.push_back(StoreLocal{result_name,cv,result,true});
                block->instructions.push_back(Jump{done});
                block=&add_block(next);
            }
            block=&add_block(done);
            auto out=fresh();
            block->instructions.push_back(LoadLocal{out,result_name,result});
            return out;
        }
        if (const auto* n=std::get_if<MethodCallExpr>(&e.data)) {
            const auto* receiver_name=std::get_if<NameExpr>(&n->receiver->data);
            if(receiver_name && n->method=="parse"){
                const auto target=builtin_scalar_type(receiver_name->name);
                if(target && is_numeric(*target)){
                    auto text=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(ParseNumber{out,text,*target,checked.raw_types.at(&e)});
                    release_temporary(*n->args[0].value,text);
                    return out;
                }
                if(target && target->kind==TypeKind::Bin){
                    auto text=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(ParseBin{out,text,checked.raw_types.at(&e)});
                    release_temporary(*n->args[0].value,text);
                    return out;
                }
            }
            if(receiver_name && receiver_name->name=="bin" && n->method=="fill"){
                auto length=expr(*n->args[0].value),fill=expr(*n->args[1].value),out=fresh();
                block->instructions.push_back(BinAlloc{out,length,fill});
                return out;
            }
            if(receiver_name && receiver_name->name=="string" && n->method=="repeat"){
                auto fill=expr(*n->args[0].value),count=expr(*n->args[1].value),out=fresh();
                block->instructions.push_back(StringRepeat{out,count,fill});
                release_temporary(*n->args[0].value,fill);
                return out;
            }
            const auto receiver_type=type_of(*n->receiver);
            if(receiver_type.kind==TypeKind::String){
                auto receiver=expr(*n->receiver);
                if(n->method=="string") return receiver;
                auto finish_string_receiver=[&](ValueId out){
                    release_temporary(*n->receiver,receiver);
                    return out;
                };
                if(n->method=="contains"){
                    auto a=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(StringContains{out,receiver,a});
                    release_temporary(*n->args[0].value,a);
                    return finish_string_receiver(out);
                }
                if(n->method=="starts_with"){
                    auto a=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(StringStartsWith{out,receiver,a});
                    release_temporary(*n->args[0].value,a);
                    return finish_string_receiver(out);
                }
                if(n->method=="ends_with"){
                    auto a=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(StringEndsWith{out,receiver,a});
                    release_temporary(*n->args[0].value,a);
                    return finish_string_receiver(out);
                }
                if(n->method=="find"){
                    auto a=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(StringFind{out,receiver,a,checked.raw_types.at(&e)});
                    release_temporary(*n->args[0].value,a);
                    return finish_string_receiver(out);
                }
                if(n->method=="slice"){
                    auto a=expr(*n->args[0].value),b=expr(*n->args[1].value),out=fresh();
                    block->instructions.push_back(StringSlice{out,receiver,a,b});
                    return finish_string_receiver(out);
                }
                if(n->method=="trim"){
                    auto out=fresh();
                    block->instructions.push_back(StringTrim{out,receiver});
                    return finish_string_receiver(out);
                }
                if(n->method=="split"){
                    auto a=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(StringSplit{out,receiver,a});
                    release_temporary(*n->args[0].value,a);
                    return finish_string_receiver(out);
                }
                if(n->method=="utf8"){
                    auto out=fresh();
                    block->instructions.push_back(StringUtf8{out,receiver});
                    return finish_string_receiver(out);
                }
                if(n->method=="codepoints"){
                    auto out=fresh();
                    block->instructions.push_back(StringCodepoints{out,receiver});
                    return finish_string_receiver(out);
                }
            }
            if(receiver_type.kind==TypeKind::Neural){
                auto receiver=expr(*n->receiver),out=fresh();
                if(n->method=="untrack"){
                    const auto result=type_of(e);
                    block->instructions.push_back(NeuralUntrack{out,receiver,result});
                    if(expression_owns_result(*n->receiver)) block->instructions.push_back(Release{receiver,receiver_type});
                    return out;
                }
            }
            if(receiver_type.kind==TypeKind::Tensor){
                auto receiver=expr(*n->receiver);
                const bool receiver_owned=expression_owns_result(*n->receiver);
                auto finish=[&](ValueId out){
                    if(receiver_owned) block->instructions.push_back(Release{receiver,receiver_type});
                    return out;
                };
                if(n->method=="gpu"){
                    auto gpu=expr(*n->args[0].value),out=fresh();
                    block->instructions.push_back(TensorTransfer{
                        out,receiver,gpu,receiver_type,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    return finish(out);
                }
                if(n->method=="cpu"){
                    auto out=fresh();
                    block->instructions.push_back(TensorTransfer{
                        out,receiver,std::nullopt,receiver_type,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    return finish(out);
                }
                if(n->method=="reshape"){
                    const auto shape_type=Type::array(Type::simple(TypeKind::Int));
                    auto shape=destination_value(*n->args[0].value,shape_type),out=fresh();
                    block->instructions.push_back(TensorReshape{
                        out,receiver,shape,receiver_type,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_temporary(*n->args[0].value,shape);
                    return finish(out);
                }
                if(n->method=="transpose"){
                    auto axis0=expr(*n->args[0].value);
                    auto axis1=expr(*n->args[1].value);
                    auto out=fresh();
                    block->instructions.push_back(TensorTranspose{
                        out,receiver,axis0,axis1,type_of(e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    return finish(out);
                }
                if(n->method=="contiguous"){
                    auto out=fresh();
                    block->instructions.push_back(TensorContiguous{out,receiver,receiver_type});
                    return finish(out);
                }
                if(n->method=="shape"){
                    auto out=fresh();
                    block->instructions.push_back(TensorShape{out,receiver,type_of(e)});
                    return finish(out);
                }
                if(n->method=="is_contiguous"){
                    auto out=fresh();
                    block->instructions.push_back(TensorIsContiguous{out,receiver});
                    return finish(out);
                }
                if(n->method=="item"){
                    auto out=fresh();
                    block->instructions.push_back(TensorItem{
                        out,receiver,*receiver_type.first,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    return finish(out);
                }
            }
            if(receiver_type.kind==TypeKind::Error && n->method=="string"){
                return expr(*n->receiver);
            }
            if(receiver_type.kind==TypeKind::Array && n->method=="sorted"){
                const bool receiver_owned=expression_owns_result(*n->receiver);
                auto source=expr(*n->receiver);
                auto source_type=receiver_type;
                bool source_owned=receiver_owned;
                if(receiver_type.length>=0){
                    const auto dynamic_type=Type::array(*receiver_type.first);
                    auto converted=convert(source,receiver_type,dynamic_type);
                    if(receiver_owned) block->instructions.push_back(Release{source,receiver_type});
                    source=converted;
                    source_type=dynamic_type;
                    source_owned=true;
                }
                auto out=fresh();
                block->instructions.push_back(ArraySorted{
                    out,source,source_type,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
                if(source_owned) block->instructions.push_back(Release{source,source_type});
                return out;
            }
            if(receiver_type.kind==TypeKind::Array && n->method=="join"){
                const bool receiver_owned=expression_owns_result(*n->receiver);
                auto source=expr(*n->receiver);
                auto source_type=receiver_type;
                bool source_owned=receiver_owned;
                if(receiver_type.length>=0){
                    const auto dynamic_type=Type::array(*receiver_type.first);
                    auto converted=convert(source,receiver_type,dynamic_type);
                    if(receiver_owned) block->instructions.push_back(Release{source,receiver_type});
                    source=converted;
                    source_type=dynamic_type;
                    source_owned=true;
                }
                auto separator=expr(*n->args[0].value);
                auto out=fresh();
                block->instructions.push_back(StringJoin{
                    out,source,separator,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
                release_temporary(*n->args[0].value,separator);
                if(source_owned) block->instructions.push_back(Release{source,source_type});
                return out;
            }
            if(receiver_type.kind==TypeKind::Array&&(n->method=="append"||n->method=="concat")){
                const bool source_owned=expression_owns_result(*n->receiver);
                auto source=expr(*n->receiver),source_length=fresh();
                block->instructions.push_back(ArrayLength{source_length,source});
                const auto element_type=*receiver_type.first;
                const auto result_type=Type::array(element_type);
                auto copy_into=[&](ValueId from,ValueId length,ValueId destination,ValueId offset,const std::string& prefix){
                    const auto index_name=hidden(prefix+".index");locals[index_name]=Type::simple(TypeKind::Int);
                    const auto zero=const_int(0);block->instructions.push_back(StoreLocal{index_name,zero,locals[index_name]});
                    const auto cond=label(prefix+".cond"),body=label(prefix+".body"),done=label(prefix+".done");
                    block->instructions.push_back(Jump{cond});block=&add_block(cond);
                    auto index=fresh(),more=fresh();block->instructions.push_back(LoadLocal{index,index_name,locals[index_name]});
                    block->instructions.push_back(Binary{more,"<",index,length,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)});
                    block->instructions.push_back(Branch{more,body,done});block=&add_block(body);
                    auto item=fresh();block->instructions.push_back(ArrayGet{item,from,index,element_type});item=copy_value(item,element_type);
                    auto destination_index=fresh();block->instructions.push_back(Binary{destination_index,"+",offset,index,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)});
                    block->instructions.push_back(ArraySet{destination,destination_index,item,element_type});
                    const auto one=const_int(1);auto next=fresh();block->instructions.push_back(Binary{next,"+",index,one,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)});
                    block->instructions.push_back(StoreLocal{index_name,next,locals[index_name]});block->instructions.push_back(Jump{cond});block=&add_block(done);
                };
                if(n->method=="append"){
                    const auto one=const_int(1);auto output_length=fresh();block->instructions.push_back(Binary{output_length,"+",source_length,one,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)});
                    auto output=fresh();block->instructions.push_back(ArrayAlloc{output,output_length,result_type});
                    const auto zero=const_int(0);copy_into(source,source_length,output,zero,"append.copy");
                    auto appended=destination_value(*n->args[0].value,element_type);
                    block->instructions.push_back(ArraySet{output,source_length,appended,element_type});
                    if(source_owned) block->instructions.push_back(Release{source,receiver_type});
                    return output;
                }
                const bool other_owned=expression_owns_result(*n->args[0].value);
                auto other=expr(*n->args[0].value),other_length=fresh();block->instructions.push_back(ArrayLength{other_length,other});
                auto output_length=fresh();block->instructions.push_back(Binary{output_length,"+",source_length,other_length,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)});
                auto output=fresh();block->instructions.push_back(ArrayAlloc{output,output_length,result_type});
                const auto zero=const_int(0);copy_into(source,source_length,output,zero,"concat.left");copy_into(other,other_length,output,source_length,"concat.right");
                if(source_owned) block->instructions.push_back(Release{source,receiver_type});
                if(other_owned) block->instructions.push_back(Release{other,type_of(*n->args[0].value)});
                return output;
            }
            if(n->method=="string" && !checked.method_calls.contains(&e)){
                auto value=expr(*n->receiver),out=fresh();
                block->instructions.push_back(ToString{out,value,type_of(*n->receiver)});
                release_temporary(*n->receiver,value);
                return out;
            }
            const auto internal=checked.method_calls.at(&e).internal_name;
            const auto& sig=checked.functions.at(internal);
            auto lowered=lower_call_arguments(n->args,sig,1,internal);
            const bool super_receiver=receiver_name&&receiver_name->name=="super";
            auto receiver=super_receiver?receiver_value():expr(*n->receiver);
            lowered.args[0]=CallArgument{receiver,std::nullopt};
            const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:fresh();
            block->instructions.push_back(Call{out,internal,std::move(lowered.args),sig.result,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
            if(sig.result.kind!=TypeKind::Never) release_borrowed_temporaries(lowered);
            if(sig.result.kind!=TypeKind::Never&&!super_receiver) release_temporary(*n->receiver,receiver);
            return out;
        }

        const auto& n=std::get<CallExpr>(e.data);
        const auto resolution_it=checked.call_resolutions.find(&e);
        if(resolution_it==checked.call_resolutions.end()) {
            throw std::logic_error("Checked CallExpr has no call resolution.");
        }
        const auto& resolution=resolution_it->second;

        if(const auto method=checked.method_calls.find(&e);method!=checked.method_calls.end()){
            const auto& sig=checked.functions.at(resolution.target);
            auto lowered=lower_call_arguments(n.args,sig,1,resolution.target);
            lowered.args[0]=CallArgument{receiver_value(),std::nullopt};
            const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:fresh();
            block->instructions.push_back(Call{out,resolution.target,std::move(lowered.args),sig.result,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
            if(sig.result.kind!=TypeKind::Never) release_borrowed_temporaries(lowered);
            return out;
        }

        if(resolution.kind==CallKind::NumericCast){
            auto value=expr(*n.args[0].value),out=fresh();
            const auto source=type_of(*n.args[0].value);
            const auto& target=resolution.type;
            if(source.kind==TypeKind::Bin){
                block->instructions.push_back(BinConvert{
                    out,value,source,target,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
                release_temporary(*n.args[0].value,value);
            }else if(source.kind==TypeKind::Tensor){
                block->instructions.push_back(TensorCast{
                    out,value,source,target,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
            }else if(source.kind==TypeKind::Neural){
                block->instructions.push_back(NeuralNumericCast{
                    out,value,source,target,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
            }else if(source.kind==TypeKind::Array){
                block->instructions.push_back(ArrayNumericCast{
                    out,value,source,target,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
            }else{
                const bool checked_range=
                    numeric_conversion_policy(source,target)==NumericConversionPolicy::ExplicitRangeCheck;
                block->instructions.push_back(NumericConvert{
                    out,value,source,target,checked_range,
                    static_cast<std::uint32_t>(e.span.start.line),
                    static_cast<std::uint32_t>(e.span.start.column)});
                release_temporary(*n.args[0].value,value);
            }
            return out;
        }

        if(resolution.kind==CallKind::Constructor){
            if(resolution.type.kind==TypeKind::Error) {
                return expr(*n.args[0].value);
            }
            if(resolution.target=="string.repeat"){
                auto count=expr(*n.args[0].value),fill=expr(*n.args[1].value),out=fresh();
                block->instructions.push_back(StringRepeat{out,count,fill});
                release_temporary(*n.args[1].value,fill);
                return out;
            }
            if(resolution.type.kind==TypeKind::Bin){
                if(resolution.target=="bin.cast"){
                    auto value=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(BinConvert{
                        out,value,type_of(*n.args[0].value),Type::simple(TypeKind::Bin),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_temporary(*n.args[0].value,value);
                    return out;
                }
                auto length=expr(*n.args[0].value);
                auto fill=expr(*n.args[1].value);
                auto out=fresh();
                block->instructions.push_back(BinAlloc{out,length,fill});
                return out;
            }
            if(resolution.type.kind==TypeKind::Class){
                const auto& info=checked.classes.at(resolution.target);
                std::vector<std::optional<ValueId>> fields(info.fields.size());
                std::unordered_set<std::string> supplied;

                for(const auto& arg:n.args){
                    const auto it=std::find_if(info.fields.begin(),info.fields.end(),
                        [&](const auto& field){return field.name==*arg.name;});
                    auto v=destination_value(*arg.value,it->type);
                    fields[it->index]=v;
                    supplied.insert(it->name);
                }

                for(const auto& field:info.fields){
                    if(supplied.contains(field.name)||!field.default_value)continue;
                    auto v=destination_value(*field.default_value,field.type);
                    fields[field.index]=v;
                }

                auto out=fresh();
                block->instructions.push_back(
                    ClassMake{out,Type::class_type(resolution.target),std::move(fields)});
                return out;
            }
            throw std::logic_error("Unsupported constructor resolution.");
        }

        if(resolution.kind==CallKind::Builtin){
            if(!resolution.builtin) throw std::logic_error("Builtin resolution has no builtin kind.");
            auto release_arg = [&](std::size_t index, ValueId value) {
                release_temporary(*n.args[index].value, value);
            };
            switch(*resolution.builtin){
                case BuiltinCallable::Print: {
                    auto v=expr(*n.args[0].value);
                    block->instructions.push_back(Print{v,type_of(*n.args[0].value)});
                    release_arg(0,v);
                    return 0;
                }
                case BuiltinCallable::Write: {
                    auto v=expr(*n.args[0].value);
                    block->instructions.push_back(Write{v,type_of(*n.args[0].value)});
                    release_arg(0,v);
                    return 0;
                }
                case BuiltinCallable::Input: {
                    auto out=fresh();
                    block->instructions.push_back(Input{out,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::Exit: {
                    auto v=expr(*n.args[0].value);
                    block->instructions.push_back(Exit{v});
                    return 0;
                }
                case BuiltinCallable::NeuralTrack:
                case BuiltinCallable::NeuralParameterTrack: {
                    auto input=expr(*n.args[0].value),out=fresh();
                    const bool parameter =
                        *resolution.builtin == BuiltinCallable::NeuralParameterTrack;
                    block->instructions.push_back(NeuralTrack{
                        out,input,checked.raw_types.at(&e),parameter,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    return out;
                }
                case BuiltinCallable::NeuralGrad: {
                    auto loss=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(NeuralGrad{out,loss,type_of(*n.args[0].value),
                        static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,loss);
                    return out;
                }
                case BuiltinCallable::NeuralAbsolute:
                case BuiltinCallable::NeuralExponential:
                case BuiltinCallable::NeuralLogarithm:
                case BuiltinCallable::NeuralMean:
                case BuiltinCallable::NeuralSumLast:
                case BuiltinCallable::NeuralMaxLast: {
                    auto input=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(NeuralUnary{
                        out,input,checked.raw_types.at(&e),*resolution.builtin,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    return out;
                }
                case BuiltinCallable::NeuralUpdate: {
                    auto model=expr(*n.args[0].value);
                    auto gradients=expr(*n.args[1].value);
                    auto rate=expr(*n.args[2].value);
                    std::vector<NeuralParameterRef> parameters;
                    std::unordered_set<std::string> active;
                    collect_neural_parameters(
                        model,type_of(*n.args[0].value),"",parameters,active);
                    block->instructions.push_back(NeuralUpdate{
                        std::move(parameters),gradients,rate,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(1,gradients);
                    return 0;
                }
                case BuiltinCallable::NeuralNormalize:
                case BuiltinCallable::NeuralNormalizeInference: {
                    const bool training=
                        *resolution.builtin==BuiltinCallable::NeuralNormalize;
                    auto input=expr(*n.args[0].value);
                    auto scale=expr(*n.args[1].value);
                    auto bias=expr(*n.args[2].value);
                    auto running_mean=expr(*n.args[3].value);
                    auto running_variance=expr(*n.args[4].value);
                    auto momentum=training?expr(*n.args[5].value):const_float(0.0);
                    auto epsilon=expr(*n.args[training?6:5].value);
                    auto out=fresh();
                    block->instructions.push_back(NeuralNormalize{
                        out,input,scale,bias,running_mean,running_variance,
                        momentum,epsilon,checked.raw_types.at(&e),training,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    for(std::size_t i=0;i<5;++i) release_arg(i,
                        i==0?input:i==1?scale:i==2?bias:i==3?running_mean:running_variance);
                    return out;
                }
                case BuiltinCallable::NeuralRandomMask: {
                    auto input=expr(*n.args[0].value);
                    auto state=expr(*n.args[1].value);
                    auto rate=expr(*n.args[2].value);
                    auto out=fresh();
                    block->instructions.push_back(NeuralRandomMask{
                        out,input,state,rate,checked.raw_types.at(&e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    release_arg(1,state);
                    return out;
                }
                case BuiltinCallable::NeuralMomentUpdate: {
                    auto model=expr(*n.args[0].value);
                    auto rate=expr(*n.args[1].value);
                    auto beta1=expr(*n.args[2].value);
                    auto beta2=expr(*n.args[3].value);
                    auto epsilon=expr(*n.args[4].value);
                    auto step=expr(*n.args[5].value);
                    auto moments=expr(*n.args[6].value);
                    auto gradients=expr(*n.args[7].value);
                    std::vector<NeuralParameterRef> parameters;
                    std::unordered_set<std::string> active;
                    collect_neural_parameters(
                        model,type_of(*n.args[0].value),"",parameters,active);
                    block->instructions.push_back(NeuralMomentUpdate{
                        std::move(parameters),rate,beta1,beta2,epsilon,step,moments,gradients,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(7,gradients);
                    return 0;
                }
                case BuiltinCallable::NeuralSave: {
                    std::vector<NeuralStateValue> values;
                    std::vector<std::pair<const Expr*,ValueId>> roots;
                    std::vector<std::pair<std::string,Type>> root_types;
                    std::unordered_set<std::string> active;
                    const auto object_count=n.args.size()-1;
                    for(std::size_t i=0;i<object_count;++i){
                        auto object=expr(*n.args[i].value);
                        roots.push_back({n.args[i].value.get(),object});
                        const std::string root=i==0?"model":"optimizer";
                        const auto root_type=type_of(*n.args[i].value);
                        root_types.push_back({root,root_type});
                        collect_neural_state_values(
                            object,root_type,root,values,active);
                    }
                    auto path=expr(*n.args.back().value);
                    block->instructions.push_back(NeuralSave{
                        path,neural_state_schema(root_types,values),std::move(values),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(n.args.size()-1,path);
                    for(std::size_t i=0;i<roots.size();++i)
                        release_arg(i,roots[i].second);
                    return 0;
                }
                case BuiltinCallable::NeuralLoad: {
                    std::vector<NeuralStateTarget> targets;
                    std::vector<std::pair<std::string,Type>> root_types;
                    std::unordered_set<std::string> active;
                    const auto object_count=n.args.size()-1;
                    for(std::size_t i=0;i<object_count;++i){
                        auto address=address_of(*n.args[i].value);
                        const std::string root=i==0?"model":"optimizer";
                        const auto root_type=type_of(*n.args[i].value);
                        root_types.push_back({root,root_type});
                        collect_neural_state_targets(
                            address,root_type,root,targets,active);
                    }
                    auto path=expr(*n.args.back().value);
                    block->instructions.push_back(NeuralLoad{
                        path,neural_state_schema(root_types,targets),std::move(targets),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(n.args.size()-1,path);
                    return 0;
                }
                case BuiltinCallable::NeuralConvolve2D: {
                    auto input=expr(*n.args[0].value);
                    auto weight=expr(*n.args[1].value);
                    auto bias=expr(*n.args[2].value);
                    auto stride=expr(*n.args[3].value);
                    auto padding=expr(*n.args[4].value);
                    auto out=fresh();
                    block->instructions.push_back(NeuralConvolve2D{
                        out,input,weight,bias,stride,padding,type_of(*n.args[0].value),
                        checked.raw_types.at(&e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input); release_arg(1,weight); release_arg(2,bias);
                    return out;
                }
                case BuiltinCallable::NeuralAffine: {
                    auto input=expr(*n.args[0].value);
                    auto weight=expr(*n.args[1].value);
                    auto bias=expr(*n.args[2].value);
                    auto out=fresh();
                    block->instructions.push_back(NeuralAffine{
                        out,input,weight,bias,type_of(*n.args[0].value),
                        checked.raw_types.at(&e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    release_arg(1,weight);
                    release_arg(2,bias);
                    return out;
                }
                case BuiltinCallable::StatsMean: {
                    auto input=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(StatsMean{
                        out,input,type_of(*n.args[0].value),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    return out;
                }
                case BuiltinCallable::StatsSum:
                case BuiltinCallable::StatsMin:
                case BuiltinCallable::StatsMax: {
                    auto input=expr(*n.args[0].value),out=fresh();
                    const auto tensor_type=type_of(*n.args[0].value);
                    block->instructions.push_back(StatsReduce{
                        out,input,*tensor_type.first,*resolution.builtin,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    return out;
                }
                case BuiltinCallable::LinearMatmul: {
                    auto left=expr(*n.args[0].value);
                    auto right=expr(*n.args[1].value);
                    auto out=fresh();
                    block->instructions.push_back(LinearMatmul{
                        out,left,right,checked.raw_types.at(&e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,left);
                    release_arg(1,right);
                    return out;
                }
                case BuiltinCallable::LinearDot: {
                    auto left=expr(*n.args[0].value);
                    auto right=expr(*n.args[1].value);
                    auto out=fresh();
                    block->instructions.push_back(LinearDot{
                        out,left,right,checked.raw_types.at(&e),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,left);
                    release_arg(1,right);
                    return out;
                }
                case BuiltinCallable::TensorCreate:
                case BuiltinCallable::TensorZeros:
                case BuiltinCallable::TensorOnes: {
                    const auto shape_type=Type::array(Type::simple(TypeKind::Int));
                    ValueId shape{};
                    std::optional<ValueId> gpu;
                    std::optional<std::size_t> shape_argument;
                    bool generated=false;
                    for(std::size_t i=0;i<n.args.size();++i){
                        const auto& argument=n.args[i];
                        if(argument.name && *argument.name=="gpu"){
                            gpu=expr(*argument.value);
                        }else{
                            shape=destination_value(*argument.value,shape_type);
                            shape_argument=i;
                        }
                    }
                    if(!shape_argument){
                        const auto found=contextual_tensor_shapes.find(&e);
                        if(found==contextual_tensor_shapes.end())
                            throw std::logic_error("missing contextual tensor shape capture");
                        shape=generated_shape_array(found->second,e.span);
                        generated=true;
                    }
                    auto out=fresh();
                    const auto type=checked.raw_types.at(&e);
                    const int fill_mode=
                        *resolution.builtin==BuiltinCallable::TensorZeros ? 1 :
                        *resolution.builtin==BuiltinCallable::TensorOnes ? 2 : 0;
                    block->instructions.push_back(TensorCreate{
                        out,shape,gpu,type,fill_mode,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    if(generated) block->instructions.push_back(Release{shape,shape_type});
                    else release_arg(*shape_argument,shape);
                    return out;
                }
                case BuiltinCallable::Array: {
                    auto count=expr(*n.args[0].value);
                    const auto t=checked.raw_types.at(&e);
                    const bool has_fill=n.args.size()!=1;
                    auto out=fresh();
                    block->instructions.push_back(ArrayAlloc{out,count,t,has_fill});
                    if(!has_fill) return out;

                    const auto& fill_expression=*n.args[1].value;
                    bool zero_fill=false;
                    if(const auto* value=std::get_if<IntegerExpr>(&fill_expression.data))
                        zero_fill=value->value==0 && is_integer(*t.first);
                    else if(const auto* value=std::get_if<FloatExpr>(&fill_expression.data))
                        zero_fill=value->value==0.0 && !std::signbit(value->value) && is_float(*t.first);
                    else if(const auto* value=std::get_if<BoolExpr>(&fill_expression.data))
                        zero_fill=!value->value && t.first->kind==TypeKind::Bool;
                    if(zero_fill) return out;

                    auto fill=expr(fill_expression);
                    auto idxname=hidden("fill.index");
                    locals[idxname]=Type::simple(TypeKind::Int);
                    auto zero=const_int(0);
                    block->instructions.push_back(StoreLocal{idxname,zero,locals[idxname]});
                    auto cond=label("fill.cond"),body=label("fill.body"),done=label("fill.end");
                    block->instructions.push_back(Jump{cond});
                    block=&add_block(cond);
                    auto idx=fresh(),cmp=fresh();
                    block->instructions.push_back(LoadLocal{idx,idxname,locals[idxname]});
                    block->instructions.push_back(
                        Binary{cmp,"<",idx,count,locals[idxname],Type::simple(TypeKind::Bool)});
                    block->instructions.push_back(Branch{cmp,body,done});
                    block=&add_block(body);
                    auto value=convert(fill,*t.first,*t.first,true);
                    block->instructions.push_back(ArraySet{
                        out,idx,value,*t.first,0,0,true,true});
                    auto one=const_int(1),next=fresh();
                    block->instructions.push_back(
                        Binary{next,"+",idx,one,locals[idxname],locals[idxname]});
                    block->instructions.push_back(StoreLocal{idxname,next,locals[idxname]});
                    block->instructions.push_back(Jump{cond});
                    block=&add_block(done);
                    release_arg(1,fill);
                    return out;
                }
                case BuiltinCallable::Range:
                    throw std::logic_error("range is lowered only by for statements");
                case BuiltinCallable::Len: {
                    auto value=expr(*n.args[0].value),out=fresh();
                    const auto kind=type_of(*n.args[0].value).kind;
                    if(kind==TypeKind::Bin)block->instructions.push_back(BinLength{out,value});
                    else if(kind==TypeKind::String)block->instructions.push_back(StringLength{out,value});
                    else block->instructions.push_back(ArrayLength{out,value});
                    release_arg(0,value);
                    return out;
                }
                case BuiltinCallable::Abs: {
                    auto value=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(
                        NumericAbs{out,value,type_of(*n.args[0].value),static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,value);
                    return out;
                }
                case BuiltinCallable::Sqrt: {
                    auto value=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(Sqrt{
                        out,value,type_of(*n.args[0].value),
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,value);
                    return out;
                }
                case BuiltinCallable::Min:
                case BuiltinCallable::Max: {
                    auto left=expr(*n.args[0].value),right=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(NumericMinMax{
                        out,left,right,type_of(*n.args[0].value),
                        *resolution.builtin==BuiltinCallable::Max});
                    release_arg(0,left);
                    release_arg(1,right);
                    return out;
                }
                case BuiltinCallable::MathSin:
                case BuiltinCallable::MathCos:
                case BuiltinCallable::MathTan:
                case BuiltinCallable::MathLog:
                case BuiltinCallable::MathExp: {
                    auto value=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(MathUnary{
                        out,value,type_of(*n.args[0].value),*resolution.builtin});
                    release_arg(0,value);
                    return out;
                }
                case BuiltinCallable::MathTrunc:
                case BuiltinCallable::MathRound:
                case BuiltinCallable::MathFloor:
                case BuiltinCallable::MathCeil: {
                    auto input=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(MathRoundInt{
                        out,input,type_of(*n.args[0].value),checked.raw_types.at(&e),
                        *resolution.builtin,static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,input);
                    return out;
                }
                case BuiltinCallable::MathPow: {
                    auto base=expr(*n.args[0].value),exponent=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(MathPow{
                        out,base,exponent,type_of(*n.args[0].value)});
                    release_arg(0,base);
                    release_arg(1,exponent);
                    return out;
                }
                case BuiltinCallable::CliArgument: {
                    auto name=expr(*n.args[0].value),index=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(CliArgument{out,name,index,checked.raw_types.at(&e)});
                    release_arg(0,name);
                    return out;
                }
                case BuiltinCallable::CliOption: {
                    auto name=expr(*n.args[0].value),fallback=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(CliOption{out,name,fallback,checked.raw_types.at(&e)});
                    release_arg(0,name);
                    release_arg(1,fallback);
                    return out;
                }
                case BuiltinCallable::CliFlag: {
                    auto name=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(CliFlag{out,name});
                    release_arg(0,name);
                    return out;
                }
                case BuiltinCallable::CliFinish:
                    block->instructions.push_back(CliFinish{});
                    return 0;
                case BuiltinCallable::FileRead: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(FileRead{out,path,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::FileReadBin: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(FileReadBin{out,path,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::FileList: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    ValueId recursive=n.args.size()==2?expr(*n.args[1].value):const_bool(false);
                    block->instructions.push_back(FileList{out,path,recursive,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::FileWrite: {
                    auto path=expr(*n.args[0].value),text=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(FileWrite{out,path,text,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    release_arg(1,text);
                    return out;
                }
                case BuiltinCallable::FileWriteBin: {
                    auto path=expr(*n.args[0].value),bin=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(FileWriteBin{out,path,bin,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    release_arg(1,bin);
                    return out;
                }
                case BuiltinCallable::FileExists: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(FileExists{out,path,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::FileIsDirectory: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(FileIsDirectory{out,path,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::FileRemove: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(FileRemove{out,path,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::FileCopy: {
                    auto source=expr(*n.args[0].value),destination=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(FileCopy{out,source,destination,checked.raw_types.at(&e)});
                    release_arg(0,source);
                    release_arg(1,destination);
                    return out;
                }
                case BuiltinCallable::FileMove: {
                    auto source=expr(*n.args[0].value),destination=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(FileMove{out,source,destination,checked.raw_types.at(&e)});
                    release_arg(0,source);
                    release_arg(1,destination);
                    return out;
                }
                case BuiltinCallable::FileMkdir: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(FileMkdir{out,path,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::EnvironmentGet: {
                    auto name=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(EnvironmentGet{out,name,checked.raw_types.at(&e)});
                    release_arg(0,name);
                    return out;
                }
                case BuiltinCallable::EnvironmentHas: {
                    auto name=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(EnvironmentHas{out,name});
                    release_arg(0,name);
                    return out;
                }
                case BuiltinCallable::TestCheck: {
                    auto condition=expr(*n.args[0].value);
                    block->instructions.push_back(TestAssert{condition});
                    return 0;
                }
                case BuiltinCallable::TestEqual: {
                    auto actual=expr(*n.args[0].value),expected_value=expr(*n.args[1].value);
                    auto equal=fresh();
                    block->instructions.push_back(Binary{
                        equal,"==",actual,expected_value,type_of(*n.args[0].value),Type::simple(TypeKind::Bool)});
                    block->instructions.push_back(TestAssert{equal});
                    release_arg(0,actual);
                    release_arg(1,expected_value);
                    return 0;
                }
                case BuiltinCallable::TimeNow: {
                    auto out=fresh();
                    block->instructions.push_back(TimeNow{out});
                    return out;
                }
                case BuiltinCallable::TimeSince: {
                    auto start=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(TimeSince{out,start});
                    release_arg(0,start);
                    return out;
                }
                case BuiltinCallable::TimeSeconds: {
                    auto seconds=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(TimeSeconds{out,seconds});
                    return out;
                }
                case BuiltinCallable::TimeSleep: {
                    auto duration=expr(*n.args[0].value);
                    block->instructions.push_back(TimeSleep{duration,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
                    release_arg(0,duration);
                    return 0;
                }
                case BuiltinCallable::RandomGenerator: {
                    auto seed=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(RandomGenerator{out,seed});
                    return out;
                }
                case BuiltinCallable::RandomInt: {
                    auto generator=receiver_value();
                    auto start=expr(*n.args[0].value),end=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(RandomInt{out,generator,start,end,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
                    return out;
                }
                case BuiltinCallable::RandomFloat: {
                    auto generator=receiver_value(),out=fresh();
                    block->instructions.push_back(RandomFloat{out,generator});
                    return out;
                }
                case BuiltinCallable::RandomBool: {
                    auto generator=receiver_value(),out=fresh();
                    block->instructions.push_back(RandomBool{out,generator});
                    return out;
                }
                case BuiltinCallable::ProcessRun: {
                    auto program=expr(*n.args[0].value),args=expr(*n.args[1].value),out=fresh();
                    block->instructions.push_back(ProcessRun{out,program,args});
                    release_arg(0,program);
                    release_arg(1,args);
                    return out;
                }
                case BuiltinCallable::JsonParse: {
                    auto text=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(JsonParse{out,text,checked.raw_types.at(&e)});
                    release_arg(0,text);
                    return out;
                }
                case BuiltinCallable::JsonKind: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonKind{out,value});
                    return out;
                }
                case BuiltinCallable::JsonSize: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonSize{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonGet: {
                    auto value=receiver_value(),key=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(JsonGet{out,value,key,checked.raw_types.at(&e)});
                    release_arg(0,key);
                    return out;
                }
                case BuiltinCallable::JsonAt: {
                    auto value=receiver_value(),index=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(JsonAt{out,value,index,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonText: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonText{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonInteger: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonInteger{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonNumber: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonNumber{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonBigInt: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonBigInt{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonBigReal: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonBigReal{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonBoolean: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonBoolean{out,value,checked.raw_types.at(&e)});
                    return out;
                }
                case BuiltinCallable::JsonEncode: {
                    auto value=receiver_value(),out=fresh();
                    block->instructions.push_back(JsonEncode{out,value});
                    return out;
                }
                case BuiltinCallable::JsonEqual: {
                    auto left=receiver_value(),right=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(JsonEqual{out,left,right});
                    release_arg(0,right);
                    return out;
                }
                case BuiltinCallable::ImageRead: {
                    auto path=expr(*n.args[0].value),out=fresh();
                    std::optional<Type> target_dtype;
                    int target_channels=0;
                    for(std::size_t i=1;i<n.args.size();++i){
                        if(!n.args[i].name) continue;
                        if(*n.args[i].name=="channels"){
                            if(const auto* value=std::get_if<IntegerExpr>(&n.args[i].value->data))
                                target_channels=static_cast<int>(value->value);
                        }else if(*n.args[i].name=="dtype"){
                            if(const auto* name=std::get_if<NameExpr>(&n.args[i].value->data))
                                target_dtype=builtin_scalar_type(name->name);
                        }
                    }
                    const auto result_type=checked.raw_types.at(&e);
                    std::vector<long long> expected_shape_prefix;
                    if(result_type.kind==TypeKind::Union){
                        for(const auto& candidate:result_type.cases){
                            if(candidate.kind!=TypeKind::Tensor) continue;
                            if(expected_shape_prefix.empty())
                                expected_shape_prefix=candidate.tensor_shape_prefix;
                            else if(expected_shape_prefix!=candidate.tensor_shape_prefix)
                                expected_shape_prefix.clear();
                        }
                    }
                    block->instructions.push_back(
                        ImageRead{out,path,result_type,target_dtype,target_channels,
                                  std::move(expected_shape_prefix)});
                    release_arg(0,path);
                    return out;
                }
                case BuiltinCallable::ImageWrite: {
                    auto path=expr(*n.args[0].value);
                    auto image=expr(*n.args[1].value);
                    ValueId quality;
                    if(n.args.size()==3){
                        quality=expr(*n.args[2].value);
                    }else{
                        quality=fresh();
                        block->instructions.push_back(
                            ConstantInt{quality,"95",Type::simple(TypeKind::Int)});
                    }
                    auto out=fresh();
                    block->instructions.push_back(
                        ImageWrite{out,path,image,quality,checked.raw_types.at(&e)});
                    release_arg(0,path);
                    release_arg(1,image);
                    if(n.args.size()==3) release_arg(2,quality);
                    return out;
                }
                case BuiltinCallable::ImageTensorCrop:
                case BuiltinCallable::ImageTensorResize:
                case BuiltinCallable::ImageTensorFlipHorizontal:
                case BuiltinCallable::ImageTensorFlipVertical:
                case BuiltinCallable::ImageTensorRotate90:
                case BuiltinCallable::ImageTensorRotate270:
                case BuiltinCallable::ImageTensorGrayscale:
                case BuiltinCallable::ImageTensorThreshold:
                case BuiltinCallable::ImageTensorBlur:
                case BuiltinCallable::ImageTensorFilter:
                case BuiltinCallable::ImageTensorDilate:
                case BuiltinCallable::ImageTensorErode: {
                    std::vector<ValueId> args;
                    args.reserve(n.args.size());
                    for (const auto& argument : n.args) {
                        args.push_back(expr(*argument.value));
                    }
                    auto out=fresh();
                    const auto input_type=type_of(*n.args[0].value);
                    block->instructions.push_back(ImageTensorOp{
                        out,*resolution.builtin,std::move(args),
                        checked.raw_types.at(&e),*input_type.first,
                        static_cast<std::uint32_t>(e.span.start.line),
                        static_cast<std::uint32_t>(e.span.start.column)});
                    for(std::size_t i=0;i<n.args.size();++i)
                        release_arg(i,std::get<ImageTensorOp>(block->instructions.back()).args[i]);
                    return out;
                }
                case BuiltinCallable::HttpGet: {
                    auto url=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(HttpGet{out,url,checked.raw_types.at(&e)});
                    release_arg(0,url);
                    return out;
                }
                case BuiltinCallable::HttpHeader: {
                    auto response=receiver_value(),name=expr(*n.args[0].value),out=fresh();
                    block->instructions.push_back(HttpHeader{out,response,name,checked.raw_types.at(&e)});
                    release_arg(0,name);
                    return out;
                }
            }
        }

        if(resolution.kind!=CallKind::Function) {
            throw std::logic_error("Unsupported resolved call kind.");
        }
        const auto& sig=checked.functions.at(resolution.target);
        auto lowered=lower_call_arguments(n.args,sig,0,resolution.target);
        const auto out=(sig.result.kind==TypeKind::Void||sig.result.kind==TypeKind::Never)?0:fresh();
        block->instructions.push_back(Call{out,resolution.target,std::move(lowered.args),sig.result,static_cast<std::uint32_t>(e.span.start.line),static_cast<std::uint32_t>(e.span.start.column)});
        if(sig.result.kind!=TypeKind::Never) release_borrowed_temporaries(lowered);
        return out;
    }

    void lower_for(const ForStmt& n) {
        const auto* range_call=std::get_if<CallExpr>(&n.iterable->data);
        const auto range_resolution=checked.call_resolutions.find(n.iterable.get());
        if(range_call && range_resolution!=checked.call_resolutions.end() &&
           range_resolution->second.kind==CallKind::Builtin &&
           range_resolution->second.builtin==BuiltinCallable::Range){
            const auto* call=range_call;
            std::vector<ValueId> av; for(const auto& a:call->args) av.push_back(expr(*a.value));
            ValueId start,end,step;
            if(av.size()==1){ start=const_int(0); end=av[0]; step=const_int(1); }
            else if(av.size()==2){ start=av[0]; end=av[1]; step=const_int(1); }
            else { start=av[0]; end=av[1]; step=av[2]; }
            block->instructions.push_back(RangeCheckStep{step,static_cast<std::uint32_t>(n.iterable->span.start.line),static_cast<std::uint32_t>(n.iterable->span.start.column)});
            const auto i_name=hidden("range.i"), end_name=hidden("range.end"), step_name=hidden("range.step");
            locals[i_name]=locals[end_name]=locals[step_name]=Type::simple(TypeKind::Int); const auto iter_name=bind_source_local(n.name,Type::simple(TypeKind::Int));
            block->instructions.push_back(StoreLocal{i_name,start,locals[i_name]}); block->instructions.push_back(StoreLocal{end_name,end,locals[end_name]}); block->instructions.push_back(StoreLocal{step_name,step,locals[step_name]});
            const auto cond=label("for.cond"), body_name=label("for.body"),
                       step_label=label("for.step"), done=label("for.end"); block->instructions.push_back(Jump{cond});
            auto& cb=add_block(cond); block=&cb;
            auto si=fresh(), ii=fresh(), ee=fresh(); block->instructions.push_back(LoadLocal{si,step_name,locals[step_name]}); block->instructions.push_back(LoadLocal{ii,i_name,locals[i_name]}); block->instructions.push_back(LoadLocal{ee,end_name,locals[end_name]});
            auto zero=const_int(0), pos=fresh(), neg=fresh(), lt=fresh(), gt=fresh(), a=fresh(), b=fresh(), c=fresh();
            block->instructions.push_back(Binary{pos,">",si,zero,locals[step_name],Type::simple(TypeKind::Bool)}); block->instructions.push_back(Binary{neg,"<",si,zero,locals[step_name],Type::simple(TypeKind::Bool)});
            block->instructions.push_back(Binary{lt,"<",ii,ee,locals[i_name],Type::simple(TypeKind::Bool)}); block->instructions.push_back(Binary{gt,">",ii,ee,locals[i_name],Type::simple(TypeKind::Bool)});
            block->instructions.push_back(Binary{a,"and",pos,lt,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)}); block->instructions.push_back(Binary{b,"and",neg,gt,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)}); block->instructions.push_back(Binary{c,"or",a,b,Type::simple(TypeKind::Bool),Type::simple(TypeKind::Bool)}); block->instructions.push_back(Branch{c,body_name,done});
            auto& bb=add_block(body_name); block=&bb; auto cur=fresh(); block->instructions.push_back(LoadLocal{cur,i_name,locals[i_name]}); block->instructions.push_back(StoreLocal{iter_name,cur,locals[iter_name]});
            loop_targets.push_back({step_label,done});
            for(const auto& s:n.body){ stmt(*s); if(terminated()) break; }
            loop_targets.pop_back();
            if(!terminated()) block->instructions.push_back(Jump{step_label});
            auto& sb=add_block(step_label); block=&sb;
            auto x=fresh(), st=fresh(), nx=fresh(); block->instructions.push_back(LoadLocal{x,i_name,locals[i_name]}); block->instructions.push_back(LoadLocal{st,step_name,locals[step_name]}); block->instructions.push_back(Binary{nx,"+",x,st,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)}); block->instructions.push_back(StoreLocal{i_name,nx,locals[i_name]}); block->instructions.push_back(Jump{cond});
            auto& db=add_block(done); block=&db; return;
        }
        const auto array_type=type_of(*n.iterable);
        const bool iterable_initialization_proven =
            array_type.kind == TypeKind::Array &&
            array_expression_fully_initialized(*n.iterable);
        auto array=expr(*n.iterable);
        const auto item=array_type.kind==TypeKind::Bin?Type::simple(TypeKind::Bin):*array_type.first;
        const bool iterable_temporary=expression_owns_result(*n.iterable);
        const auto idx_name=hidden("for.index"), len_name=hidden("for.length"); locals[idx_name]=locals[len_name]=Type::simple(TypeKind::Int); const auto iter_name=bind_source_local(n.name,item);
        auto len=fresh();
        if(array_type.kind==TypeKind::Bin) block->instructions.push_back(BinLength{len,array}); else block->instructions.push_back(ArrayLength{len,array});
        auto zero=const_int(0); block->instructions.push_back(StoreLocal{idx_name,zero,locals[idx_name]}); block->instructions.push_back(StoreLocal{len_name,len,locals[len_name]});
        const auto cond=label("for.cond"), body_name=label("for.body"),
                   step_label=label("for.step"), break_label=label("for.break"),
                   done=label("for.end"); block->instructions.push_back(Jump{cond});
        auto& cb=add_block(cond); block=&cb; auto idx=fresh(), l=fresh(), cmp=fresh(); block->instructions.push_back(LoadLocal{idx,idx_name,locals[idx_name]}); block->instructions.push_back(LoadLocal{l,len_name,locals[len_name]}); block->instructions.push_back(Binary{cmp,"<",idx,l,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)}); block->instructions.push_back(Branch{cmp,body_name,done});
        auto& bb=add_block(body_name); block=&bb; auto ix=fresh(), element=fresh(); block->instructions.push_back(LoadLocal{ix,idx_name,locals[idx_name]});
        if(array_type.kind==TypeKind::Bin) {
            block->instructions.push_back(BinGet{element,array,ix,0,0,true});
        } else {
            block->instructions.push_back(ArrayGet{
                element,array,ix,item,
                static_cast<std::uint32_t>(n.iterable->span.start.line),
                static_cast<std::uint32_t>(n.iterable->span.start.column),
                iterable_initialization_proven,true});
        }
        if(array_type.kind!=TypeKind::Bin) element=copy_value(element,item);
        block->instructions.push_back(StoreLocal{iter_name,element,item});
        loop_targets.push_back({step_label,break_label});
        for(const auto& s:n.body){ stmt(*s); if(terminated()) break; }
        loop_targets.pop_back();
        if(!terminated()) block->instructions.push_back(Jump{step_label});

        auto write_back=[&](){
            if(!n.writable)return;
            auto ix2=fresh(), val=fresh();
            block->instructions.push_back(LoadLocal{ix2,idx_name,locals[idx_name]});
            block->instructions.push_back(LoadLocal{val,iter_name,item});
            if(array_type.kind==TypeKind::Bin) {
                block->instructions.push_back(BinSet{array,ix2,val,0,0,true});
            } else {
                val=copy_value(val,item);
                block->instructions.push_back(ArraySet{
                array,ix2,val,item,
                static_cast<std::uint32_t>(n.iterable->span.start.line),
                static_cast<std::uint32_t>(n.iterable->span.start.column),
                iterable_initialization_proven,true});
            }
        };

        auto& sb=add_block(step_label); block=&sb; write_back();
        auto old=fresh(); block->instructions.push_back(LoadLocal{old,idx_name,locals[idx_name]}); auto one=const_int(1), next=fresh(); block->instructions.push_back(Binary{next,"+",old,one,Type::simple(TypeKind::Int),Type::simple(TypeKind::Int)}); block->instructions.push_back(StoreLocal{idx_name,next,locals[idx_name]}); block->instructions.push_back(Jump{cond});
        auto& brb=add_block(break_label); block=&brb; write_back(); block->instructions.push_back(Jump{done});
        auto& db=add_block(done); block=&db;
        if(iterable_temporary && requires_lifetime_management(array_type))
            block->instructions.push_back(Release{array,array_type});
    }

    void stmt(const Stmt& s) {
        block->instructions.push_back(SourceLocation{
            static_cast<std::uint32_t>(s.span.start.line),
            static_cast<std::uint32_t>(s.span.start.column)});
        if(const auto* n=std::get_if<BindingStmt>(&s.data)){
            const auto t=checked.binding_types.at(&s);
            if(n->reference){
                const auto ir_name=bind_source_reference(n->name,t);
                block->instructions.push_back(DeclareReference{ir_name,t,n->is_const});
                auto address=address_of(*n->value, !n->is_const);
                block->instructions.push_back(BindReference{ir_name,address});
                return;
            }
            const auto ir_name=bind_source_local(n->name,t);
            block->instructions.push_back(DeclareLocal{ir_name,t});

            if(t.kind==TypeKind::Tensor || t.kind==TypeKind::Neural){
                auto captured=capture_extents(
                    n->declared_type.tensor_shape_expressions,"shape.extent");
                if(!captured.empty()) shaped_constraints[ir_name]=captured;
                if(n->value && !captured.empty()){
                    if(const auto* call=std::get_if<CallExpr>(&n->value->data)){
                        const auto found=checked.call_resolutions.find(n->value.get());
                        const bool has_explicit_shape=std::any_of(
                            call->args.begin(),call->args.end(),[](const auto& argument){
                                return !argument.name || *argument.name!="gpu";
                            });
                        if(!has_explicit_shape && found!=checked.call_resolutions.end() &&
                           found->second.kind==CallKind::Builtin &&
                           (found->second.builtin==BuiltinCallable::TensorZeros ||
                            found->second.builtin==BuiltinCallable::TensorOnes)){
                            contextual_tensor_shapes[n->value.get()]=captured;
                        }
                    }
                }
            }else if(t.kind==TypeKind::Array){
                auto captured=capture_extents(
                    n->declared_type.dimension_expressions,"array.extent");
                if(!captured.empty()) array_constraints[ir_name]=captured;
            }

            if(n->value){
                const bool array_full =
                    t.kind == TypeKind::Array &&
                    array_expression_fully_initialized(*n->value);
                auto v=destination_value(*n->value,t);
                if(const auto found=shaped_constraints.find(ir_name);
                   found!=shaped_constraints.end()){
                    emit_shaped_constraint(v,t.kind,found->second,s.span);
                }
                if(const auto found=array_constraints.find(ir_name);
                   found!=array_constraints.end()){
                    emit_array_constraints(v,t,found->second,0,s.span);
                }
                block->instructions.push_back(StoreLocal{ir_name,v,t});
                if (t.kind == TypeKind::Array) {
                    if (array_full) fully_initialized_array_locals.insert(n->name);
                    else fully_initialized_array_locals.erase(n->name);
                }
            } else if(t.kind==TypeKind::Array) {
                const auto captured=array_constraints.find(ir_name);
                if(t.length>=0 || (t.length==-2 && captured!=array_constraints.end() &&
                                   !captured->second.empty() && captured->second[0])){
                    ValueId length{};
                    if(t.length>=0){
                        length=const_int(t.length);
                    }else{
                        length=fresh();
                        block->instructions.push_back(LoadLocal{
                            length,*captured->second[0],Type::simple(TypeKind::Int)});
                    }
                    auto storage=fresh();
                    block->instructions.push_back(ArrayAlloc{storage,length,t,false});
                    block->instructions.push_back(StoreLocal{ir_name,storage,t});
                    fully_initialized_array_locals.erase(n->name);
                }
            }
            return;
        }
        if(const auto* n=std::get_if<RebindStmt>(&s.data)){
            auto address=address_of(*n->target);
            block->instructions.push_back(BindReference{source_reference(n->name),address});
            return;
        }
        if(const auto* n=std::get_if<AssignStmt>(&s.data)){
            const auto t=type_of(*n->target);
            if(!n->compound_op.empty()){
                if(const auto* name=std::get_if<NameExpr>(&n->target->data);
                   name && !checked.field_accesses.contains(n->target.get()) &&
                   !is_source_reference(name->name)){
                    auto old=fresh();
                    const auto local_name=source_local(name->name);
                    block->instructions.push_back(LoadLocal{old,local_name,t});
                    if(t.kind==TypeKind::String && n->compound_op=="+" &&
                       !expression_contains_writable_argument(*n->value)){
                        auto can_move=fresh();
                        block->instructions.push_back(StringCanAppendMove{can_move,old});
                        const auto fast=label("string.plus_assign.move");
                        const auto fallback=label("string.plus_assign.copy");
                        const auto done=label("string.plus_assign.done");
                        block->instructions.push_back(Branch{can_move,fast,fallback});

                        block=&add_block(fast);
                        auto rhs=expr(*n->value);
                        auto moved=fresh();
                        block->instructions.push_back(
                            StringAppendMove{moved,old,std::vector<ValueId>{rhs}});
                        release_temporary(*n->value,rhs);
                        block->instructions.push_back(
                            StoreLocal{local_name,moved,t,false,true});
                        block->instructions.push_back(Jump{done});

                        block=&add_block(fallback);
                        auto copied_rhs=expr(*n->value);
                        auto result=fresh();
                        block->instructions.push_back(Binary{
                            result,n->compound_op,old,copied_rhs,t,t,
                            static_cast<std::uint32_t>(s.span.start.line),
                            static_cast<std::uint32_t>(s.span.start.column)});
                        release_temporary(*n->value,copied_rhs);
                        block->instructions.push_back(StoreLocal{local_name,result,t});
                        block->instructions.push_back(Jump{done});

                        block=&add_block(done);
                        return;
                    }
                    auto rhs=expr(*n->value);
                    auto result=fresh();
                    block->instructions.push_back(Binary{
                        result,n->compound_op,old,rhs,t,t,
                        static_cast<std::uint32_t>(s.span.start.line),
                        static_cast<std::uint32_t>(s.span.start.column)});
                    release_temporary(*n->value,rhs);
                    block->instructions.push_back(
                        StoreLocal{local_name,result,t});
                    return;
                }
                auto address=address_of(*n->target);
                auto old=fresh(); block->instructions.push_back(LoadAddress{old,address,t});
                auto rhs=expr(*n->value);
                auto result=fresh(); block->instructions.push_back(Binary{result,n->compound_op,old,rhs,t,t,static_cast<std::uint32_t>(s.span.start.line),static_cast<std::uint32_t>(s.span.start.column)});
                release_temporary(*n->value,rhs);
                block->instructions.push_back(StoreAddress{address,result,t});
                return;
            }

            // s = s + ... may reuse uniquely-owned string storage. Shared strings
            // keep ordinary immutable value semantics through the fallback path.
            if(t.kind==TypeKind::String &&
               !expression_contains_writable_argument(*n->value)){
                const auto* target_name=std::get_if<NameExpr>(&n->target->data);
                if(target_name && !checked.field_accesses.contains(n->target.get()) &&
                   !is_source_reference(target_name->name)){
                    std::vector<const Expr*> parts;
                    std::vector<const Expr*> pending{n->value.get()};
                    while(!pending.empty()){
                        const auto* current=pending.back();
                        pending.pop_back();
                        const auto* binary=std::get_if<BinaryExpr>(&current->data);
                        if(binary && binary->op=="+" &&
                           type_of(*current).kind==TypeKind::String){
                            pending.push_back(binary->right.get());
                            pending.push_back(binary->left.get());
                        }else{
                            parts.push_back(current);
                        }
                    }
                    const auto* first_name=parts.empty()
                        ? nullptr
                        : std::get_if<NameExpr>(&parts.front()->data);
                    if(first_name && first_name->name==target_name->name &&
                       parts.size()>1){
                        const auto local_name=source_local(target_name->name);
                        auto source=fresh();
                        block->instructions.push_back(LoadLocal{source,local_name,t});
                        auto can_move=fresh();
                        block->instructions.push_back(StringCanAppendMove{can_move,source});

                        const auto fast=label("string.append.move");
                        const auto fallback=label("string.append.copy");
                        const auto done=label("string.append.done");
                        block->instructions.push_back(Branch{can_move,fast,fallback});

                        block=&add_block(fast);
                        std::vector<ValueId> suffixes;
                        suffixes.reserve(parts.size()-1);
                        for(std::size_t i=1;i<parts.size();++i)
                            suffixes.push_back(expr(*parts[i]));
                        auto moved=fresh();
                        block->instructions.push_back(
                            StringAppendMove{moved,source,suffixes});
                        for(std::size_t i=1;i<parts.size();++i)
                            release_temporary(*parts[i],suffixes[i-1]);
                        block->instructions.push_back(
                            StoreLocal{local_name,moved,t,false,true});
                        block->instructions.push_back(Jump{done});

                        block=&add_block(fallback);
                        auto copied=destination_value(*n->value,t);
                        block->instructions.push_back(StoreLocal{local_name,copied,t});
                        block->instructions.push_back(Jump{done});

                        block=&add_block(done);
                        return;
                    }
                }
            }

            // xs = xs.append(value) can reuse xs only when the runtime proves that
            // the allocation is uniquely owned, fully initialized, and not pinned
            // by an interior reference. Otherwise the ordinary value-copy path is
            // preserved exactly.
            if(t.kind==TypeKind::Array && t.length==-1){
                const auto* target_name=std::get_if<NameExpr>(&n->target->data);
                const auto* append=std::get_if<MethodCallExpr>(&n->value->data);
                const auto* receiver_name=append
                    ? std::get_if<NameExpr>(&append->receiver->data)
                    : nullptr;
                const auto target_field =
                    target_name ? checked.field_accesses.find(n->target.get())
                                : checked.field_accesses.end();
                const auto receiver_field =
                    (append && receiver_name)
                        ? checked.field_accesses.find(append->receiver.get())
                        : checked.field_accesses.end();
                const bool simple_append_value = append && append->args.size()==1 && (
                    std::holds_alternative<IntegerExpr>(append->args[0].value->data) ||
                    std::holds_alternative<FloatExpr>(append->args[0].value->data) ||
                    std::holds_alternative<StringExpr>(append->args[0].value->data) ||
                    std::holds_alternative<BoolExpr>(append->args[0].value->data) ||
                    std::holds_alternative<NoneExpr>(append->args[0].value->data) ||
                    std::holds_alternative<NameExpr>(append->args[0].value->data));

                // The compiler-generated map/set storage uses class fields. Reuse
                // unique backing storage for field = field.append(simple_value)
                // without changing source evaluation order. The runtime still
                // rejects the move when aliases or interior references exist.
                if(target_name && append && receiver_name && simple_append_value &&
                   append->method=="append" && append->args.size()==1 &&
                   receiver_name->name==target_name->name &&
                   target_field!=checked.field_accesses.end() &&
                   receiver_field!=checked.field_accesses.end() &&
                   target_field->second.owner==receiver_field->second.owner &&
                   target_field->second.index==receiver_field->second.index){
                    auto object=receiver_value();
                    auto source=fresh();
                    block->instructions.push_back(FieldGet{
                        source,object,target_field->second.index,t});
                    auto can_move=fresh();
                    block->instructions.push_back(ArrayCanAppendMove{can_move,source});

                    const auto fast=label("append.field.move");
                    const auto fallback=label("append.field.copy");
                    const auto done=label("append.field.done");
                    block->instructions.push_back(Branch{can_move,fast,fallback});

                    block=&add_block(fast);
                    auto old_length=fresh();
                    block->instructions.push_back(ArrayLength{old_length,source});
                    auto appended=destination_value(*append->args[0].value,*t.first);
                    auto grown=fresh();
                    block->instructions.push_back(ArrayGrowMove{grown,source,t});
                    block->instructions.push_back(ArraySet{
                        grown,old_length,appended,*t.first,
                        static_cast<std::uint32_t>(s.span.start.line),
                        static_cast<std::uint32_t>(s.span.start.column),true});
                    block->instructions.push_back(FieldSet{
                        object,target_field->second.index,grown,t,true});
                    block->instructions.push_back(Jump{done});

                    block=&add_block(fallback);
                    auto copied=destination_value(*n->value,t);
                    block->instructions.push_back(FieldSet{
                        object,target_field->second.index,copied,t});
                    block->instructions.push_back(Jump{done});

                    block=&add_block(done);
                    return;
                }

                if(target_name && append && receiver_name &&
                   append->method=="append" && append->args.size()==1 &&
                   receiver_name->name==target_name->name &&
                   !checked.field_accesses.contains(n->target.get()) &&
                   !is_source_reference(target_name->name)){
                    const auto local_name=source_local(target_name->name);
                    auto source=fresh();
                    block->instructions.push_back(LoadLocal{source,local_name,t});
                    auto can_move=fresh();
                    block->instructions.push_back(ArrayCanAppendMove{can_move,source});

                    const auto fast=label("append.move");
                    const auto fallback=label("append.copy");
                    const auto done=label("append.done");
                    block->instructions.push_back(Branch{can_move,fast,fallback});

                    block=&add_block(fast);
                    auto old_length=fresh();
                    block->instructions.push_back(ArrayLength{old_length,source});
                    auto appended=destination_value(*append->args[0].value,*t.first);
                    auto grown=fresh();
                    block->instructions.push_back(ArrayGrowMove{grown,source,t});
                    block->instructions.push_back(ArraySet{
                        grown,old_length,appended,*t.first,
                        static_cast<std::uint32_t>(s.span.start.line),
                        static_cast<std::uint32_t>(s.span.start.column),true});
                    block->instructions.push_back(
                        StoreLocal{local_name,grown,t,false,true});
                    block->instructions.push_back(Jump{done});

                    block=&add_block(fallback);
                    auto copied=destination_value(*n->value,t);
                    block->instructions.push_back(StoreLocal{local_name,copied,t});
                    block->instructions.push_back(Jump{done});

                    block=&add_block(done);
                    fully_initialized_array_locals.insert(target_name->name);
                    return;
                }
            }

            const bool assigned_array_full =
                t.kind == TypeKind::Array &&
                array_expression_fully_initialized(*n->value);
            auto v=destination_value(*n->value,t);
            if(const auto* name=std::get_if<NameExpr>(&n->target->data)){
                if(const auto it=checked.field_accesses.find(n->target.get());it!=checked.field_accesses.end()){
                    auto object=receiver_value();block->instructions.push_back(FieldSet{object,it->second.index,v,t});
                } else if(is_source_reference(name->name)) {
                    block->instructions.push_back(StoreReference{source_reference(name->name),v,t});
                } else {
                    const auto local_name=source_local(name->name);
                    if(const auto found=shaped_constraints.find(local_name);
                       found!=shaped_constraints.end()){
                        emit_shaped_constraint(v,t.kind,found->second,s.span);
                    }
                    if(const auto found=array_constraints.find(local_name);
                       found!=array_constraints.end()){
                        emit_array_constraints(v,t,found->second,0,s.span);
                    }
                    block->instructions.push_back(StoreLocal{local_name,v,t});
                    if (t.kind == TypeKind::Array) {
                        if (assigned_array_full) fully_initialized_array_locals.insert(name->name);
                        else fully_initialized_array_locals.erase(name->name);
                    }
                }
            }
            else if(const auto* member=std::get_if<MemberExpr>(&n->target->data)){
                auto object=expr(*member->base);const auto& info=checked.field_accesses.at(n->target.get());
                block->instructions.push_back(FieldSet{object,info.index,v,t});
            }
            else {
                const auto& ix=std::get<IndexExpr>(n->target->data);
                const auto base_type=type_of(*ix.base);
                const bool initialization_proven =
                    base_type.kind==TypeKind::Array &&
                    array_expression_fully_initialized(*ix.base);
                auto a=expr(*ix.base);
                if(base_type.kind==TypeKind::Tensor){
                    std::vector<ValueId> indices;
                    indices.reserve(ix.items.size());
                    for(const auto& item:ix.items) indices.push_back(expr(*item.index));
                    block->instructions.push_back(TensorSet{
                        a,std::move(indices),v,t,
                        static_cast<std::uint32_t>(n->target->span.start.line),
                        static_cast<std::uint32_t>(n->target->span.start.column)});
                }else{
                    auto i=expr(*ix.items.front().index);
                    if(base_type.kind==TypeKind::Bin) {
                        block->instructions.push_back(BinSet{
                            a,i,v,static_cast<std::uint32_t>(n->target->span.start.line),
                            static_cast<std::uint32_t>(n->target->span.start.column),
                            checked.bounds_proven.contains(ix.items.front().index.get())});
                        block->instructions.push_back(Release{v,t});
                    } else block->instructions.push_back(ArraySet{
                        a,i,v,t,static_cast<std::uint32_t>(n->target->span.start.line),
                        static_cast<std::uint32_t>(n->target->span.start.column),
                        initialization_proven,
                        checked.bounds_proven.contains(ix.items.front().index.get())});
                }
            }
            return;
        }
        if(const auto* n=std::get_if<LoopControlStmt>(&s.data)){
            if(loop_targets.empty()) throw std::logic_error("loop control escaped checker");
            block->instructions.push_back(Jump{n->is_continue?loop_targets.back().first:loop_targets.back().second});
            return;
        }
        if(const auto* n=std::get_if<ReturnStmt>(&s.data)){
            auto v=destination_value(*n->value,fn->result);
            if(!return_shaped_constraints.empty()){
                emit_shaped_constraint(
                    v,fn->result.kind,return_shaped_constraints,s.span);
            }
            if(!return_array_constraints.empty()){
                emit_array_constraints(
                    v,fn->result,return_array_constraints,0,s.span);
            }
            block->instructions.push_back(Return{v,fn->result});
            return;
        }
        if(const auto* n=std::get_if<ExprStmt>(&s.data)){
            auto value=expr(*n->value);
            const auto type=type_of(*n->value);
            if(n->value.get()==repl_expression && type.kind!=TypeKind::Void && type.kind!=TypeKind::Never){
                std::vector<std::string> initialized_paths;
                if (const auto it = checked.class_expr_initialized_paths.find(n->value.get());
                    it != checked.class_expr_initialized_paths.end()) {
                    initialized_paths.assign(it->second.begin(), it->second.end());
                    std::sort(initialized_paths.begin(), initialized_paths.end());
                }
                block->instructions.push_back(
                    ReplDisplay{value,type,std::move(initialized_paths)});
            }
            release_temporary(*n->value,value);
            return;
        }
        if(const auto* n=std::get_if<IfStmt>(&s.data)){
            auto cond=expr(*n->condition);
            const auto then_name=label("if.then"), else_name=label("if.else"), end_name=label("if.end");
            block->instructions.push_back(Branch{cond,then_name,else_name});
            auto before=locals;
            auto before_names=local_names;
            const auto before_full=fully_initialized_array_locals;

            auto& tb=add_block(then_name);
            block=&tb;
            fully_initialized_array_locals=before_full;
            for(const auto& x:n->then_body){ stmt(*x); if(terminated()) break; }
            const bool then_continues=!terminated();
            const auto then_full=fully_initialized_array_locals;
            if(then_continues) block->instructions.push_back(Jump{end_name});
            locals=before; local_names=before_names;

            auto& eb=add_block(else_name);
            block=&eb;
            fully_initialized_array_locals=before_full;
            for(const auto& x:n->else_body){ stmt(*x); if(terminated()) break; }
            const bool else_continues=!terminated();
            const auto else_full=fully_initialized_array_locals;
            if(else_continues) block->instructions.push_back(Jump{end_name});
            locals=before; local_names=before_names;

            if(then_continues && else_continues)
                fully_initialized_array_locals=intersect_full_arrays(then_full,else_full);
            else if(then_continues)
                fully_initialized_array_locals=then_full;
            else if(else_continues)
                fully_initialized_array_locals=else_full;
            else
                fully_initialized_array_locals.clear();

            auto& endb=add_block(end_name); block=&endb; return;
        }
        if(const auto* n=std::get_if<WhileStmt>(&s.data)){
            const auto cond_name=label("while.cond"), body_name=label("while.body"), end_name=label("while.end");
            block->instructions.push_back(Jump{cond_name});
            auto before=locals;
            auto before_names=local_names;
            auto& cb=add_block(cond_name);
            block=&cb;
            auto c=expr(*n->condition);
            const auto condition_full=fully_initialized_array_locals;
            block->instructions.push_back(Branch{c,body_name,end_name});

            auto& bb=add_block(body_name);
            block=&bb;
            locals=before;
            local_names=before_names;
            fully_initialized_array_locals=condition_full;
            loop_targets.push_back({cond_name,end_name});
            for(const auto& x:n->body){ stmt(*x); if(terminated()) break; }
            loop_targets.pop_back();
            const bool body_continues=!terminated();
            const auto body_full=fully_initialized_array_locals;
            if(body_continues) block->instructions.push_back(Jump{cond_name});

            locals=before;
            local_names=before_names;
            fully_initialized_array_locals=
                body_continues ? intersect_full_arrays(condition_full,body_full)
                               : condition_full;
            auto& eb=add_block(end_name); block=&eb; return;
        }
        if(const auto* n=std::get_if<ForStmt>(&s.data)){
            auto before=locals;
            auto before_names=local_names;
            const auto before_full=fully_initialized_array_locals;
            const auto loop_full_array=full_array_written_by_for(*n);
            lower_for(*n);
            fully_initialized_array_locals=
                intersect_full_arrays(before_full,fully_initialized_array_locals);
            if(loop_full_array) fully_initialized_array_locals.insert(*loop_full_array);
            locals=before;
            local_names=before_names;
            return;
        }
        const auto& n=std::get<MatchStmt>(s.data);
        const bool container_owned=expression_owns_result(*n.value);
        auto container=expr(*n.value);
        if(container_owned){
            const auto owner_name=hidden("match.container");
            locals[owner_name]=type_of(*n.value);
            block->instructions.push_back(StoreLocal{owner_name,container,locals[owner_name]});
        }
        auto tag=fresh();
        block->instructions.push_back(VariantTag{tag,container});
        const auto mt=type_of(*n.value);
        auto done=label("match.end");
        auto before=locals;
        auto before_names=local_names;
        const auto before_full=fully_initialized_array_locals;
        std::optional<std::unordered_set<std::string>> joined_full;
        for(auto& c:n.cases){
            const auto ct=checked.case_types.at(&c);
            auto yes=label("match.case"),next=label("match.next");
            auto num=const_int(case_index(mt,ct)),test=fresh();
            block->instructions.push_back(Binary{
                test,"==",tag,num,Type::simple(TypeKind::Int),Type::simple(TypeKind::Bool)});
            block->instructions.push_back(Branch{test,yes,next});
            block=&add_block(yes);
            locals=before;
            local_names=before_names;
            fully_initialized_array_locals=before_full;
            if(c.binder&&ct.kind!=TypeKind::Void&&ct.kind!=TypeKind::None){
                auto pv=fresh();
                block->instructions.push_back(VariantPayload{pv,container,ct});
                const bool borrow=
                    requires_value_clone(ct)&&!block_mutates_parameter(c.body,*c.binder);
                if(!borrow)pv=convert(pv,ct,ct,true);
                const auto binder_name=bind_source_local(*c.binder,ct);
                block->instructions.push_back(StoreLocal{binder_name,pv,ct,borrow});
            }
            for(auto&x:c.body){stmt(*x);if(terminated())break;}
            if(!terminated()){
                if(joined_full)
                    *joined_full=intersect_full_arrays(
                        *joined_full,fully_initialized_array_locals);
                else
                    joined_full=fully_initialized_array_locals;
                block->instructions.push_back(Jump{done});
            }
            block=&add_block(next);
            locals=before;
            local_names=before_names;
            fully_initialized_array_locals=before_full;
        }
        block=&add_block(done);
        if(joined_full) fully_initialized_array_locals=std::move(*joined_full);
        else fully_initialized_array_locals.clear();
    }

    void capture_signature_constraints(
        const FunctionDecl& source, std::size_t parameter_offset) {
        for (std::size_t i = 0; i < source.parameters.size(); ++i) {
            const auto ir_index = i + parameter_offset;
            if (ir_index >= fn->parameters.size()) {
                throw std::logic_error("signature parameter offset mismatch");
            }
            const auto& parameter = fn->parameters[ir_index];
            const auto& syntax = source.parameters[i].type;

            if ((parameter.type.kind == TypeKind::Tensor ||
                 parameter.type.kind == TypeKind::Neural) &&
                !syntax.tensor_shape_expressions.empty()) {
                auto captured =
                    capture_extents(syntax.tensor_shape_expressions, "param.shape");
                shaped_constraints[parameter.name] = captured;
                auto value = fresh();
                block->instructions.push_back(
                    LoadLocal{value, parameter.name, parameter.type});
                emit_shaped_constraint(
                    value, parameter.type.kind, captured, source.parameters[i].span);
            }

            if (parameter.type.kind == TypeKind::Array &&
                !syntax.dimension_expressions.empty()) {
                auto captured =
                    capture_extents(syntax.dimension_expressions, "param.array");
                array_constraints[parameter.name] = captured;
                auto value = fresh();
                block->instructions.push_back(
                    LoadLocal{value, parameter.name, parameter.type});
                emit_array_constraints(
                    value, parameter.type, captured, 0, source.parameters[i].span);
            }
        }

        if ((fn->result.kind == TypeKind::Tensor ||
             fn->result.kind == TypeKind::Neural) &&
            !source.return_type.tensor_shape_expressions.empty()) {
            return_shaped_constraints = capture_extents(
                source.return_type.tensor_shape_expressions, "return.shape");
        }
        if (fn->result.kind == TypeKind::Array &&
            !source.return_type.dimension_expressions.empty()) {
            return_array_constraints = capture_extents(
                source.return_type.dimension_expressions, "return.array");
        }
    }

    void begin_function(Function out) {
        module.functions.push_back(std::move(out));fn=&module.functions.back();next_value=1;next_label=0;next_hidden=0;locals.clear();local_names.clear();reference_names.clear();references.clear();fully_initialized_array_locals.clear();shaped_constraints.clear();array_constraints.clear();contextual_tensor_shapes.clear();return_shaped_constraints.clear();return_array_constraints.clear();fn->blocks.push_back(Block{"entry",{}});block=&fn->blocks.back();
        for(const auto& p:fn->parameters){locals[p.name]=p.type;local_names[p.name]=p.name;}
        for(const auto& p:fn->parameters){
            if((p.type.kind!=TypeKind::Tensor&&p.type.kind!=TypeKind::Neural)||
               p.type.tensor_shape_prefix.empty()) continue;
            auto parameter=fresh();
            block->instructions.push_back(LoadLocal{parameter,p.name,p.type});
            std::vector<std::optional<ValueId>> extents;
            extents.reserve(p.type.tensor_shape_prefix.size());
            for(const auto extent:p.type.tensor_shape_prefix){
                if(extent>=0) extents.push_back(const_int(extent));
                else extents.push_back(std::nullopt);
            }
            block->instructions.push_back(ShapedConstraintCheck{
                parameter,p.type.kind,std::move(extents),0,0});
        }
    }

    void lower_function(const FunctionDecl& source){
        Function out; out.name=source.name; out.source_file=source.source_file;
        out.source_line=static_cast<std::uint32_t>(source.span.start.line);
        out.source_column=static_cast<std::uint32_t>(source.span.start.column);
        const auto& sig=checked.functions.at(source.name); out.result=sig.result;
        out.external_symbol=source.external_symbol;
        for(std::size_t i=0;i<sig.parameters.size();++i){const auto& p=sig.parameters[i];out.parameters.push_back(Parameter{
            p.name, p.type, p.writable, parameter_is_borrowed(source.name, i), p.is_const});}
        if(out.external_symbol){module.functions.push_back(std::move(out));return;}
        current_class.clear();begin_function(std::move(out));
        capture_signature_constraints(source,0);
        for(const auto& s:source.body){stmt(*s);if(terminated())break;}
        if(!terminated()&&fn->result.kind==TypeKind::Void)block->instructions.push_back(ReturnVoid{});
    }

    void lower_method(const std::string& class_name,const FunctionDecl& source){
        const auto internal="$method."+class_name+"."+source.name;
        const auto& sig=checked.functions.at(internal);Function out;out.name=internal;
        out.source_file=source.source_file;
        out.source_line=static_cast<std::uint32_t>(source.span.start.line);
        out.source_column=static_cast<std::uint32_t>(source.span.start.column);
        out.result=sig.result;
        for(std::size_t i=0;i<sig.parameters.size();++i){const auto& p=sig.parameters[i];out.parameters.push_back(Parameter{
            p.name, p.type, p.writable,
            p.name=="$receiver"||parameter_is_borrowed(internal,i), p.is_const});}
        current_class=class_name;begin_function(std::move(out));
        capture_signature_constraints(source,1);
        for(const auto& s:source.body){stmt(*s);if(terminated())break;}
        if(!terminated()&&fn->result.kind==TypeKind::Void)block->instructions.push_back(ReturnVoid{});
        current_class.clear();
    }

    void lower_main(const std::vector<StmtPtr>& statements) {
        Function out;
        out.name = "$entry";
        out.source_file = checked.program.root_source_file;
        if (!statements.empty()) {
            out.source_line = static_cast<std::uint32_t>(statements.front()->span.start.line);
            out.source_column = static_cast<std::uint32_t>(statements.front()->span.start.column);
        }
        out.result = Type::simple(TypeKind::Int);
        out.entrypoint = true;
        current_class.clear();
        begin_function(std::move(out));

        bool replaying = repl_replay_prefix_offset != 0;
        if (replaying) block->instructions.push_back(ReplReplayMode{true});
        for (const auto& statement : statements) {
            if (replaying &&
                statement->span.start.offset >= repl_replay_prefix_offset) {
                block->instructions.push_back(ReplReplayMode{false});
                replaying = false;
            }
            stmt(*statement);
            if (terminated()) break;
        }
        if (!terminated()) {
            if (replaying) block->instructions.push_back(ReplReplayMode{false});
            auto zero = const_int(0);
            block->instructions.push_back(Return{zero, Type::simple(TypeKind::Int)});
        }
    }
};

std::string instr_text(const Instruction& i){ std::ostringstream out; std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
    if constexpr(std::is_same_v<T,SourceLocation>)out<<"source "<<n.line<<":"<<n.column;
    if constexpr(std::is_same_v<T,ConstantInt>)out<<"%"<<n.out<<" = const.int "<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ConstantFloat>)out<<"%"<<n.out<<" = const.float "<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ConstantExact>)out<<"%"<<n.out<<" = const.exact "<<n.spelling<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ConstantBool>)out<<"%"<<n.out<<" = const.bool "<<(n.value?"true":"false");
    if constexpr(std::is_same_v<T,ConstantString>)out<<"%"<<n.out<<" = const.string \""<<n.value<<"\"";
    if constexpr(std::is_same_v<T,ArrayMake>)out<<"%"<<n.out<<" = array.make "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ArrayAlloc>)out<<"%"<<n.out<<" = array.alloc %"<<n.length;
    if constexpr(std::is_same_v<T,ClassMake>)out<<"%"<<n.out<<" = class.make "<<type_name(n.type);
    if constexpr(std::is_same_v<T,FieldGet>)out<<"%"<<n.out<<" = field.get %"<<n.object<<", "<<n.index;
    if constexpr(std::is_same_v<T,FieldSet>)out<<"field.set %"<<n.object<<", "<<n.index<<", %"<<n.value;
    if constexpr(std::is_same_v<T,DeclareLocal>)out<<"local "<<n.name<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,DeclareReference>)out<<(n.is_const?"const reference ":"reference ")<<n.name<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,AddressLocal>)out<<"%"<<n.out<<" = address.local "<<n.name;
    if constexpr(std::is_same_v<T,AddressField>)out<<"%"<<n.out<<" = address.field %"<<n.object<<", "<<n.index;
    if constexpr(std::is_same_v<T,AddressElement>)out<<"%"<<n.out<<" = address.element %"<<n.array<<", %"<<n.index<<" : "<<type_name(n.element_type);
    if constexpr(std::is_same_v<T,LoadAddress>)out<<"%"<<n.out<<" = address.load %"<<n.address;
    if constexpr(std::is_same_v<T,StoreAddress>)out<<"address.store %"<<n.address<<", %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,BindReference>)out<<"reference.bind "<<n.name<<", %"<<n.address;
    if constexpr(std::is_same_v<T,ReferenceAddress>)out<<"%"<<n.out<<" = reference.address "<<n.name;
    if constexpr(std::is_same_v<T,LoadReference>)out<<"%"<<n.out<<" = reference.load "<<n.name<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,StoreReference>)out<<"reference.store "<<n.name<<", %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ArrayLength>)out<<"%"<<n.out<<" = array.length %"<<n.array;
    if constexpr(std::is_same_v<T,ArrayCanAppendMove>)out<<"%"<<n.out<<" = array.can_append_move %"<<n.array;
    if constexpr(std::is_same_v<T,ArrayGrowMove>)out<<"%"<<n.out<<" = array.grow_move %"<<n.array<<" : "<<type_name(n.array_type);
    if constexpr(std::is_same_v<T,ArraySorted>)out<<"%"<<n.out<<" = array.sorted %"<<n.array<<" : "<<type_name(n.array_type);
    if constexpr(std::is_same_v<T,StringIndex>)out<<"%"<<n.out<<" = string.index %"<<n.text<<", %"<<n.index;
    if constexpr(std::is_same_v<T,StringLength>)out<<"%"<<n.out<<" = string.length %"<<n.text;
    if constexpr(std::is_same_v<T,StringContains>)out<<"%"<<n.out<<" = string.contains %"<<n.text<<", %"<<n.needle;
    if constexpr(std::is_same_v<T,StringStartsWith>)out<<"%"<<n.out<<" = string.starts_with %"<<n.text<<", %"<<n.prefix;
    if constexpr(std::is_same_v<T,StringEndsWith>)out<<"%"<<n.out<<" = string.ends_with %"<<n.text<<", %"<<n.suffix;
    if constexpr(std::is_same_v<T,StringFind>)out<<"%"<<n.out<<" = string.find %"<<n.text<<", %"<<n.needle;
    if constexpr(std::is_same_v<T,StringSlice>)out<<"%"<<n.out<<" = string.slice %"<<n.text<<", %"<<n.start<<", %"<<n.end;
    if constexpr(std::is_same_v<T,StringTrim>)out<<"%"<<n.out<<" = string.trim %"<<n.text;
    if constexpr(std::is_same_v<T,StringSplit>)out<<"%"<<n.out<<" = string.split %"<<n.text<<", %"<<n.separator;
    if constexpr(std::is_same_v<T,StringUtf8>)out<<"%"<<n.out<<" = string.utf8 %"<<n.text;
    if constexpr(std::is_same_v<T,StringCodepoints>)out<<"%"<<n.out<<" = string.codepoints %"<<n.text;
    if constexpr(std::is_same_v<T,StringJoin>)out<<"%"<<n.out<<" = string.join %"<<n.values<<", %"<<n.separator;
    if constexpr(std::is_same_v<T,StringConcat>){out<<"%"<<n.out<<" = string.concat";for(const auto value:n.values)out<<" %"<<value;}
    if constexpr(std::is_same_v<T,StringCanAppendMove>)out<<"%"<<n.out<<" = string.can_append_move %"<<n.text;
    if constexpr(std::is_same_v<T,StringAppendMove>){out<<"%"<<n.out<<" = string.append_move %"<<n.text;for(const auto value:n.suffixes)out<<" %"<<value;}
    if constexpr(std::is_same_v<T,BinAlloc>)out<<"%"<<n.out<<" = bin.alloc %"<<n.length<<", %"<<n.fill;
    if constexpr(std::is_same_v<T,BinLength>)out<<"%"<<n.out<<" = bin.length %"<<n.bin;
    if constexpr(std::is_same_v<T,BinGet>)out<<"%"<<n.out<<" = bin.get %"<<n.bin<<", %"<<n.index;
    if constexpr(std::is_same_v<T,BinSet>)out<<"bin.set %"<<n.bin<<", %"<<n.index<<", %"<<n.value;
    if constexpr(std::is_same_v<T,MathRoundInt>)out<<"%"<<n.out<<" = math.round-int %"<<n.value;
    if constexpr(std::is_same_v<T,NumericConvert>)out<<"%"<<n.out<<" = convert %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type)<<(n.checked_range?" checked":"");
    if constexpr(std::is_same_v<T,TensorCreate>)out<<"%"<<n.out<<" = tensor.create %"<<n.shape<<" : "<<type_name(n.type)<<" init="<<(n.fill_mode==0?"uninitialized":n.fill_mode==1?"zeros":"ones")<<(n.gpu?" gpu=%"+std::to_string(*n.gpu):" cpu");
    if constexpr(std::is_same_v<T,TensorTransfer>)out<<"%"<<n.out<<" = tensor."<<(n.gpu?"gpu":"cpu")<<" %"<<n.tensor<<(n.gpu?", %"+std::to_string(*n.gpu):"")<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorReshape>)out<<"%"<<n.out<<" = tensor.reshape %"<<n.tensor<<", %"<<n.shape<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorTranspose>)out<<"%"<<n.out<<" = tensor.transpose %"<<n.tensor<<", %"<<n.axis0<<", %"<<n.axis1<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorContiguous>)out<<"%"<<n.out<<" = tensor.contiguous %"<<n.tensor<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorShape>)out<<"%"<<n.out<<" = tensor.shape %"<<n.tensor<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorIsContiguous>)out<<"%"<<n.out<<" = tensor.is_contiguous %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorItem>)out<<"%"<<n.out<<" = tensor.item %"<<n.tensor<<" : "<<type_name(n.element_type);
    if constexpr(std::is_same_v<T,NeuralTrack>)out<<"%"<<n.out<<" = neural.track %"<<n.tensor;
    if constexpr(std::is_same_v<T,NeuralUntrack>)out<<"%"<<n.out<<" = neural.untrack %"<<n.value;
    if constexpr(std::is_same_v<T,NeuralUnary>)out<<"%"<<n.out<<" = neural.unary %"<<n.value;
    if constexpr(std::is_same_v<T,NeuralBinary>)out<<"%"<<n.out<<" = neural.binary "<<n.op<<" %"<<n.left<<", %"<<n.right;
    if constexpr(std::is_same_v<T,NeuralGrad>)out<<"%"<<n.out<<" = neural.grad %"<<n.loss;
if constexpr(std::is_same_v<T,NeuralAffine>)out<<"%"<<n.out<<" = neural.affine %"<<n.input;
if constexpr(std::is_same_v<T,NeuralConvolve2D>)out<<"%"<<n.out<<" = neural.convolve2d %"<<n.input;
if constexpr(std::is_same_v<T,NeuralUpdate>)out<<"neural.update params="<<n.parameters.size();
if constexpr(std::is_same_v<T,NeuralNormalize>)out<<"%"<<n.out<<" = neural.normalize %"<<n.input;
if constexpr(std::is_same_v<T,NeuralRandomMask>)out<<"%"<<n.out<<" = neural.random_mask %"<<n.input;
if constexpr(std::is_same_v<T,NeuralMomentUpdate>)out<<"neural.moment_update params="<<n.parameters.size();
if constexpr(std::is_same_v<T,NeuralSave>)out<<"neural.save leaves="<<n.values.size();
if constexpr(std::is_same_v<T,NeuralLoad>)out<<"neural.load leaves="<<n.targets.size();
    if constexpr(std::is_same_v<T,ArrayNumericCast>)out<<"%"<<n.out<<" = array.numeric_cast %"<<n.array<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,TensorCast>)out<<"%"<<n.out<<" = tensor.numeric_cast %"<<n.tensor<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,NeuralNumericCast>)out<<"%"<<n.out<<" = neural.numeric_cast %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,ShapedConstraintCheck>)out<<"shape.constraint %"<<n.value<<" rank="<<n.extents.size();
    if constexpr(std::is_same_v<T,ExtentEqualCheck>)out<<"extent.check %"<<n.actual<<", %"<<n.expected;
    if constexpr(std::is_same_v<T,StatsMean>)out<<"%"<<n.out<<" = stats.mean %"<<n.tensor;
    if constexpr(std::is_same_v<T,StatsReduce>)out<<"%"<<n.out<<" = stats.reduce %"<<n.tensor;
    if constexpr(std::is_same_v<T,LinearMatmul>)out<<"%"<<n.out<<" = linear.matmul %"<<n.left<<", %"<<n.right<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,LinearDot>)out<<"%"<<n.out<<" = linear.dot %"<<n.left<<", %"<<n.right<<" : "<<type_name(n.element_type);
    if constexpr(std::is_same_v<T,ImageRead>){
        out<<"%"<<n.out<<" = image.read %"<<n.path;
        if(n.target_channels) out<<", channels="<<n.target_channels;
        if(n.target_dtype) out<<", dtype="<<type_name(*n.target_dtype);
        out<<" : "<<type_name(n.result_type);
    }
    if constexpr(std::is_same_v<T,ImageWrite>)out<<"%"<<n.out<<" = image.write %"<<n.path<<", %"<<n.image<<", quality %"<<n.quality<<" : "<<type_name(n.result_type);
    if constexpr(std::is_same_v<T,ImageTensorOp>)out<<"%"<<n.out<<" = image.tensor.op";
    if constexpr(std::is_same_v<T,TensorBinary>)out<<"%"<<n.out<<" = tensor.binary "<<n.op<<" %"<<n.left<<", %"<<n.right<<" : "<<type_name(n.result_type);
    if constexpr(std::is_same_v<T,TensorIndex>){
        out<<"%"<<n.out<<" = tensor.index %"<<n.tensor<<" [";
        for(std::size_t i=0;i<n.items.size();++i){
            if(i)out<<", ";
            const auto& item=n.items[i];
            if(!item.slice){if(item.index)out<<"%"<<*item.index;else out<<"?";continue;}
            if(item.start)out<<"%"<<*item.start;
            out<<":";
            if(item.stop)out<<"%"<<*item.stop;
            if(item.step){out<<":%"<<*item.step;}
        }
        out<<"] : "<<type_name(n.type);
    }
    if constexpr(std::is_same_v<T,TensorSet>){
        out<<"tensor.set %"<<n.tensor<<" [";
        for(std::size_t i=0;i<n.indices.size();++i){if(i)out<<", ";out<<"%"<<n.indices[i];}
        out<<"], %"<<n.value<<" : "<<type_name(n.element_type);
    }
    if constexpr(std::is_same_v<T,ParseNumber>)out<<"%"<<n.out<<" = parse %"<<n.text<<" as "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,NumericAbs>)out<<"%"<<n.out<<" = abs %"<<n.value;
    if constexpr(std::is_same_v<T,Sqrt>)out<<"%"<<n.out<<" = sqrt %"<<n.value;
    if constexpr(std::is_same_v<T,MathUnary>)out<<"%"<<n.out<<" = math.unary %"<<n.value;
    if constexpr(std::is_same_v<T,MathPow>)out<<"%"<<n.out<<" = math.pow %"<<n.base<<", %"<<n.exponent;
    if constexpr(std::is_same_v<T,CliArgument>)out<<"%"<<n.out<<" = cli.argument %"<<n.index;
    if constexpr(std::is_same_v<T,CliOption>)out<<"%"<<n.out<<" = cli.option %"<<n.name;
    if constexpr(std::is_same_v<T,CliFlag>)out<<"%"<<n.out<<" = cli.flag %"<<n.name;
    if constexpr(std::is_same_v<T,CliFinish>)out<<"cli.finish";
    if constexpr(std::is_same_v<T,FileRead>)out<<"%"<<n.out<<" = file.read %"<<n.path;
    if constexpr(std::is_same_v<T,FileReadBin>)out<<"%"<<n.out<<" = file.read_bin %"<<n.path;
    if constexpr(std::is_same_v<T,FileWrite>)out<<"%"<<n.out<<" = file.write %"<<n.path<<", %"<<n.text;
    if constexpr(std::is_same_v<T,FileWriteBin>)out<<"%"<<n.out<<" = file.write_bin %"<<n.path<<", %"<<n.bin;
    if constexpr(std::is_same_v<T,FileExists>)out<<"%"<<n.out<<" = file.exists %"<<n.path;
    if constexpr(std::is_same_v<T,FileIsDirectory>)out<<"%"<<n.out<<" = file.is_directory %"<<n.path;
    if constexpr(std::is_same_v<T,FileRemove>)out<<"%"<<n.out<<" = file.remove %"<<n.path;
    if constexpr(std::is_same_v<T,FileCopy>)out<<"%"<<n.out<<" = file.copy %"<<n.source<<", %"<<n.destination;
    if constexpr(std::is_same_v<T,FileMove>)out<<"%"<<n.out<<" = file.move %"<<n.source<<", %"<<n.destination;
    if constexpr(std::is_same_v<T,FileMkdir>)out<<"%"<<n.out<<" = file.mkdir %"<<n.path;
    if constexpr(std::is_same_v<T,FileList>)out<<"%"<<n.out<<" = file.list %"<<n.path<<", recursive %"<<n.recursive;
    if constexpr(std::is_same_v<T,EnvironmentGet>)out<<"%"<<n.out<<" = environment.get %"<<n.name;
    if constexpr(std::is_same_v<T,EnvironmentHas>)out<<"%"<<n.out<<" = environment.has %"<<n.name;
    if constexpr(std::is_same_v<T,TestAssert>)out<<"test.assert %"<<n.condition;
    if constexpr(std::is_same_v<T,TimeNow>)out<<"%"<<n.out<<" = time.now";
    if constexpr(std::is_same_v<T,TimeSince>)out<<"%"<<n.out<<" = time.since %"<<n.start;
    if constexpr(std::is_same_v<T,TimeSeconds>)out<<"%"<<n.out<<" = time.seconds %"<<n.seconds;
    if constexpr(std::is_same_v<T,TimeSleep>)out<<"time.sleep %"<<n.duration;
    if constexpr(std::is_same_v<T,RandomGenerator>)out<<"%"<<n.out<<" = random.generator %"<<n.seed;
    if constexpr(std::is_same_v<T,RandomInt>)out<<"%"<<n.out<<" = random.int %"<<n.start<<", %"<<n.end;
    if constexpr(std::is_same_v<T,RandomFloat>)out<<"%"<<n.out<<" = random.float";
    if constexpr(std::is_same_v<T,RandomBool>)out<<"%"<<n.out<<" = random.bool";
    if constexpr(std::is_same_v<T,ProcessRun>)out<<"%"<<n.out<<" = process.run %"<<n.program<<", %"<<n.args;
    if constexpr(std::is_same_v<T,JsonParse>)out<<"%"<<n.out<<" = json.parse %"<<n.text;
    if constexpr(std::is_same_v<T,JsonKind>)out<<"%"<<n.out<<" = json.kind";
    if constexpr(std::is_same_v<T,JsonSize>)out<<"%"<<n.out<<" = json.size";
    if constexpr(std::is_same_v<T,JsonGet>)out<<"%"<<n.out<<" = json.get %"<<n.key;
    if constexpr(std::is_same_v<T,JsonAt>)out<<"%"<<n.out<<" = json.at %"<<n.index;
    if constexpr(std::is_same_v<T,JsonText>)out<<"%"<<n.out<<" = json.text";
    if constexpr(std::is_same_v<T,JsonInteger>)out<<"%"<<n.out<<" = json.integer";
    if constexpr(std::is_same_v<T,JsonNumber>)out<<"%"<<n.out<<" = json.number";
    if constexpr(std::is_same_v<T,JsonBigInt>)out<<"%"<<n.out<<" = json.bigint";
    if constexpr(std::is_same_v<T,JsonBigReal>)out<<"%"<<n.out<<" = json.bigreal";
    if constexpr(std::is_same_v<T,JsonBoolean>)out<<"%"<<n.out<<" = json.boolean";
    if constexpr(std::is_same_v<T,JsonEncode>)out<<"%"<<n.out<<" = json.encode";
    if constexpr(std::is_same_v<T,JsonEqual>)out<<"%"<<n.out<<" = json.equal";
    if constexpr(std::is_same_v<T,HttpGet>)out<<"%"<<n.out<<" = http.get %"<<n.url;
    if constexpr(std::is_same_v<T,HttpHeader>)out<<"%"<<n.out<<" = http.header %"<<n.name;
    if constexpr(std::is_same_v<T,NumericMinMax>)out<<"%"<<n.out<<" = "<<(n.maximum?"max ":"min ")<<"%"<<n.left<<", %"<<n.right;
    if constexpr(std::is_same_v<T,ArrayGet>)out<<"%"<<n.out<<" = array.get %"<<n.array<<", %"<<n.index;
    if constexpr(std::is_same_v<T,ArraySet>)out<<"array.set %"<<n.array<<", %"<<n.index<<", %"<<n.value;
    if constexpr(std::is_same_v<T,Clone>)out<<"%"<<n.out<<" = clone %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Retain>)out<<"%"<<n.out<<" = retain %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Release>)out<<"release %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Unary>)out<<"%"<<n.out<<" = "<<n.op<<" %"<<n.operand;
    if constexpr(std::is_same_v<T,Binary>)out<<"%"<<n.out<<" = "<<n.op<<" %"<<n.left<<", %"<<n.right;
    if constexpr(std::is_same_v<T,ToString>)out<<"%"<<n.out<<" = text %"<<n.value<<" : "<<type_name(n.source_type);
    if constexpr(std::is_same_v<T,FormatNumber>){
        out<<"%"<<n.out<<" = format %"<<n.value<<" : "<<type_name(n.source_type);
        if(n.integer_width)out<<" int="<<*n.integer_width;
        if(n.fractional_digits)out<<" frac="<<*n.fractional_digits;
        if(n.significant_digits)out<<" sig="<<*n.significant_digits;
        if(n.zero)out<<" zero";
    }
    if constexpr(std::is_same_v<T,LoadLocal>)out<<"%"<<n.out<<" = load "<<n.name;
    if constexpr(std::is_same_v<T,StoreLocal>)out<<(n.borrowed?"store.borrow ":"store ")<<n.name<<", %"<<n.value;
    if constexpr(std::is_same_v<T,Call>){
        if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never)out<<"%"<<n.out<<" = ";
        out<<"call "<<n.callee<<"(";
        for(std::size_t index=0;index<n.args.size();++index){
            if(index)out<<", ";
            if(n.args[index].writable_address)out<<"&%"<<*n.args[index].writable_address;
            else out<<"%"<<n.args[index].value;
        }
        out<<")";
    }
    if constexpr(std::is_same_v<T,VariantMake>)out<<"%"<<n.out<<" = variant "<<n.tag;
    if constexpr(std::is_same_v<T,VariantTag>)out<<"%"<<n.out<<" = variant.tag %"<<n.container;
    if constexpr(std::is_same_v<T,VariantPayload>)out<<"%"<<n.out<<" = variant.payload %"<<n.container;
    if constexpr(std::is_same_v<T,Print>)out<<"print %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Write>)out<<"write %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ReplDisplay>)out<<"repl.display %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ReplReplayMode>)out<<"repl.replay "<<(n.active?"on":"off");
    if constexpr(std::is_same_v<T,Input>)out<<"%"<<n.out<<" = input";
    if constexpr(std::is_same_v<T,Exit>)out<<"exit %"<<n.status;
    if constexpr(std::is_same_v<T,RangeCheckStep>)out<<"range.check_step %"<<n.step;
    if constexpr(std::is_same_v<T,Return>)out<<"return %"<<n.value;
    if constexpr(std::is_same_v<T,ReturnVoid>)out<<"return";
    if constexpr(std::is_same_v<T,Jump>)out<<"jump "<<n.target;
    if constexpr(std::is_same_v<T,Branch>)out<<"branch %"<<n.condition<<", "<<n.if_true<<", "<<n.if_false;
},i);return out.str();}

} // namespace

Module lower(
    const CheckedProgram& checked, const Expr* repl_expression,
    std::size_t replay_prefix_bytes) {
    Lowerer l(checked, repl_expression, replay_prefix_bytes);
    for(const auto& [name,info]:checked.classes){ClassLayout layout;layout.name=name;for(const auto& field:info.fields){layout.field_names.push_back(field.name);layout.fields.push_back(field.type);}l.module.classes.push_back(std::move(layout));}
    for(const auto&f:checked.program.functions)l.lower_function(f);
    for(const auto&c:checked.program.classes)for(const auto&m:c.methods){const auto internal="$method."+c.name+"."+m.name;if(checked.functions.contains(internal))l.lower_method(c.name,m);}
    l.lower_main(checked.program.statements);return std::move(l.module);
}
std::string dump(const Module& module){std::ostringstream out;out<<"quidra-ir "<<ir_version<<"\n";for(const auto&c:module.classes){out<<"class "<<c.name<<"\n";}for(const auto&fn:module.functions){out<<"function "<<fn.name<<"(";for(std::size_t i=0;i<fn.parameters.size();++i){if(i)out<<", ";const auto&p=fn.parameters[i];if(p.is_const)out<<"const ";out<<type_name(p.type)<<" "<<(p.writable?"&":"")<<p.name;}out<<") -> "<<type_name(fn.result);if(fn.external_symbol)out<<" = \""<<*fn.external_symbol<<"\"";out<<"\n";for(const auto&b:fn.blocks){out<<b.label<<":\n";for(const auto&i:b.instructions){if(std::holds_alternative<SourceLocation>(i))continue;out<<"  "<<instr_text(i)<<"\n";}}out<<"end\n";}return out.str();}

} // namespace quidra::ir
