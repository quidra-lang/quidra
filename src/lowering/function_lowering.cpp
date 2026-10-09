// Function lowering: begins each function (its blocks, counters and
// facts), captures signature shape constraints, and lowers functions,
// methods, constructors and the entry, including the REPL replay of the
// accepted prefix.

#include "lowering/lowerer.hpp"
#include "quidra/member_function_names.hpp"
#include <stdexcept>

namespace quidra::lowering {

void FunctionLowering::capture_signature_constraints(
    const FunctionDecl& source, std::size_t parameter_offset) {
    for (std::size_t i = 0; i < source.parameters.size(); ++i) {
        const auto ir_index = i + parameter_offset;
        if (ir_index >= builder_.function()->parameters.size()) {
            throw std::logic_error("signature parameter offset mismatch");
        }
        const auto& parameter = builder_.function()->parameters[ir_index];
        const auto& syntax = source.parameters[i].type;

        if (parameter.type.kind == TypeKind::Tensor &&
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

    if (builder_.function()->result.kind == TypeKind::Tensor &&
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
void FunctionLowering::begin_function(Function out) {
    module_.functions.push_back(std::move(out));
    builder_=FunctionBuilder(module_.functions.back(),1,0,0);
    scope_.reset();
    facts_.reset();
    shapes_.reset();
    builder_.enter(builder_.add_block("entry"));
    for(const auto& p:builder_.function()->parameters){scope_.bind_parameter(p.name,p.type);}
    for(const auto& p:builder_.function()->parameters){
        if(p.type.kind!=TypeKind::Tensor||
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
            parameter,p.type.kind,std::move(extents),0,0});
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
    out.source_line=static_cast<std::uint32_t>(source.span.start.line);
    out.source_column=static_cast<std::uint32_t>(source.span.start.column);
    out.result=sig.result;
    for(std::size_t i=0;i<sig.parameters.size();++i){const auto& p=sig.parameters[i];out.parameters.push_back(Parameter{
        p.name, p.type, p.writable,
        (borrowed_receiver&&p.name=="$receiver")||borrows_.parameter_is_borrowed(name,i), p.is_const});}
    return out;
}

// The prologue of a begun function's body, then the body: the facts proven
// before it is lowered (the Int ranges of its locals; the initialization and
// bounds of its reference array parameters, loaded at entry) and the shape
// constraints of the signature. Shared by functions, methods and
// constructors.
void FunctionLowering::lower_prologue_and_body(
    const FunctionDecl& source, const FunctionType& sig, std::size_t parameter_offset) {
    facts_.integer_ranges.prepare(source.body);
    facts_.array_initialization.cache_reference_initialization(builder_, sig, source.body);
    facts_.array_bounds.cache_reference_bounds(builder_, sig, source.body);
    capture_signature_constraints(source,parameter_offset);
    lowerer_.lower_block(source.body);
}

void FunctionLowering::lower_function(const FunctionDecl& source){
    const auto& sig=checked_.functions.at(source.name);
    auto out=function_header(source.name, source, sig, false);
    out.external_symbol=source.external_symbol;
    if(out.external_symbol){module_.functions.push_back(std::move(out));return;}
    if(source.foreign_export) out.c_export_symbol=source.foreign_export->symbol;
    enclosing_class_.leave();begin_function(std::move(out));
    lower_prologue_and_body(source, sig, 0);
    if(!current_block_ended(builder_)&&builder_.function()->result.kind==TypeKind::Void)builder_.emit(ReturnVoid{});
    builder_.finish();
}

void FunctionLowering::lower_method(const std::string& class_name,const FunctionDecl& source){
    const auto internal=member_function_name::method(class_name,source.name);
    const auto& sig=checked_.functions.at(internal);
    auto out=function_header(internal, source, sig, true);
    enclosing_class_.enter(class_name,checked_.classes.at(class_name).standard_library);begin_function(std::move(out));
    lower_prologue_and_body(source, sig, 1);
    if(!current_block_ended(builder_)&&builder_.function()->result.kind==TypeKind::Void)builder_.emit(ReturnVoid{});
    builder_.finish();
    enclosing_class_.leave();
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
    enclosing_class_.enter(class_name,checked_.classes.at(class_name).standard_library);begin_function(std::move(out));
    const auto self=Type::class_type(class_name);
    scope_.bind_parameter("$receiver",self);
    builder_.emit(DeclareLocal{"$receiver",self,"$receiver",
        static_cast<std::uint32_t>(source.span.start.line),
        static_cast<std::uint32_t>(source.span.start.column)});
    auto made=calls_.make_class_with_defaults(class_name);
    // Borrowed: the local does not own the value, so function exit does not
    // release what the caller now holds.
    builder_.emit(StoreLocal{"$receiver",made,self,true});
    enclosing_class_.set_in_constructor(true);
    lower_prologue_and_body(source, sig, 0);
    if(!current_block_ended(builder_)) return_receiver();
    builder_.finish();
    enclosing_class_.set_in_constructor(false);
    enclosing_class_.leave();
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
