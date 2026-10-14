#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "semantics/effect_summary.hpp"
#include "semantics/parallel_effects.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <limits>
namespace quidra::lowering {
void ConcurrencyLowering::lower_parallel(const Stmt& statement,const ParallelStmt& block) {
    if(block.body.size()>std::numeric_limits<std::uint32_t>::max())throw std::logic_error("parallel block exceeds node ABI limit");
    const auto* plan=checked_.effects ? checked_.effects->parallel_block(statement) : nullptr;
    if(!plan || plan->captures.size()!=block.body.size() || plan->predecessors.size()!=block.body.size())
        throw std::logic_error("parallel block lacks checked capture/dependency plan");
    auto& parent=lowerer_;
    const auto before=parent.scope_.snapshot_full();
    const auto table=builder_.hidden("parallel.table");
    const auto owner=builder_.function()->name;
    const auto void_type=Type::simple(TypeKind::Void), error_type=Type::simple(TypeKind::Error);
    const auto result_type=Type::union_of({void_type,error_type});
    std::unordered_map<const Stmt*,std::string> hoisted;
    std::unordered_map<const Stmt*,ShapeConstraintLowering::BraceShape> brace_shapes;
    std::unordered_map<std::string,std::string> shape_variables;
    // Storage is created before scheduling; initialization remains in its
    // declaring node, including evaluation of dynamic shape expressions.
    for(const auto& source:block.body) if(const auto* binding=std::get_if<BindingStmt>(&source->data)) {
        if(binding->literal_constant) continue;
        const auto type=checked_.binding_types.at(source.get());
        const auto name=binding->reference ? parent.scope_.bind_source_reference(builder_,binding->name,type)
            :parent.scope_.bind_source_local(builder_,binding->name,type);
        hoisted.emplace(source.get(),name);
        if(binding->reference) builder_.emit(DeclareReference{name,type,binding->is_const});
        else {
            builder_.emit(DeclareLocal{name,type,binding->name,static_cast<std::uint32_t>(source->span.start.line),static_cast<std::uint32_t>(source->span.start.column)});
            parent.initialization_flags_.declare(*source,binding->name);
            const auto reserve=[&](const auto& expressions){
                CapturedExtents extents;
                for(const auto& expression:expressions){
                    if(!expression){extents.emplace_back();continue;}
                    const auto extent=builder_.hidden("parallel.extent");
                    const auto extent_type=Type::simple(TypeKind::Int64);
                    parent.scope_.local_type(extent)=extent_type;
                    builder_.emit(DeclareLocal{extent,extent_type,{},0,0});extents.emplace_back(extent);
                }
                return extents;
            };
            if(type.kind==TypeKind::Tensor && binding->declared_type.brace_shape) {
                const auto found=checked_.shape_plans.find(&binding->declared_type);
                if(found==checked_.shape_plans.end())throw std::logic_error("parallel brace shape lacks checked plan");
                const auto& shape_plan=found->second;
                ShapeConstraintLowering::BraceShape shape;
                shape.extents.ellipsis = shape_plan.ellipsis;
                shape.extents.resize(shape_plan.axes.size());
                shape.variables.resize(shape_plan.variables.size());
                for(std::size_t axis=0;axis<shape_plan.axes.size();++axis) {
                    const bool reads_extent = std::any_of(shape_plan.variables.begin(), shape_plan.variables.end(),
                        [&](const auto& variable) { return variable.axis == axis && variable.read; });
                    if(shape_plan.axes[axis].check==ShapeAxisCheck::None && !reads_extent)continue;
                    const auto extent=builder_.hidden("parallel.extent");
                    parent.scope_.local_type(extent)=Type::simple(TypeKind::Int64);
                    builder_.emit(DeclareLocal{extent,Type::simple(TypeKind::Int64),{},0,0});
                    shape.extents[axis]=extent;
                }
                for(std::size_t v=0;v<shape_plan.variables.size();++v) {
                    const auto& variable=shape_plan.variables[v];
                    if(!variable.read)continue;
                    shape.variables[v]=shape.extents.at(variable.axis);
                    const auto storage=parent.scope_.bind_shape_variable(builder_,variable.name,Type::simple(TypeKind::Nat));
                    builder_.emit(DeclareLocal{storage,Type::simple(TypeKind::Nat),variable.name,
                        static_cast<std::uint32_t>(source->span.start.line),
                        static_cast<std::uint32_t>(source->span.start.column)});
                    if(!shape_variables.emplace(variable.name,storage).second)
                        throw std::logic_error("duplicate parallel shape variable");
                }
                for(std::size_t axis=0;axis<shape_plan.axes.size();++axis)
                    if(shape_plan.axes[axis].check==ShapeAxisCheck::Variable)
                        shape.extents[axis]=shape.variables.at(shape_plan.axes[axis].variable_index);
                parent.shapes_.constrain_tensor(name,shape.extents);
                brace_shapes.emplace(source.get(),std::move(shape));
            } else if(type.kind==TypeKind::Tensor) parent.shapes_.constrain_tensor(name,reserve(binding->declared_type.tensor_shape_expressions));
            if(type.kind==TypeKind::Array) parent.shapes_.constrain_array(name,reserve(binding->declared_type.dimension_expressions));
        }
    }
    ParallelBlock instruction;
    instruction.out=builder_.fresh();instruction.table_name=owner+"."+table;
    instruction.position.line=static_cast<std::uint32_t>(statement.span.start.line);
    instruction.position.column=static_cast<std::uint32_t>(statement.span.start.column);
    bool any_fallible=false;
    for(std::size_t index=0;index<block.body.size();++index) {
        const auto& source=*block.body[index];
        const bool fallible=index<plan->fallible.size() && plan->fallible[index];
        any_fallible|=fallible;
        Lowerer child(checked_,nullptr,0,parent.options_);
        child.module_.classes=parent.module_.classes;
        child.module_.compiler_extensions=parent.module_.compiler_extensions;
        child.module_.user_sources=parent.module_.user_sources;
        child.enclosing_class_=parent.enclosing_class_;
        Function function;
        function.name="$par."+owner+"."+table+"."+std::to_string(index);
        function.source_file=builder_.function()->source_file;
        function.source_line=static_cast<std::uint32_t>(source.span.start.line);
        function.source_column=static_cast<std::uint32_t>(source.span.start.column);
        function.result=fallible?result_type:void_type;
        function.parallel_node=ParallelNodeMetadata{function.name+".entry",{},fallible};
        ParallelNode node;
        node.function=function.name;node.fallible=fallible;
        // Until managed result-graph transfer is established, fallible
        // results are made on the joining thread. No foreign registry is read.
        node.worker_safe=!fallible && index<plan->worker_safe.size() && plan->worker_safe[index];
        node.predecessors=plan->predecessors[index];node.position.line=function.source_line;node.position.column=function.source_column;
        struct Capture {std::string source,storage;Type type;bool reference;const Stmt* binding;};
        std::vector<Capture> captures;
        const auto add=[&](std::string source_name,std::string storage,Type type,bool reference,const Stmt* binding){
            if(std::any_of(captures.begin(),captures.end(),[&](const auto& c){return c.storage==storage;}))return;
            captures.push_back({std::move(source_name),std::move(storage),std::move(type),reference,binding});
        };
        for(const auto& capture:plan->captures[index]) {
            const auto source_name=capture.name=="this"?std::string("$receiver"):capture.name;
            if(const auto variable=shape_variables.find(source_name);capture.shape_variable && variable!=shape_variables.end()) {
                add(source_name,variable->second,Type::simple(TypeKind::Nat),false,nullptr);
                continue;
            }
            const auto declared=hoisted.find(capture.binding);
            const bool holder=declared!=hoisted.end() ? std::get<BindingStmt>(capture.binding->data).reference
                :before.reference_names.contains(source_name);
            if(holder) {
                const auto& storage=declared!=hoisted.end()?declared->second:before.reference_names.at(source_name);
                const auto& type=declared!=hoisted.end()?checked_.binding_types.at(capture.binding):before.references.at(storage);
                add(source_name,storage,type,true,capture.binding);
            }else {
                const auto& storage=declared!=hoisted.end()?declared->second:before.local_names.at(source_name);
                const auto& type=declared!=hoisted.end()?checked_.binding_types.at(capture.binding):before.locals.at(storage);
                add(source_name,storage,type,false,capture.binding);
            }
        }
        if(const auto found=hoisted.find(&source);found!=hoisted.end()) {
            const auto& binding=std::get<BindingStmt>(source.data);
            add(binding.name,found->second,checked_.binding_types.at(&source),binding.reference,&source);
            if(const auto shape=brace_shapes.find(&source);shape!=brace_shapes.end()) {
                const auto& plan=checked_.shape_plans.at(&binding.declared_type);
                for(const auto& variable:plan.variables)if(variable.read)
                    add(variable.name,shape_variables.at(variable.name),Type::simple(TypeKind::Nat),false,nullptr);
            }
        }
        // Hidden checks travel with the captured storage, never reevaluate
        // an outer extent and never silently omit an initialization flag.
        const auto source_captures=captures;
        for(const auto& capture:source_captures) {
            if(capture.binding) if(const auto* flag=parent.initialization_flags_.flag(capture.binding))
                add(*flag,*flag,Type::simple(TypeKind::Bool),false,capture.binding);
            for(const auto* extents:{parent.shapes_.tensor_extents(capture.storage),parent.shapes_.array_extents(capture.storage)})
                if(extents) for(const auto& extent:*extents) if(extent)
                    add(*extent,*extent,Type::simple(TypeKind::Int64),false,nullptr);
        }
        for(const auto& capture:captures) {
            function.parameters.push_back({capture.storage,capture.type,true,true,false});
            if(capture.reference) function.parallel_node->reference_slots.push_back(capture.storage);
            const auto address=builder_.fresh();
            if(capture.reference)builder_.emit(AddressReferenceSlot{address,capture.storage});
            else builder_.emit(AddressLocal{address,capture.storage});
            node.captures.push_back({0,address});
        }
        child.functions_.begin_parallel_node(std::move(function));
        for(const auto& capture:captures) {
            child.scope_.bind_capture(capture.source,capture.storage,capture.type,capture.reference);
            if(capture.binding) if(const auto* flag=parent.initialization_flags_.flag(capture.binding))
                child.initialization_flags_.adopt(capture.binding,*flag);
            if(const auto* extents=parent.shapes_.tensor_extents(capture.storage))child.shapes_.constrain_tensor(capture.storage,*extents);
            if(const auto* extents=parent.shapes_.array_extents(capture.storage))child.shapes_.constrain_array(capture.storage,*extents);
        }
        if(hoisted.contains(&source))child.parallel_hoisted_.insert(&source);
        if(const auto shape=brace_shapes.find(&source);shape!=brace_shapes.end()) {
            child.parallel_brace_shapes_.emplace(&source,shape->second);
            const auto& binding=std::get<BindingStmt>(source.data);
            const auto& plan=checked_.shape_plans.at(&binding.declared_type);
            for(const auto& variable:plan.variables)if(variable.read)
                child.shape_constraints_.adopt_shape_variable(variable.name);
        }
        child.lower_statement(source);
        if(!current_block_ended(child.builder_)) {
            if(fallible){
                const auto value=child.builder_.fresh();
                child.builder_.emit(VariantMake{value,case_index(result_type,void_type),0,result_type,void_type});
                child.builder_.emit(Return{value,result_type});
            }else child.builder_.emit(ReturnVoid{});
        }
        child.flush_outlined_functions();
        for(auto& outlined:child.module_.functions)parent.outlined_functions_.push_back(std::move(outlined));
        instruction.nodes.push_back(std::move(node));
    }
    const auto result=instruction.out;
    builder_.emit(std::move(instruction));
    if(any_fallible) {
        const auto tag=builder_.fresh();builder_.emit(VariantTag{tag,result});
        const auto is_error=builder_.fresh();
        builder_.emit(Binary{is_error,"==",tag,builder_.const_int(case_index(result_type,error_type)),Type::simple(TypeKind::Int64),Type::simple(TypeKind::Bool)});
        const auto bad=builder_.label("parallel.error"),good=builder_.label("parallel.success");
        builder_.emit(Branch{is_error,bad,good});builder_.enter(builder_.add_block(bad));
        const auto error=builder_.fresh();builder_.emit(VariantPayload{error,result,error_type});
        const auto returned=parent.conversions_.convert(error,error_type,builder_.function()->result,true);
        release_owned(builder_,result,result_type);lifetime_.release_loop_sources();
        builder_.emit(Return{returned,builder_.function()->result});builder_.enter(builder_.add_block(good));
    }
    release_owned(builder_,result,result_type);
    parent.scope_.restore(LocalScope::FullScopeSnapshot(before));
    // Calls and concurrent writes invalidate assumptions made before the
    // barrier; the checker owns initialization facts after successful join.
    parent.facts_.invalidate_after_join();
}
}
