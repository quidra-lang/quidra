// Function lowering: begins each function (its blocks, counters and
// facts), captures signature shape constraints, and lowers functions,
// methods, constructors and the entry, including the REPL replay of the
// accepted prefix.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/member_function_names.hpp"
#include <stdexcept>
#include <unordered_set>
#include <algorithm>

namespace quidra::lowering {

void FunctionLowering::capture_signature_constraints(
    const FunctionDecl& source, std::size_t parameter_offset) {
    const auto shape_unit = checked_.signature_shapes.find(&source);
    if (shape_unit != checked_.signature_shapes.end()) {
        std::vector<ValueId> values;
        for (std::size_t p = 0; p < source.parameters.size(); ++p) {
            const auto& parameter = builder_.function()->parameters.at(p + parameter_offset);
            const auto value = builder_.fresh();
            builder_.emit(LoadLocal{value, parameter.name, parameter.type});
            values.push_back(value);
        }
        const auto& unit = shape_unit->second;
        const auto captured = shape_constraints_.capture_signature_shape(
            unit, checked_.shape_plans.at(unit.source.get()), values, source.span);
        for (std::size_t p = 0; p < unit.parts.size(); ++p) {
            const auto& part = unit.parts[p];
            if (part.parameter == 0) {
                shapes_.constrain_return_tensor(CapturedExtents(captured[p]));
            } else {
                const auto& parameter = builder_.function()->parameters.at(
                    part.parameter - 1 + parameter_offset);
                shapes_.constrain_tensor(parameter.name, captured[p]);
            }
        }
    }
    for (std::size_t i = 0; i < source.parameters.size(); ++i) {
        const auto ir_index = i + parameter_offset;
        if (ir_index >= builder_.function()->parameters.size()) {
            throw std::logic_error("signature parameter offset mismatch");
        }
        const auto& parameter = builder_.function()->parameters[ir_index];
        const auto& syntax = source.parameters[i].type;

        if (shape_unit == checked_.signature_shapes.end() &&
            parameter.type.kind == TypeKind::Tensor && !syntax.brace_shape &&
            !syntax.tensor_shape_expressions.empty()) {
            auto captured =
                shape_constraints_.capture_extents(syntax.tensor_shape_expressions, "param.shape");
            shapes_.constrain_tensor(parameter.name, captured);
            auto value = builder_.fresh();
            builder_.emit(
                LoadLocal{value, parameter.name, parameter.type});
            shape_constraints_.emit_shaped_constraint(
                value, parameter.type.kind, captured, source.parameters[i].span);
        }

        if (parameter.type.kind == TypeKind::Array &&
            !syntax.dimension_expressions.empty()) {
            auto captured =
                shape_constraints_.capture_extents(syntax.dimension_expressions, "param.array");
            shapes_.constrain_array(parameter.name, captured);
            auto value = builder_.fresh();
            builder_.emit(
                LoadLocal{value, parameter.name, parameter.type});
            shape_constraints_.emit_array_constraints(
                value, parameter.type, captured, 0, source.parameters[i].span);
        }
    }

    if (shape_unit == checked_.signature_shapes.end() &&
        builder_.function()->result.kind == TypeKind::Tensor && !source.return_type.brace_shape &&
        !source.return_type.tensor_shape_expressions.empty()) {
        shapes_.constrain_return_tensor(shape_constraints_.capture_extents(
            source.return_type.tensor_shape_expressions, "return.shape"));
    }
    if (builder_.function()->result.kind == TypeKind::Array &&
        !source.return_type.dimension_expressions.empty()) {
        shapes_.constrain_return_array(shape_constraints_.capture_extents(
            source.return_type.dimension_expressions, "return.array"));
    }
}

// A function begins with the per-function components emptied (its builder
// starts the value, label and hidden-name counters at 1, 0 and 0), an entry
// block, its parameters bound as locals, and the checks of the shape
// prefixes that its tensor parameter types fix. Not reset here: the
// per-program components (locator, borrows), the ones the callers set around
// a function (enclosing class, REPL replay) and the ones that follow the
// recursion (nesting depth, loop targets).
void FunctionLowering::begin_function(Function out, bool signature_layout) {
    shape_captures_.clear();
    split_shape_entry_ = false;
    module_.functions.push_back(std::move(out));
    builder_=FunctionBuilder(module_.functions.back(),1,0,0);
    scope_.reset();
    facts_.reset();
    shapes_.reset();
    builder_.enter(builder_.add_block("entry"));
    for(const auto& p:builder_.function()->parameters){scope_.bind_parameter(p.name,p.type);}
    for(const auto& p:builder_.function()->parameters){
        if(signature_layout||builder_.function()->parallel_node||p.type.kind!=TypeKind::Tensor||
           p.type.tensor_shape_prefix.empty()) continue;
        auto parameter=builder_.fresh();
        builder_.emit(LoadLocal{parameter,p.name,p.type});
        std::vector<std::optional<ValueId>> extents;
        extents.reserve(p.type.tensor_shape_prefix.size());
        for(const auto extent:p.type.tensor_shape_prefix){
            if(extent>=0) extents.push_back(builder_.const_int(extent));
            else extents.push_back(std::nullopt);
        }
        builder_.emit(ShapedConstraintCheck{
            parameter,p.type.kind,std::move(extents),{0,0},{},{},p.type.tensor_shape_ellipsis});
    }
}

// The IR function that `source` declares under `name`: its source location,
// result and parameters, each passed borrowed when BorrowInference says so
// (and a method's receiver always, when `borrowed_receiver`). Shared by
// functions, methods and constructors.
Function FunctionLowering::function_header(
    const std::string& name, const FunctionDecl& source, const FunctionType& sig,
    bool borrowed_receiver) {
    Function out; out.name=name; out.source_file=source.source_file;
    out.declaration = source.declaration;
    out.source_line=static_cast<std::uint32_t>(source.span.start.line);
    out.source_column=static_cast<std::uint32_t>(source.span.start.column);
    out.result=sig.result;
    for(std::size_t i=0;i<sig.parameters.size();++i){const auto& p=sig.parameters[i];
        // An extern's const string, bin or tensor parameter is a borrow
        // passed by address (CallLowering): the form of `const T &`.
        if(source.external_symbol && !p.writable && p.is_const &&
           (p.type.kind==TypeKind::String||p.type.kind==TypeKind::Bin||p.type.kind==TypeKind::Tensor||is_foreign_tensor_array(p.type))){
            out.parameters.push_back(Parameter{p.name, p.type, true, false, true});
            continue;
        }
        out.parameters.push_back(Parameter{
        p.name, p.type, p.writable,
        (borrowed_receiver&&p.name=="$receiver")||borrows_.parameter_is_borrowed(name,i), p.is_const});}
    ir::analysis::SemanticBinding binding;
    binding.semantic_key = name; // Exact checked signature key supplied by this caller.
    for (std::size_t i = 0; i < out.parameters.size(); ++i)
        binding.semantic_parameter_for_ir_parameter.push_back(i);
    lowerer_.binding_origins().header(out,binding);
    lowerer_.summary_bindings().emplace(name, std::move(binding));
    return out;
}

// The prologue of a begun function's body, then the body: the facts proven
// before it is lowered (the Int ranges of its locals; the initialization and
// bounds of its reference array parameters, loaded at entry) and the shape
// constraints of the signature. Shared by functions, methods and
// constructors.
void FunctionLowering::lower_prologue_and_body(
    const FunctionDecl& source, const FunctionType& sig, std::size_t parameter_offset) {
    capture_signature_constraints(source,parameter_offset);
    const auto unit = checked_.signature_shapes.find(&source);
    split_shape_entry_ = unit != checked_.signature_shapes.end() &&
        std::any_of(unit->second.parts.begin(), unit->second.parts.end(),
                    [](const auto& part) { return part.parameter != 0; });
    for (const auto& parameter : source.parameters)
        split_shape_entry_ = split_shape_entry_ || !parameter.type.dimension_expressions.empty();
    if (split_shape_entry_) {
        std::unordered_set<std::string> captured;
        const auto record = [&](const CapturedExtents* extents) {
            if (!extents) return;
            for (const auto& extent : *extents)
                if (extent && captured.insert(*extent).second)
                    shape_captures_.push_back(Parameter{*extent, Type::simple(TypeKind::Int64)});
        };
        record(&shapes_.return_tensor_extents());
        record(&shapes_.return_array_extents());
        for (std::size_t p = parameter_offset; p < builder_.function()->parameters.size(); ++p) {
            const auto& name = builder_.function()->parameters[p].name;
            record(shapes_.tensor_extents(name));
            record(shapes_.array_extents(name));
        }
        if (unit != checked_.signature_shapes.end())
            for (const auto& variable : checked_.shape_plans.at(unit->second.source.get()).variables)
                if (variable.read) {
                    const auto& name = scope_.source_local(variable.name);
                    if (captured.insert(name).second)
                        shape_captures_.push_back(Parameter{name, scope_.local_type_at(name)});
                }
        shape_entry_block_ = builder_.function()->blocks.size() - 1;
        shape_entry_offset_ = builder_.block()->instructions.size();
    }
    if (enclosing_class_.in_constructor()) {
    const auto self=Type::class_type(enclosing_class_.name());
    scope_.bind_parameter("$receiver",self);
    lowerer_.binding_origins().local(*builder_.function(),"$receiver","this");
    builder_.emit(DeclareLocal{"$receiver",self,"$receiver",
        static_cast<std::uint32_t>(source.span.start.line),
        static_cast<std::uint32_t>(source.span.start.column)});
    auto made=calls_.make_class_with_defaults(enclosing_class_.name());
    // Borrowed: the local does not own the value, so function exit does not
    // release what the caller now holds.
    builder_.emit(StoreLocal{"$receiver",made,self,true});
    }
    facts_.integer_ranges.prepare(source.body);
    facts_.array_initialization.cache_reference_initialization(builder_, sig, source.body);
    facts_.array_bounds.cache_reference_bounds(builder_, sig, source.body);
    lowerer_.lower_block(source.body);

}

void FunctionLowering::lower_function(const FunctionDecl& source){
    const auto& sig=checked_.functions.at(source.name);
    if(sig.result.kind==TypeKind::Traversal)return;
    auto out=function_header(source.name, source, sig, false);
    out.external_symbol=source.external_symbol;
    if(out.external_symbol){module_.functions.push_back(std::move(out));return;}
    if(source.foreign_export) out.c_export_symbol=source.foreign_export->symbol;
    enclosing_class_.leave();begin_function(std::move(out), checked_.signature_shapes.contains(&source));
    lower_prologue_and_body(source, sig, 0);
    if(!current_block_ended(builder_)&&builder_.function()->result.kind==TypeKind::Void)builder_.emit(ReturnVoid{});
    builder_.finish();
    finish_shape_entry();
}

void FunctionLowering::lower_method(const std::string& class_name,const FunctionDecl& source){
    const auto internal=member_function_name::method(class_name,source.name);
    const auto& sig=checked_.functions.at(internal);
    if(sig.result.kind==TypeKind::Traversal)return;
    auto out=function_header(internal, source, sig, true);
    enclosing_class_.enter(class_name,checked_.classes.at(class_name).standard_library);begin_function(std::move(out), checked_.signature_shapes.contains(&source));
    lower_prologue_and_body(source, sig, 1);
    if(!current_block_ended(builder_)&&builder_.function()->result.kind==TypeKind::Void)builder_.emit(ReturnVoid{});
    builder_.finish();
    finish_shape_entry();
    enclosing_class_.leave();
}

void FunctionLowering::finish_shape_entry() {
    if (!split_shape_entry_) return;
    auto checked = *builder_.function();
    auto wrapper = checked;
    checked.name += ".shape_checked";
    checked.source_function = wrapper.name;
    checked.c_export_symbol.reset();
    checked.blocks.erase(checked.blocks.begin(), checked.blocks.begin() + shape_entry_block_);
    checked.blocks.front().instructions.erase(checked.blocks.front().instructions.begin(),
        checked.blocks.front().instructions.begin() + shape_entry_offset_);
    checked.parameters.insert(checked.parameters.end(), shape_captures_.begin(), shape_captures_.end());
    // The generated entry inherits this exact checked declaration, while
    // captured extents are synthetic parameters, not checker parameters.
    if (const auto origin = lowerer_.summary_bindings().find(wrapper.name);
        origin != lowerer_.summary_bindings().end()) {
        auto binding = origin->second;
        binding.semantic_parameter_for_ir_parameter.resize(checked.parameters.size());
        lowerer_.summary_bindings().emplace(checked.name, std::move(binding));
    }
    wrapper.blocks.resize(shape_entry_block_ + 1);
    wrapper.blocks.back().instructions.resize(shape_entry_offset_);
    wrapper.shape_wrapper = true;
    FunctionBuilder forward(wrapper, builder_.fresh(), 0, 0);
    forward.enter(wrapper.blocks.back());
    std::vector<CallArgument> arguments;
    for (const auto& parameter : wrapper.parameters) {
        const auto value = forward.fresh();
        if (parameter.writable) {
            forward.emit(AddressLocal{value, parameter.name});
            arguments.push_back(CallArgument{0, value});
        } else {
            forward.emit(LoadLocal{value, parameter.name, parameter.type,
                !parameter.borrowed && owns_managed(parameter.type)});
            arguments.push_back(CallArgument{value, std::nullopt});
        }
    }
    for (const auto& capture : shape_captures_) {
        const auto value = forward.fresh();
        forward.emit(LoadLocal{value, capture.name, capture.type,
            owns_managed(capture.type)});
        arguments.push_back(CallArgument{value, std::nullopt});
    }
    const auto result = checked.result.kind == TypeKind::Never ? ValueId{} : forward.fresh();
    forward.emit(Call{result, checked.name, std::move(arguments), checked.result,
        {wrapper.source_line, wrapper.source_column}, checked.result.kind == TypeKind::Never, true});
    if (checked.result.kind == TypeKind::Void) forward.emit(ReturnVoid{});
    else if (checked.result.kind != TypeKind::Never) forward.emit(Return{result, checked.result});
    forward.finish();
    lowerer_.binding_origins().split_shape_entry(wrapper.name,checked.name,shape_entry_block_,shape_entry_offset_);
    *builder_.function() = std::move(wrapper);
    module_.functions.push_back(std::move(checked));
    split_shape_entry_ = false;
}

// The receiver of a constructor is a local, not a parameter: the body
// starts from the class's defaults, initializes fields, and the value is
// returned (moved, never copied) when the body completes.
void FunctionLowering::return_receiver(){
    auto receiver=load_receiver(builder_,enclosing_class_);
    const auto self=Type::class_type(enclosing_class_.name());
    auto v=conversions_.convert(receiver,self,builder_.function()->result);
    builder_.emit(Return{v,builder_.function()->result});
}

void FunctionLowering::lower_constructor(const std::string& class_name,const FunctionDecl& source,const std::string& internal){
    const auto& sig=checked_.functions.at(internal);
    auto out=function_header(internal, source, sig, false);
    enclosing_class_.enter(class_name,checked_.classes.at(class_name).standard_library);begin_function(std::move(out), checked_.signature_shapes.contains(&source));
    enclosing_class_.set_in_constructor(true);
    lower_prologue_and_body(source, sig, 0);
    if(!current_block_ended(builder_)) return_receiver();
    builder_.finish();
    finish_shape_entry();
    enclosing_class_.set_in_constructor(false);
    enclosing_class_.leave();
}

// Before the first statement (and after package providers registered, at
// load time): the value of every contextual constant at every fixed-width
// type the program uses it at, certified once and kept in its slot.
void FunctionLowering::materialize_literal_constants() {
    std::vector<bool> used(checked_.literal_constant_slot_list.size());
    for (const auto& [use, slot] : checked_.literal_constant_slots) used[slot] = true;
    for (std::size_t slot = 0; slot < used.size(); ++slot) {
        if (!used[slot]) continue;
        const auto& entry = checked_.literal_constant_slot_list[slot];
        const auto value = lowerer_.lower(*entry.value);
        const auto out = builder_.fresh();
        builder_.emit(ExactRound{out, value, entry.type, literal_constant_slot_name(checked_, slot),
                                 {static_cast<std::uint32_t>(entry.first_use.start.line),
                                 static_cast<std::uint32_t>(entry.first_use.start.column)}});
        lowerer_.release_temporary(*entry.value, value);
    }
}

void FunctionLowering::lower_main(const std::vector<StmtPtr>& statements) {
    Function out;
    out.name = "$entry";
    out.source_file = checked_.program.root_source_file;
    if (!statements.empty()) {
        out.source_line = static_cast<std::uint32_t>(statements.front()->span.start.line);
        out.source_column = static_cast<std::uint32_t>(statements.front()->span.start.column);
    }
    out.result = Type::simple(TypeKind::Int64);
    out.entrypoint = true;
    enclosing_class_.leave();
    begin_function(std::move(out));
    facts_.integer_ranges.prepare(statements);
    materialize_literal_constants();

    bool replaying = replay_.replay_prefix_offset() != 0;
    if (!replaying) {
        lowerer_.lower_block(statements);
    } else {
        builder_.emit(ReplReplayMode{true});
        for (const auto& statement : statements) {
            if (replaying &&
                statement->span.start.offset >= replay_.replay_prefix_offset()) {
                builder_.emit(ReplReplayMode{false});
                replaying = false;
            }
            replay_.set_replayed_top_level_statement(replaying ? statement.get() : nullptr);
            lowerer_.lower_statement(*statement);
            replay_.set_replayed_top_level_statement(nullptr);
            if (current_block_ended(builder_)) break;
        }
    }
    if (!current_block_ended(builder_)) {
        if (replaying) builder_.emit(ReplReplayMode{false});
        auto zero = builder_.const_int(0);
        builder_.emit(Return{zero, Type::simple(TypeKind::Int64)});
    }
    builder_.finish();
}

} // namespace quidra::lowering
