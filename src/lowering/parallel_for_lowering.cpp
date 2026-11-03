#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <algorithm>
#include <stdexcept>

namespace quidra::lowering {
void ConcurrencyLowering::lower_parallel_for(const ForStmt& loop) {
    auto& parent=lowerer_;
    const auto& plan=checked_.parallel_for_plans.at(&loop);
    const auto before=parent.scope_.snapshot_full();
    const auto integer=Type::simple(TypeKind::Int),raw=Type::simple(TypeKind::Int64);
    const auto word=Type::simple(TypeKind::Nat64),boolean=Type::simple(TypeKind::Bool);
    const auto result_type=Type::union_of({Type::simple(TypeKind::Void),Type::simple(TypeKind::Error)});
    struct Capture {std::string source,storage;Type type;bool reference{};};
    std::vector<Capture> captures;
    std::vector<LifetimeLowering::LoopSource> sources;
    ParallelBlock instruction;
    instruction.out=builder_.fresh();instruction.table_name=builder_.function()->name+"."+builder_.hidden("parallel.for.table");
    instruction.position.line=static_cast<std::uint32_t>(loop.parallel_span.start.line);
    instruction.position.column=static_cast<std::uint32_t>(loop.parallel_span.start.column);
    instruction.iterations.emplace();
    ParallelNode node;node.fallible=false;node.worker_safe=plan.worker_arithmetic;
    node.position.line=instruction.position.line;node.position.column=instruction.position.column;
    const auto capture=[&](std::string source,std::string storage,Type type,bool reference=false) {
        if(std::any_of(captures.begin(),captures.end(),[&](const auto& item){return item.storage==storage;}))return;
        captures.push_back({std::move(source),std::move(storage),std::move(type),reference});
    };
    const auto hold=[&](ValueId value,const Type& type,bool borrowed,const std::string& role) {
        const auto slot=builder_.hidden(role);parent.scope_.local_type(slot)=type;
        if(borrowed)value=parent.lifetime_.copy_value(value,type);
        builder_.emit(StoreLocal{slot,value,type});return slot;
    };
    const auto load=[&](const std::string& slot,const Type& type) {
        auto value=builder_.fresh();builder_.emit(LoadLocal{value,slot,type});return value;
    };
    const auto constant=[&](long long value) {
        auto out=builder_.fresh();builder_.emit(ConstantExact{out,std::to_string(value),integer});return out;
    };
    const auto convert=[&](ValueId value,const Type& from,const Type& to,bool proven=false) {
        if(from==to)return value;
        auto out=builder_.fresh();builder_.emit(NumericConvert{out,value,from,to,false,
            instruction.position,proven});return out;
    };
    const auto binary=[&](std::string operation,ValueId a,ValueId b,const Type& operand,const Type& result) {
        auto value=builder_.fresh();builder_.emit(Binary{value,std::move(operation),a,b,operand,result,
            instruction.position.line,instruction.position.column});
        if(owns_managed(result))return load(hold(value,result,false,"parallel.for.calculation"),result);
        return value;
    };
    auto worker_safe=builder_.const_bool(plan.worker_arithmetic);
    std::string first_slot,step_slot,fast_first_slot,fast_step_slot,fast_slot;
    struct Collection {std::string name,source;Type type;};
    std::vector<Collection> collections;
    if(loop.range) {
        const auto item=type_of(checked_,*loop.iterable);
        const auto operand=[&](const Expr& expression) {
            const auto type=type_of(checked_,expression);
            auto value=parent.lower(expression);
            value=load(hold(value,type,!parent.lifetime_.expression_owns_result(expression),"parallel.for.bound"),type);
            return convert(value,type,integer);
        };
        // Evaluate written operands once, in their source order, before any
        // scheduling or body storage is prepared.
        auto start=operand(*loop.iterable),end=operand(*loop.range->end);
        auto step=loop.range->step?operand(*loop.range->step):constant(loop.range->descending?-1:1);
        const auto step_origin=loop.range->step?loop.range->step->span.start:loop.parallel_span.start;
        builder_.emit(ForStepCheck{step,loop.range->descending,
            {static_cast<std::uint32_t>(step_origin.line),static_cast<std::uint32_t>(step_origin.column)}});
        step_slot=hold(step,integer,true,"parallel.for.step");
        auto first=loop.range->start_inclusive?start:binary("+",start,step,integer,integer);
        first_slot=hold(first,integer,true,"parallel.for.first");
        auto magnitude=loop.range->descending?binary("-",constant(0),step,integer,integer):step;
        auto stop=end;
        if(!loop.range->end_inclusive)
            stop=binary(loop.range->descending?"+":"-",end,constant(1),integer,integer);
        const auto nonempty=binary(loop.range->descending?">=":"<=",first,stop,integer,boolean);
        const auto count_slot=builder_.hidden("parallel.for.count");parent.scope_.local_type(count_slot)=raw;
        builder_.emit(StoreLocal{count_slot,builder_.const_int(0),raw});
        const auto prepare=builder_.label("parallel.for.nonempty"),ready=builder_.label("parallel.for.ready");
        builder_.emit(Branch{nonempty,prepare,ready});builder_.enter(builder_.add_block(prepare));
        auto distance=binary("-",loop.range->descending?first:stop,loop.range->descending?stop:first,integer,integer);
        auto quotient=binary("/",distance,magnitude,integer,integer);
        auto count=binary("+",quotient,constant(1),integer,integer);
        // A count, not the range value, crosses the scheduler's bounded ABI.
        // Too many logical iterations fail before any body executes.
        builder_.emit(StoreLocal{count_slot,convert(count,integer,raw),raw});
        builder_.emit(Jump{ready});builder_.enter(builder_.add_block(ready));
        instruction.iterations->count=load(count_slot,raw);
        auto fast=builder_.const_bool(true);
        // The exact slow mapping stays on the owner. Inline bounds admit a
        // fixed-word mapping whose intermediate distance fits signed i64.
        for(const auto value:{first,stop,step}) {
            auto inline_value=builder_.fresh();builder_.emit(Unary{inline_value,"int.is_inline",value,boolean});
            fast=binary("and",fast,inline_value,boolean,boolean);
        }
        fast_slot=hold(fast,boolean,false,"parallel.for.fast");
        fast_first_slot=builder_.hidden("parallel.for.first.word");fast_step_slot=builder_.hidden("parallel.for.step.word");
        parent.scope_.local_type(fast_first_slot)=parent.scope_.local_type(fast_step_slot)=raw;
        builder_.emit(StoreLocal{fast_first_slot,builder_.const_int(0),raw});
        builder_.emit(StoreLocal{fast_step_slot,builder_.const_int(0),raw});
        const auto words=builder_.label("parallel.for.words"),mapped=builder_.label("parallel.for.mapped");
        builder_.emit(Branch{fast,words,mapped});builder_.enter(builder_.add_block(words));
        builder_.emit(StoreLocal{fast_first_slot,convert(first,integer,raw,true),raw});
        builder_.emit(StoreLocal{fast_step_slot,convert(step,integer,raw,true),raw});
        builder_.emit(Jump{mapped});builder_.enter(builder_.add_block(mapped));
        for(const auto& pair:{std::pair{first_slot,integer},std::pair{step_slot,integer},
                             std::pair{fast_first_slot,raw},std::pair{fast_step_slot,raw},std::pair{fast_slot,boolean}})
            capture(pair.first,pair.first,pair.second);
        worker_safe=binary("and",worker_safe,fast,boolean,boolean);
        (void)item;
    } else {
        const auto snapshot=[&](const Expr& expression,const std::string& name) {
            const auto type=type_of(checked_,expression);
            const auto original=parent.lower(expression);
            auto copy=parent.lifetime_.copy_value(original,type);
            parent.lifetime_.release_temporary(expression,original);
            LifetimeLowering::LoopSource source{builder_.hidden("parallel.for.source"),builder_.hidden("parallel.for.owned"),type};
            parent.scope_.local_type(source.source)=type;parent.scope_.local_type(source.owned)=boolean;
            builder_.emit(StoreLocal{source.source,copy,type,true});
            builder_.emit(StoreLocal{source.owned,builder_.const_bool(true),boolean});
            parent.lifetime_.enter_loop_source(source);sources.push_back(source);
            collections.push_back({name,source.source,type});capture(source.source,source.source,type);
            auto length=builder_.fresh();builder_.emit(ArrayLength{length,copy});return length;
        };
        auto count=snapshot(*loop.iterable,loop.name);
        std::vector<ValueId> lengths;
        for(const auto& extra:loop.collections)lengths.push_back(snapshot(*extra.iterable,extra.name));
        for(const auto length:lengths)builder_.emit(ExtentEqualCheck{length,count,instruction.position,true});
        instruction.iterations->count=count;
    }
    // Empty ranges and collections finish after their common header checks;
    // they do not detach or validate output storage that no iteration writes.
    const auto dispatch=builder_.label("parallel.for.dispatch"),joined=builder_.label("parallel.for.joined");
    auto nonzero=builder_.fresh();builder_.emit(Binary{nonzero,"!=",instruction.iterations->count,
        builder_.const_int(0),raw,boolean,instruction.position.line,instruction.position.column});
    builder_.emit(Branch{nonzero,dispatch,joined});builder_.enter(builder_.add_block(dispatch));
    for(const auto& name:plan.captures) {
        const auto storage=before.local_names.at(name);const auto type=before.locals.at(storage);
        if(std::find(plan.outputs.begin(),plan.outputs.end(),name)!=plan.outputs.end()) {
            // Parent-only replacement prepares independent value storage.
            // No worker calls an ArrayValue COW path or replaces its root.
            auto old=load(storage,type);
            auto copy=parent.lifetime_.copy_value(old,type);
            builder_.emit(StoreLocal{storage,copy,type});
        } else if(is_bare_integer(type)) {
            auto inline_value=builder_.fresh();builder_.emit(Unary{inline_value,"int.is_inline",load(storage,type),boolean});
            worker_safe=binary("and",worker_safe,inline_value,boolean,boolean);
        }
        capture(name,storage,type);
        if(const auto binding=plan.capture_bindings.find(name);binding!=plan.capture_bindings.end())
            if(const auto* flag=parent.initialization_flags_.flag(binding->second))capture(*flag,*flag,boolean);
    }
    struct Receipt {std::string source,storage;Type type;ValueId array;bool read_only;};
    std::vector<Receipt> receipts;
    for(const auto& collection:collections)
        receipts.push_back({collection.name,builder_.hidden("parallel.for.receipt"),collection.type,load(collection.source,collection.type),true});
    for(const auto& name:plan.outputs) {
        const auto type=before.locals.at(before.local_names.at(name));
        receipts.push_back({name,builder_.hidden("parallel.for.receipt"),type,load(before.local_names.at(name),type),false});
    }
    for(const auto& receipt:receipts)capture(receipt.storage,receipt.storage,word,true);
    instruction.iterations->worker_safe=worker_safe;
    Lowerer child(checked_,nullptr,0,parent.options_);
    child.module_.classes=parent.module_.classes;child.module_.compiler_extensions=parent.module_.compiler_extensions;
    child.module_.user_sources=parent.module_.user_sources;child.enclosing_class_=parent.enclosing_class_;
    Function function;function.name="$par.for."+instruction.table_name;function.result=Type::simple(TypeKind::Void);
    function.source_file=builder_.function()->source_file;function.source_line=instruction.position.line;function.source_column=instruction.position.column;
    function.parallel_node=ParallelNodeMetadata{function.name+".entry",{},false};node.function=function.name;
    for(std::size_t i=0;i<captures.size();++i) {
        const auto& cap=captures[i];function.parameters.push_back({cap.storage,cap.type,true,true,false});
        if(cap.reference) {
            function.parallel_node->reference_slots.push_back(cap.storage);
            const auto receipt=std::find_if(receipts.begin(),receipts.end(),[&](const auto& item){return item.storage==cap.storage;});
            instruction.iterations->arrays.push_back({i,receipt->array,receipt->read_only,is_bare_integer(*receipt->type.first)});
            node.captures.push_back({0,ValueId{0}});
        } else {
            auto address=builder_.fresh();builder_.emit(AddressLocal{address,cap.storage});node.captures.push_back({0,address});
        }
    }
    const auto ordinal_name=builder_.hidden("parallel.for.ordinal");
    function.parameters.push_back({ordinal_name,word,true,true,false});
    child.functions_.begin_parallel_node(std::move(function));
    for(const auto& cap:captures)child.scope_.bind_capture(cap.source,cap.storage,cap.type,cap.reference);
    child.scope_.bind_capture(ordinal_name,ordinal_name,word,false);
    for(const auto& [name,binding]:plan.capture_bindings) {
        (void)name;
        if(const auto* flag=parent.initialization_flags_.flag(binding))child.initialization_flags_.adopt(binding,*flag);
    }
    auto ordinal=child.builder_.fresh();child.builder_.emit(LoadLocal{ordinal,ordinal_name,word});
    const auto child_load=[&](const std::string& slot,const Type& type) {auto value=child.builder_.fresh();child.builder_.emit(LoadLocal{value,slot,type});return value;};
    const auto child_convert=[&](ValueId value,const Type& from,const Type& to,bool proven=false) {if(from==to)return value;auto result=child.builder_.fresh();child.builder_.emit(NumericConvert{result,value,from,to,false,instruction.position,proven});return result;};
    const auto child_binary=[&](std::string operation,ValueId a,ValueId b,const Type& type,bool proven=false) {auto result=child.builder_.fresh();child.builder_.emit(Binary{result,std::move(operation),a,b,type,type,instruction.position.line,instruction.position.column,proven});return result;};
    if(loop.range && !loop.discards()) {
        const auto item=type_of(checked_,*loop.iterable);
        const auto visible=child.scope_.bind_source_local(child.builder_,loop.name,item);
        const auto fast=child_load(fast_slot,boolean);
        const auto words=child.builder_.label("parallel.for.inline"),exact=child.builder_.label("parallel.for.exact"),body=child.builder_.label("parallel.for.body");
        child.builder_.emit(Branch{fast,words,exact});child.builder_.enter(child.builder_.add_block(words));
        auto index=child_convert(ordinal,word,raw,true);
        auto offset=child_binary("*",index,child_load(fast_step_slot,raw),raw,true);
        auto value=child_binary("+",child_load(fast_first_slot,raw),offset,raw,true);
        child.builder_.emit(StoreLocal{visible,child_convert(value,raw,item),item});
        child.builder_.emit(Jump{body});child.builder_.enter(child.builder_.add_block(exact));
        offset=child_binary("*",child_convert(ordinal,word,integer),child_load(step_slot,integer),integer);
        value=child_binary("+",child_load(first_slot,integer),offset,integer);
        child.builder_.emit(StoreLocal{visible,child_convert(value,integer,item),item});
        child.builder_.emit(Jump{body});child.builder_.enter(child.builder_.add_block(body));
    } else if(!loop.range) {
        const auto index=child_convert(ordinal,word,raw,true);
        for(const auto& collection:collections)if(collection.name!="_") {
            const auto item=*collection.type.first;const auto visible=child.scope_.bind_source_local(child.builder_,collection.name,item);
            auto element=child.builder_.fresh();child.builder_.emit(ArrayGet{element,child_load(collection.source,collection.type),index,item,instruction.position,true,true});
            // Managed scalar copies stay on the owner unless every element
            // has the inline representation proved before scheduling.
            element=child.lifetime_.copy_value(element,item);
            child.builder_.emit(StoreLocal{visible,element,item});
        }
    }
    for(const auto& receipt:receipts)if(!receipt.read_only) {
        auto address=child.builder_.fresh();child.builder_.emit(ReferenceAddress{address,receipt.storage});
        child.parallel_for_outputs_[receipt.source]={child_load(before.local_names.at(receipt.source),receipt.type),address};
    }
    const auto done=child.builder_.label("parallel.for.iteration.end");
    child.loop_targets_.enter(done,done);child.lower_block(loop.body);child.loop_targets_.leave();
    if(!current_block_ended(child.builder_))child.builder_.emit(Jump{done});
    child.builder_.enter(child.builder_.add_block(done));child.builder_.emit(ReturnVoid{});
    child.flush_outlined_functions();
    for(auto& outlined:child.module_.functions)parent.outlined_functions_.push_back(std::move(outlined));
    instruction.nodes.push_back(std::move(node));
    const auto result=instruction.out;builder_.emit(std::move(instruction));release_owned(builder_,result,result_type);
    builder_.emit(Jump{joined});builder_.enter(builder_.add_block(joined));
    for(auto it=sources.rbegin();it!=sources.rend();++it) {
        parent.lifetime_.leave_loop_source();parent.lifetime_.release_loop_source(*it);
    }
    parent.scope_.restore(LocalScope::FullScopeSnapshot(before));parent.facts_.invalidate_after_join();
}
}
