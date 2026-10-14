#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "lowering/assignment_order.hpp"
#include <functional>
#include <stdexcept>

namespace quidra::lowering {
void ControlFlowLowering::lower_traversal_for(const ForStmt& n) {
    const auto* outer_call=std::get_if<CallExpr>(&n.iterable->data);
    const auto* outer_method=std::get_if<MethodCallExpr>(&n.iterable->data);
    const auto root_binding=checked_.traversal_bindings.find(n.iterable.get());
    const auto& guard_source=root_binding!=checked_.traversal_bindings.end()?*root_binding->second.root:outer_method?*outer_method->receiver:
        outer_call&&!outer_call->args.empty()?*outer_call->args.front().value:*n.iterable;
    const auto& plan=checked_.traversal_plans.at(n.iterable.get());
    const auto integer=Type::simple(TypeKind::Int64),natural=Type::simple(TypeKind::Nat);
    const auto boolean=Type::simple(TypeKind::Bool);
    std::vector<std::pair<ValueId,Type>> argument_values;
    std::vector<std::string> argument_references;
    std::vector<ValueId> argument_pins;
    std::vector<const Expr*> operands;
    const Expr* primitive=nullptr;
    ValueId root_address=0,root=0;
    std::function<void(const Expr&)> evaluate;
    evaluate=[&](const Expr& expression) {
        const auto* call=std::get_if<CallExpr>(&expression.data);
        const auto* method=std::get_if<MethodCallExpr>(&expression.data);
        if(call&&call->callee=="$std.reflect.each") {
            primitive=&expression;
            const auto& source=*call->args.front().value;
            if(n.writable) {
                auto captured=AssignmentOrder::target_subexpressions(source);
                for(const auto* operand:captured){lowerer_.capture_loop_operand(*operand);operands.push_back(operand);}
                root_address=lowerer_.lower_address(source);root=builder_.fresh();
                builder_.emit(LoadAddress{root,root_address,plan.root});
            }else root=lowerer_.lower(source);
            return;
        }
        const auto resolved_method=checked_.method_calls.find(&expression);
        const auto target=resolved_method!=checked_.method_calls.end()?resolved_method->second.internal_name:checked_.call_resolutions.at(&expression).target;
        const auto& function=*checked_.traversal_functions.at(target);
        const auto& signature=checked_.functions.at(target);
        const auto& arguments=method?method->args:call->args;
        const auto previous_class=lowerer_.enclosing_class_;
        auto before=scope_.snapshot_full();
        struct Argument {std::size_t parameter;ValueId value;bool reference;std::string storage;};
        std::vector<Argument> evaluated;
        std::vector<bool> filled(signature.parameters.size(),false);
        const bool member=!signature.parameters.empty()&&signature.parameters.front().name=="$receiver";
        std::size_t positional=member?1:0;
        if(member) {
            const auto receiver=method?lowerer_.lower(*method->receiver):load_receiver(builder_,lowerer_.enclosing_class_);
            if(method&&lifetime_.expression_owns_result(*method->receiver)) {
                argument_values.emplace_back(receiver,signature.parameters.front().type);
                lifetime_.enter_loop_value(receiver,signature.parameters.front().type);
            }
            builder_.emit(Pin{receiver});argument_pins.push_back(receiver);lifetime_.enter_loop_pin(receiver);
            evaluated.push_back({0,receiver,false,{}});filled[0]=true;
        }
        for(const auto& argument:arguments) {
            auto index=positional++;
            if(argument.name) {
                const auto found=std::find_if(signature.parameters.begin(),signature.parameters.end(),
                    [&](const auto& parameter){return parameter.name==*argument.name;});
                index=static_cast<std::size_t>(found-signature.parameters.begin());
            }
            const auto& parameter=signature.parameters.at(index);filled[index]=true;
            auto value=parameter.writable?lowerer_.lower_address(*argument.value):lowerer_.lower(*argument.value);
            if(!parameter.writable) {
                if(!lifetime_.expression_owns_result(*argument.value))value=lifetime_.copy_value(value,parameter.type);
                argument_values.emplace_back(value,parameter.type);lifetime_.enter_loop_value(value,parameter.type);
            }
            std::string storage;
            if(parameter.writable) {
                storage=builder_.hidden("traversal.argument.reference");
                builder_.emit(DeclareReference{storage,parameter.type});builder_.emit(BindReference{storage,value});
                argument_references.push_back(storage);lifetime_.enter_loop_reference(storage);
            }
            evaluated.push_back({index,value,parameter.writable,std::move(storage)});
        }
        for(const auto& argument:evaluated) {
            const auto& parameter=signature.parameters.at(argument.parameter);
            if(argument.reference) {
                scope_.bind_capture(parameter.name,argument.storage,parameter.type,true);
            }else {
                const auto storage=scope_.bind_source_local(builder_,parameter.name,parameter.type);
                builder_.emit(StoreLocal{storage,argument.value,parameter.type,true});
            }
        }
        for(std::size_t i=0;i<filled.size();++i)if(!filled[i]) {
            const auto& parameter=signature.parameters[i];
            if(!parameter.default_value)throw std::logic_error("checked traversal argument lacks a default");
            const auto value=lowerer_.lower(*parameter.default_value);
            const auto owned=lifetime_.expression_owns_result(*parameter.default_value)?value:lifetime_.copy_value(value,parameter.type);
            const auto storage=scope_.bind_source_local(builder_,parameter.name,parameter.type);
            builder_.emit(StoreLocal{storage,owned,parameter.type,true});
            argument_values.emplace_back(owned,parameter.type);lifetime_.enter_loop_value(owned,parameter.type);
        }
        if(member) {
            const auto& receiver=signature.parameters.front().type;
            lowerer_.enclosing_class_.enter(receiver.class_name,checked_.classes.at(receiver.class_name).standard_library);
            lowerer_.enclosing_class_.set_shape_receiver(scope_.source_local("$receiver"),false);
        }
        evaluate(*std::get<ReturnStmt>(function.body.front()->data).value);
        lowerer_.enclosing_class_=previous_class;
        scope_.restore(std::move(before));
    };
    evaluate(*n.iterable);
    const auto& source=*std::get<CallExpr>(primitive->data).args.front().value;
    const bool direct=primitive==n.iterable.get();
    const bool copied=direct&&!n.writable&&!lifetime_.expression_owns_result(source)&&
        std::any_of(n.body.begin(),n.body.end(),[&](const StmtPtr& statement){
            return storage_writes_.statement_writes_iterable(*statement,guard_source)!=StorageWrites::IterableWrite::none;
        });
    if(copied) root=lifetime_.copy_value(root,plan.root);
    const bool owned=!n.writable&&(copied||lifetime_.expression_owns_result(source));
    if(owned) lifetime_.enter_loop_value(root,plan.root);
    // A stored reference carries a serial for provenance, but its actual
    // target address is what managed_pin protects. Preserve that address
    // from entry through every visit, including a suffix below the field.
    const bool pinned_root=n.writable&&(!direct||storage_writes_.reaches_reference_storage(source));
    if(pinned_root){builder_.emit(Pin{root_address});lifetime_.enter_loop_pin(root_address);}
    const auto name=(n.discards()||n.pattern)?std::string{}:n.writable?
        scope_.bind_source_reference(builder_,n.name,plan.element):
        n.declares?scope_.bind_source_local(builder_,n.name,plan.element):scope_.source_local(n.name);
    if(n.writable&&!n.discards()&&!n.pattern) builder_.emit(DeclareReference{name,plan.element});
    const auto counter=builder_.hidden("traversal.index");scope_.local_type(counter)=natural;
    builder_.emit(StoreLocal{counter,lowerer_.bare_from_int64(builder_.const_int(0),natural,false),natural});
    const auto done=builder_.label("traversal.end");
    struct Step {TraversalNode::Kind kind;Type parent,type;std::size_t field;ValueId index;};
    using Path=std::vector<Step>;
    const auto resolve=[&](const Path& path)->std::pair<ValueId,ValueId> {
        auto value=root,address=root_address;
        if(n.writable){value=builder_.fresh();builder_.emit(LoadAddress{value,root_address,plan.root});}
        for(const auto& step:path) {
            const auto parent_address=address;
            address=builder_.fresh();
            if(step.kind==TraversalNode::Kind::Field) builder_.emit(AddressField{address,value,step.field});
            else if(step.kind==TraversalNode::Kind::TupleElement)
                builder_.emit(AddressTupleElement{address,parent_address,step.field,step.parent});
            else builder_.emit(AddressElement{address,value,step.index,step.parent,step.type,false,{0,0},true});
            const auto next=builder_.fresh();builder_.emit(LoadAddress{next,address,step.type});value=next;
        }
        return {value,address};
    };
    struct Guard {Path path;ValueId array,length;};
    std::vector<Guard> guards;
    std::function<void(const TraversalNode&,ValueId,ValueId,Path)> walk;
    walk=[&](const TraversalNode& node,ValueId value,ValueId address,Path path) {
        if(node.kind==TraversalNode::Kind::Visit) {
            const auto next=builder_.label("traversal.next"),leave=builder_.label("traversal.break");
            if(n.pattern)lower_control_pattern(*n.pattern,n.writable?address:value,n.writable);
            else if(!n.discards()) {
                if(n.writable) builder_.emit(BindReference{name,address});
                else {builder_.emit(StoreLocal{name,lifetime_.copy_value(value,plan.element),plan.element});lowerer_.initialization_flags().for_iteration(n);}
            }
            ValueId ordinal=0;
            if(n.index) {
                ordinal=builder_.fresh();builder_.emit(LoadLocal{ordinal,counter,natural});
                ordinal=lifetime_.copy_value(ordinal,natural);scope_.bind_source_value(n.index->name,ordinal);
                lifetime_.enter_loop_value(ordinal,natural);
            }
            std::vector<bool> checks;
            for(const auto& statement:n.body) checks.push_back(n.writable&&
                (lowerer_.options().check_every_reference_loop_statement||
                 storage_writes_.statement_may_reshape_iterable(*statement,guard_source)));
            const bool checked=std::any_of(checks.begin(),checks.end(),[](bool flag){return flag;});
            if(checked){builder_.emit(Pin{address});lifetime_.enter_loop_pin(address);}
            std::vector<std::string> references;
            if(n.writable&&n.pattern) {
                references.push_back(scope_.source_reference(checked_.control_patterns.at(&*n.pattern).subject));
                visit_pattern(*n.pattern,[&](const Pattern& leaf){if(leaf.kind==Pattern::Kind::Declare)references.push_back(scope_.source_reference(leaf.name));});
            }else if(n.writable&&!n.discards())references.push_back(name);
            for(const auto& reference:references)lifetime_.enter_loop_reference(reference);
            const auto check=[&](const Stmt& statement) {
                const auto line=static_cast<std::uint32_t>(statement.span.start.line),column=static_cast<std::uint32_t>(statement.span.start.column);
                for(const auto& guard:guards) {
                    const auto current=resolve(guard.path).first;
                    builder_.emit(IterationShapeCheck{current,guard.array,guard.length,{line,column}});
                }
                const auto current=resolve(path).second;
                builder_.emit(TraversalSiteCheck{address,current,{line,column}});
            };
            loop_targets_.enter(next,leave);
            for(std::size_t i=0;i<n.body.size();++i) {
                if(checks[i]) loop_targets_.set_before_jump([&,i]{check(*n.body[i]);});
                else loop_targets_.set_before_jump({});
                lowerer_.lower_statement(*n.body[i]);
                if(current_block_ended(builder_))break;
                if(checks[i])check(*n.body[i]);
            }
            loop_targets_.leave();
            for(std::size_t i=0;i<references.size();++i)lifetime_.leave_loop_reference();
            if(checked)lifetime_.leave_loop_pin();
            if(ordinal)lifetime_.leave_loop_value();
            if(!current_block_ended(builder_))builder_.emit(Jump{next});
            builder_.enter(builder_.add_block(leave));
            for(auto reference=references.rbegin();reference!=references.rend();++reference)builder_.emit(BindReference{*reference,0});
            if(checked)builder_.emit(Unpin{address});
            if(ordinal)release_owned(builder_,ordinal,natural);
            builder_.emit(Jump{done});
            builder_.enter(builder_.add_block(next));
            for(auto reference=references.rbegin();reference!=references.rend();++reference)builder_.emit(BindReference{*reference,0});
            if(checked)builder_.emit(Unpin{address});
            if(ordinal)release_owned(builder_,ordinal,natural);
            const auto index=builder_.fresh(),increment=builder_.fresh();builder_.emit(LoadLocal{index,counter,natural});
            builder_.emit(Binary{increment,"+",index,lowerer_.bare_from_int64(builder_.const_int(1),natural,false),natural,natural});
            builder_.emit(StoreLocal{counter,increment,natural});return;
        }
        if(node.kind==TraversalNode::Kind::Field||node.kind==TraversalNode::Kind::TupleElement) {
            const auto parent=path.empty()?plan.root:path.back().type;
            path.push_back({node.kind,parent,node.type,node.index,0});
            auto child=builder_.fresh(),child_address=ValueId{};
            if(n.writable) {
                child_address=builder_.fresh();
                if(node.kind==TraversalNode::Kind::Field)builder_.emit(AddressField{child_address,value,node.index});
                else builder_.emit(AddressTupleElement{child_address,address,node.index,parent});
                builder_.emit(LoadAddress{child,child_address,node.type});
            } else if(node.kind==TraversalNode::Kind::Field)builder_.emit(FieldGet{child,value,node.index,node.type});
            else builder_.emit(TupleGet{child,value,node.index,node.type});
            walk(node.children.front(),child,child_address,std::move(path));return;
        }
        if(node.kind==TraversalNode::Kind::Array) {
            auto length=builder_.fresh();builder_.emit(ArrayLength{length,value});
            const auto index_slot=builder_.hidden("traversal.array.index");scope_.local_type(index_slot)=integer;
            builder_.emit(StoreLocal{index_slot,builder_.const_int(0),integer});
            const auto cond=builder_.label("traversal.array.cond"),body=builder_.label("traversal.array.body"),end=builder_.label("traversal.array.end");
            builder_.emit(Jump{cond});builder_.enter(builder_.add_block(cond));
            auto index=builder_.fresh(),more=builder_.fresh();builder_.emit(LoadLocal{index,index_slot,integer});
            builder_.emit(Binary{more,"<",index,length,integer,boolean});builder_.emit(Branch{more,body,end});
            builder_.enter(builder_.add_block(body));
            if(n.writable)guards.push_back({path,value,length});
            path.push_back({TraversalNode::Kind::Array,node.type,*node.type.first,0,index});
            auto child=builder_.fresh(),child_address=ValueId{};
            if(n.writable) {
                child_address=builder_.fresh();builder_.emit(AddressElement{child_address,value,index,node.type,*node.type.first,false,{0,0},true});
                builder_.emit(LoadAddress{child,child_address,*node.type.first});
            }else builder_.emit(ArrayGet{child,value,index,*node.type.first,{0,0},true,true});
            walk(node.children.front(),child,child_address,std::move(path));
            if(n.writable)guards.pop_back();
            auto increment=builder_.fresh();builder_.emit(Binary{increment,"+",index,builder_.const_int(1),integer,integer,0,0,true});
            builder_.emit(StoreLocal{index_slot,increment,integer});builder_.emit(Jump{cond});
            builder_.enter(builder_.add_block(end));return;
        }
        for(const auto& child:node.children) {
            auto current=std::pair{value,address};
            if(n.writable)current=resolve(path);
            walk(child,current.first,current.second,path);
        }
    };
    walk(plan.tree,root,root_address,{});
    if(!current_block_ended(builder_)) builder_.emit(Jump{done});
    builder_.enter(builder_.add_block(done));
    if(pinned_root){lifetime_.leave_loop_pin();builder_.emit(Unpin{root_address});}
    if(owned){lifetime_.leave_loop_value();release_owned(builder_,root,plan.root);}
    for(auto it=argument_pins.rbegin();it!=argument_pins.rend();++it){lifetime_.leave_loop_pin();builder_.emit(Unpin{*it});}
    for(auto it=argument_references.rbegin();it!=argument_references.rend();++it) {
        lifetime_.leave_loop_reference();builder_.emit(BindReference{*it,0});
    }
    for(auto it=argument_values.rbegin();it!=argument_values.rend();++it) {
        lifetime_.leave_loop_value();release_owned(builder_,it->first,it->second);
    }
    for(const auto* operand:operands)lowerer_.forget_loop_operand(*operand);
}
}
