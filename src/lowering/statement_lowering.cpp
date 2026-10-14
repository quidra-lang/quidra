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
    lowerer_.statement_sites().register_tree(builder_.function()->name, statements);
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
    builder_.emit(ir::StatementBoundary{
        lowerer_.statement_sites().identity(builder_.function()->name, s)});
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
    if(const auto* n=std::get_if<ParallelStmt>(&s.data)) return lowerer_.lower_parallel(s,*n);
    control_flow_.lower_match(std::get<MatchStmt>(s.data));
}

void StatementLowering::lower_binding(const Stmt& s, const BindingStmt& n) {
    // A contextual constant has no storage: each use is its own value.
    if(n.pattern) {
        const auto& expansion=checked_.pattern_expansions.at(&s);
        lowerer_.statement_sites().register_expansion(builder_.function()->name,
            lowerer_.statement_sites().current_identity(builder_.function()->name), expansion.bindings);
        bool move=false;ir::ValueId subject=0;
        if(n.reference){auto name=scope_.bind_source_reference(builder_,expansion.subject,expansion.type);builder_.emit(DeclareReference{name,expansion.type});auto address=lowerer_.lower_address(*n.value);builder_.emit(BindReference{name,address});}
        else {subject=lowerer_.lower_into(*n.value,expansion.type);scope_.bind_source_value(expansion.subject,subject);move=lifetime_.expression_owns_result(*n.value) || checked_.fail_fast_expressions.contains(n.value.get());if(move)lifetime_.enter_tuple_move(expansion.subject);}
        for(const auto& binding:expansion.bindings)lower_statement(*binding);
        if(move){
            lifetime_.leave_tuple_move();
            const auto drop=[&](const auto& self,ValueId value,const Type& type)->void {if(type.kind!=TypeKind::Tuple){release_owned(builder_,value,type);return;}for(std::size_t i=0;i<type.parameters.size();++i){const auto& child_type=type.parameters[i];auto child=builder_.fresh();builder_.emit(TupleGet{child,value,i,child_type,child_type.kind!=TypeKind::Tuple && owns_managed(child_type)});self(self,child,child_type);}};
            const auto discarded=[&](const auto& self,const Pattern& pattern,ValueId value,const Type& type)->void {if(pattern.kind==Pattern::Kind::Discard){drop(drop,value,type);return;}if(pattern.kind!=Pattern::Kind::Tuple)return;for(std::size_t i=0;i<pattern.elements.size();++i){const auto& child_pattern=pattern.elements[i];const auto& child_type=type.parameters[i];if(child_pattern.kind==Pattern::Kind::Declare)continue;auto child=builder_.fresh();builder_.emit(TupleGet{child,value,i,child_type,child_pattern.kind==Pattern::Kind::Discard && child_type.kind!=TypeKind::Tuple && owns_managed(child_type)});self(self,child_pattern,child,child_type);}};
            discarded(discarded,*n.pattern,subject,expansion.type);
        }
        else if(!n.reference)release_owned(builder_,subject,expansion.type);
        return;
    }
    if(n.literal_constant)return;
    const auto t=checked_.binding_types.at(&s);
    const bool hoisted=lowerer_.parallel_hoisted(s);
    if(n.reference){
        const auto ir_name=hoisted?scope_.source_reference(n.name):scope_.bind_source_reference(builder_,n.name,t);
        if(!hoisted) builder_.emit(DeclareReference{ir_name,t,n.is_const});
        const ShapePlan* plan = nullptr;
        std::optional<ShapeConstraintLowering::BraceShape> shape;
        if (t.kind == TypeKind::Tensor && n.declared_type.brace_shape) {
            if (const auto found = checked_.shape_plans.find(&n.declared_type);
                found != checked_.shape_plans.end()) {
                plan = &found->second;
                shape = shape_constraints_.capture_brace_shape(*plan, s.span);
            }
        }
        auto address=n.value ? lowerer_.lower_address(*n.value, !n.is_const) : 0;
        builder_.emit(BindReference{ir_name,address});
        if (shape && n.value) {
            const auto value = builder_.fresh();
            builder_.emit(LoadReference{value, ir_name, t});
            shape_constraints_.finish_brace_shape(*plan, *shape, value, t.kind, n.name, s.span);
            shapes_.constrain_tensor(ir_name, shape->extents);
        } else if (t.kind == TypeKind::Tensor && n.value) {
            if (const auto* source = std::get_if<NameExpr>(&n.value->data)) {
                const auto source_name = scope_.is_source_reference(source->name)
                    ? scope_.source_reference(source->name) : scope_.source_local(source->name);
                if (const auto* captured = shapes_.tensor_extents(source_name))
                    shapes_.constrain_tensor(ir_name, *captured);
            }
        }
        return;
    }
    const auto ir_name=hoisted?scope_.source_local(n.name):scope_.bind_source_local(builder_,n.name,t);
    if(!hoisted) builder_.emit(DeclareLocal{
        ir_name,t,n.name,
        static_cast<std::uint32_t>(s.span.start.line),
        static_cast<std::uint32_t>(s.span.start.column)});
    auto& initialization=lowerer_.initialization_flags();
    if(!hoisted) initialization.declare(s,n.name);

    // A brace shape (L17): its expressions before the initializer, its
    // variables and checks after it.
    const ShapePlan* brace_plan=nullptr;
    std::optional<ShapeConstraintLowering::BraceShape> brace_shape;
    if(t.kind==TypeKind::Tensor && n.declared_type.brace_shape){
        if(const auto found=checked_.shape_plans.find(&n.declared_type);
           found!=checked_.shape_plans.end()) brace_plan=&found->second;
    }
    if(brace_plan){
        brace_shape=shape_constraints_.capture_brace_shape(*brace_plan,s.span,true,
            hoisted?lowerer_.parallel_brace_shape(s):nullptr);
        const auto& captured=brace_shape->extents;
        const bool every_axis=std::all_of(captured.begin(),captured.end(),
                                          [](const auto& extent){ return extent.has_value(); });
        if(n.value && every_axis){
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
    }else if(t.kind==TypeKind::Tensor){
        auto captured=hoisted && shapes_.tensor_extents(ir_name)
            ? *shapes_.tensor_extents(ir_name) : shape_constraints_.capture_extents(
            n.declared_type.tensor_shape_expressions,"shape.extent");
        if(hoisted) shape_constraints_.initialize_captured_extents(n.declared_type.tensor_shape_expressions,captured);
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
        auto captured=hoisted && shapes_.array_extents(ir_name)
            ? *shapes_.array_extents(ir_name) : shape_constraints_.capture_extents(
            n.declared_type.dimension_expressions,"array.extent");
        if(hoisted) shape_constraints_.initialize_captured_extents(n.declared_type.dimension_expressions,captured);
        if(!captured.empty()) shapes_.constrain_array(ir_name, captured);
    }

    if(!n.value && t.kind==TypeKind::Class &&
       class_storage_established_at_declaration(
           t.class_name,checked_.classes.at(t.class_name).standard_library)){
        auto v=calls_.make_class_with_defaults(t.class_name, true);
        builder_.emit(StoreLocal{ir_name,v,t});
        facts_.array_bounds.invalidate_length_relation(n.name);
        initialization.set(s);
        return;
    }
    if(brace_plan && !n.value){
        shape_constraints_.finish_brace_shape(*brace_plan,*brace_shape,std::nullopt,t.kind,n.name,s.span);
        shapes_.constrain_tensor(ir_name, brace_shape->extents);
    }
    if(n.value){
        const bool array_full =
            t.kind == TypeKind::Array &&
            facts_.array_initialization.fully_initialized(*n.value);
        auto v=lowerer_.lower_into(*n.value,t);
        if(brace_plan){
            shape_constraints_.finish_brace_shape(*brace_plan,*brace_shape,v,t.kind,n.name,s.span,n.value.get());
            shapes_.constrain_tensor(ir_name, brace_shape->extents);
        }
        if(const auto* found=brace_plan ? nullptr : shapes_.tensor_extents(ir_name)){
            shape_constraints_.emit_shaped_constraint(v,t.kind,*found,s.span,n.value.get());
        }
        if(const auto* found=shapes_.array_extents(ir_name)){
            shape_constraints_.emit_array_constraints(v,t,*found,0,s.span,n.value.get());
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
    if (n.place) {
        const auto& field=checked_.field_accesses.at(n.place.get());
        auto object=load_receiver(builder_,enclosing_class_);
        auto address=lowerer_.lower_address(*n.target);
        if(const auto checks=checked_.stored_target_initialization_checks.find(n.target.get());checks!=checked_.stored_target_initialization_checks.end()) {
            auto value=builder_.fresh();builder_.emit(LoadAddress{value,address,field.type});
            const auto word=Type::simple(TypeKind::Nat64);
            for(const auto& path:checks->second) {
                auto parent=value;auto type=field.type;
                for(std::size_t depth=0;depth<path.size();++depth) {
                    const auto& info=checked_.classes.at(type.class_name);
                    const auto index=path[depth];
                    if(depth+1==path.size()) {
                        auto mask=builder_.fresh();builder_.emit(FieldGet{mask,parent,info.fields.size(),word});
                        auto bit=builder_.fresh();builder_.emit(ConstantInt{bit,std::to_string(std::uint64_t{1}<<index),word});
                        auto selected=builder_.fresh();builder_.emit(Binary{selected,"AND",mask,bit,word,word});
                        auto zero=builder_.fresh();builder_.emit(ConstantInt{zero,"0",word});
                        auto set=builder_.fresh();builder_.emit(Binary{set,"!=",selected,zero,word,Type::simple(TypeKind::Bool)});
                        builder_.emit(InitializedCheck{set,InitializedSubject::field,"reference target",{static_cast<std::uint32_t>(n.target->span.start.line),static_cast<std::uint32_t>(n.target->span.start.column)}});
                    } else {auto child=builder_.fresh();const auto& child_type=info.fields[index].type;builder_.emit(FieldGet{child,parent,index,child_type});parent=child;type=child_type;}
                }
            }
        }
        builder_.emit(BindFieldReference{object,field.index,address,0});
        return;
    }
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
        auto v=lowerer_.lower_into(*n.value,builder_.function()->result);
        auto receiver=load_receiver(builder_,enclosing_class_);
        release_owned(builder_,receiver,Type::class_type(enclosing_class_.name()));
        lifetime_.release_loop_sources();
        builder_.emit(Return{v,builder_.function()->result});
        return;
    }
    auto v=lowerer_.lower_into(*n.value,builder_.function()->result);
    if(!shapes_.return_tensor_extents().empty()){
        shape_constraints_.emit_shaped_constraint(
            v,builder_.function()->result.kind,shapes_.return_tensor_extents(),s.span,n.value.get());
    }
    if(!shapes_.return_array_extents().empty()){
        shape_constraints_.emit_array_constraints(
            v,builder_.function()->result,shapes_.return_array_extents(),0,s.span,n.value.get());
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
    if(!displayed_result && checked_.raw_types.at(n.value.get()).kind==TypeKind::Error){
        auto problem=lowerer_.lower_at_raw_type(*n.value);
        if(lifetime_.expression_owns_result(*n.value)){
            if(const auto hop=lowerer_.error_hop_call(*n.value))
                problem=lowerer_.error_hop(problem,Type::simple(TypeKind::Error),
                                          *hop,abi::ErrorHopKind::fail_fast);
        }
        builder_.emit(FailError{
            problem,{static_cast<std::uint32_t>(s.span.start.line),
            static_cast<std::uint32_t>(s.span.start.column)}});
        return;
    }
    if((!displayed_result || residual_is_void()) &&
       checked_.fail_fast_expressions.contains(n.value.get())){
        // The statement discards its value, but an error is not a
        // value to discard: the program fails here.
        auto value=lowerer_.lower_at_raw_type(*n.value);
        const auto source=checked_.raw_types.at(n.value.get());
        const bool owned=lifetime_.expression_owns_result(*n.value);
        const auto hop=owned?lowerer_.error_hop_call(*n.value):std::nullopt;
        conversions_.consume_fail_fast(value,source,Type::simple(TypeKind::Void),
                          owned,s.span,hop?&*hop:nullptr);
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
    facts_.array_initialization.set_full_arrays(
        intersect_full_arrays(before_full, facts_.array_initialization.full_arrays()));
}

} // namespace quidra::lowering
