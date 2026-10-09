// Statement lowering: lower_block lowers every body (of a function, method,
// constructor, the entry and every loop) one statement at a time, trying the
// text sequences first (text_idioms.cpp), under the statement nesting
// budget; lower_statement emits a statement's SourceLocation and dispatches
// on its kind: bindings, rebinds, loop control, returns, expression
// statements and the main guard here, assignments in
// assignment_lowering.cpp, if, while, for and match in
// control_flow_lowering.cpp.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "quidra/language.hpp"
#include <algorithm>
#include <stdexcept>

namespace quidra::lowering {

void StatementLowering::lower_block(
    const std::vector<StmtPtr>& statements,
    const std::function<void(std::size_t)>& before_statement) {
    const auto guard = nesting_depth_.statement(
        statements.empty() ? SourceSpan{} : statements.front()->span);
    for (std::size_t i = 0; i < statements.size(); ++i) {
        if (before_statement) before_statement(i);
        if (const auto last = lowerer_.initialization_flags().active()
                                  ? std::optional<std::size_t>{}
                                  : text_idioms_.lower_text_sequence(statements, i)) {
            i = *last;
            if (current_block_ended(builder_)) break;
            continue;
        }
        lower_statement(*statements[i]);
        if (current_block_ended(builder_)) break;
    }
}

void StatementLowering::lower_statement(const Stmt& s) {
    const auto guard = nesting_depth_.statement(s.span);
    builder_.emit(locator_.locate(s.span, builder_.function()));
    lower_statement_kind(s);
    lowerer_.initialization_flags().after(s);
}

void StatementLowering::lower_statement_kind(const Stmt& s) {
    if(const auto* n=std::get_if<BindingStmt>(&s.data)) return lower_binding(s,*n);
    if(const auto* n=std::get_if<RebindStmt>(&s.data)) return lower_rebind(*n);
    if(const auto* n=std::get_if<AssignStmt>(&s.data)) return assignments_.lower_assignment(s,*n);
    if(const auto* n=std::get_if<LoopControlStmt>(&s.data)) return lower_loop_control(*n);
    if(const auto* n=std::get_if<ReturnStmt>(&s.data)) return lower_return(s,*n);
    if(const auto* n=std::get_if<ExprStmt>(&s.data)) return lower_expression_statement(s,*n);
    if(const auto* n=std::get_if<MainGuardStmt>(&s.data)) return lower_main_guard(*n);
    if(const auto* n=std::get_if<IfStmt>(&s.data)) return control_flow_.lower_if(*n);
    if(const auto* n=std::get_if<WhileStmt>(&s.data)) return control_flow_.lower_while(*n);
    if(const auto* n=std::get_if<ForStmt>(&s.data)) return control_flow_.lower_for(*n);
    control_flow_.lower_match(std::get<MatchStmt>(s.data));
}

void StatementLowering::lower_binding(const Stmt& s, const BindingStmt& n) {
    const auto t=checked_.binding_types.at(&s);
    if(n.reference){
        const auto ir_name=scope_.bind_source_reference(builder_,n.name,t);
        builder_.emit(DeclareReference{ir_name,t,n.is_const});
        auto address=lowerer_.lower_address(*n.value, !n.is_const);
        builder_.emit(BindReference{ir_name,address});
        return;
    }
    const auto ir_name=scope_.bind_source_local(builder_,n.name,t);
    builder_.emit(DeclareLocal{
        ir_name,t,n.name,
        static_cast<std::uint32_t>(s.span.start.line),
        static_cast<std::uint32_t>(s.span.start.column)});
    auto& initialization=lowerer_.initialization_flags();
    initialization.declare(s,n.name);

    if(t.kind==TypeKind::Tensor){
        auto captured=shape_constraints_.capture_extents(
            n.declared_type.tensor_shape_expressions,"shape.extent");
        if(!captured.empty()) shapes_.constrain_tensor(ir_name, captured);
        if(n.value && !captured.empty()){
            if(const auto* call=std::get_if<CallExpr>(&n.value->data)){
                const auto found=checked_.call_resolutions.find(n.value.get());
                const bool has_explicit_shape=std::any_of(
                    call->args.begin(),call->args.end(),[](const auto& argument){
                        return !argument.name || *argument.name!="gpu";
                    });
                if(!has_explicit_shape && found!=checked_.call_resolutions.end() &&
                   found->second.kind==CallKind::Builtin &&
                   (found->second.builtin==BuiltinCallable::TensorZeros ||
                    found->second.builtin==BuiltinCallable::TensorOnes)){
                    shapes_.set_initializer_shape(n.value.get(), captured);
                }
            }
        }
    }else if(t.kind==TypeKind::Array){
        auto captured=shape_constraints_.capture_extents(
            n.declared_type.dimension_expressions,"array.extent");
        if(!captured.empty()) shapes_.constrain_array(ir_name, captured);
    }

    if(!n.value && t.kind==TypeKind::Class &&
       class_storage_established_at_declaration(
           t.class_name,checked_.classes.at(t.class_name).standard_library)){
        auto v=calls_.make_class_with_defaults(t.class_name);
        builder_.emit(StoreLocal{ir_name,v,t});
        facts_.array_bounds.invalidate_length_relation(n.name);
        initialization.set(s);
        return;
    }
    if(n.value){
        const bool array_full =
            t.kind == TypeKind::Array &&
            facts_.array_initialization.fully_initialized(*n.value);
        auto v=lowerer_.lower_into(*n.value,t);
        if(const auto* found=shapes_.tensor_extents(ir_name)){
            shape_constraints_.emit_shaped_constraint(v,t.kind,*found,s.span);
        }
        if(const auto* found=shapes_.array_extents(ir_name)){
            shape_constraints_.emit_array_constraints(v,t,*found,0,s.span);
        }
        builder_.emit(StoreLocal{ir_name,v,t});
        facts_.record_local_store(n.name, t, *n.value, array_full);
        initialization.set(s);
    } else if(t.kind==TypeKind::Array) {
        const auto* captured=shapes_.array_extents(ir_name);
        if(t.length>=0 || (t.length==-2 && captured &&
                           !captured->empty() && (*captured)[0])){
            ValueId length{};
            if(t.length>=0){
                length=builder_.const_int(t.length);
            }else{
                length=builder_.fresh();
                builder_.emit(LoadLocal{
                    length,*(*captured)[0],Type::simple(TypeKind::Int64)});
            }
            auto storage=builder_.fresh();
            builder_.emit(ArrayAlloc{storage,length,t,false});
            builder_.emit(StoreLocal{ir_name,storage,t});
            facts_.array_initialization.forget_full(n.name);
            initialization.set(s);
        }
    }
}

void StatementLowering::lower_rebind(const RebindStmt& n) {
    auto address=lowerer_.lower_address(*n.target);
    builder_.emit(BindReference{scope_.source_reference(n.name),address});
}

void StatementLowering::lower_loop_control(const LoopControlStmt& n) {
    if(loop_targets_.empty()) throw std::logic_error("loop control escaped checker");
    loop_targets_.emit_before_jump();
    builder_.emit(Jump{n.is_continue?loop_targets_.continue_target():loop_targets_.break_target()});
}

void StatementLowering::lower_return(const Stmt& s, const ReturnStmt& n) {
    if(enclosing_class_.in_constructor()){
        if(std::holds_alternative<VoidExpr>(n.value->data)){
            lifetime_.release_loop_sources();
            functions_.return_receiver();
            return;
        }
        // An error return: the receiver never reaches a caller.
        auto receiver=load_receiver(builder_,enclosing_class_);
        builder_.emit(Release{receiver,Type::class_type(enclosing_class_.name())});
        auto v=lowerer_.lower_into(*n.value,builder_.function()->result);
        lifetime_.release_loop_sources();
        builder_.emit(Return{v,builder_.function()->result});
        return;
    }
    auto v=lowerer_.lower_into(*n.value,builder_.function()->result);
    if(!shapes_.return_tensor_extents().empty()){
        shape_constraints_.emit_shaped_constraint(
            v,builder_.function()->result.kind,shapes_.return_tensor_extents(),s.span);
    }
    if(!shapes_.return_array_extents().empty()){
        shape_constraints_.emit_array_constraints(
            v,builder_.function()->result,shapes_.return_array_extents(),0,s.span);
    }
    lifetime_.release_loop_sources();
    builder_.emit(Return{v,builder_.function()->result});
}

void StatementLowering::lower_expression_statement(const Stmt& s, const ExprStmt& n) {
    const auto residual_is_void=[&](){
        const auto& source=checked_.raw_types.at(n.value.get());
        if(source.kind!=TypeKind::Union) return false;
        for(const auto& current:source.cases)
            if(current.kind!=TypeKind::Error && current.kind!=TypeKind::Void) return false;
        return true;
    };
    // An accepted REPL submission that ends in an expression displays
    // its value, an error alternative included. Replaying that prefix
    // must discard the value the same way instead of failing on it; a
    // top-level statement that failed fast was never accepted.
    const bool displayed_result =
        n.value.get()==replay_.displayed_expression() || &s==replay_.replayed_top_level_statement();
    if((!displayed_result || residual_is_void()) &&
       checked_.fail_fast_expressions.contains(n.value.get())){
        // The statement discards its value, but an error is not a
        // value to discard: the program fails here.
        auto value=lowerer_.lower_at_raw_type(*n.value);
        const auto source=checked_.raw_types.at(n.value.get());
        conversions_.consume_fail_fast(value,source,Type::simple(TypeKind::Void),
                          lifetime_.expression_owns_result(*n.value),s.span);
        return;
    }
    auto value=lowerer_.lower(*n.value);
    const auto type=type_of(checked_,*n.value);
    if(n.value.get()==replay_.displayed_expression() && type.kind!=TypeKind::Void && type.kind!=TypeKind::Never){
        std::vector<std::string> initialized_paths;
        if (const auto it = checked_.class_expr_initialized_paths.find(n.value.get());
            it != checked_.class_expr_initialized_paths.end()) {
            initialized_paths.assign(it->second.begin(), it->second.end());
            std::sort(initialized_paths.begin(), initialized_paths.end());
        }
        builder_.emit(
            ReplDisplay{value,type,std::move(initialized_paths)});
    }
    lifetime_.release_temporary(*n.value,value);
}

void StatementLowering::lower_main_guard(const MainGuardStmt& n) {
    if(!n.active) return;
    auto before=scope_.snapshot_full();
    const auto before_full=facts_.array_initialization.full_arrays();
    for(const auto& x:n.body){ lower_statement(*x); if(current_block_ended(builder_)) break; }
    if(current_block_ended(builder_)) return;
    scope_.restore(std::move(before));
    facts_.array_initialization.set_full_arrays(before_full);
}

} // namespace quidra::lowering
