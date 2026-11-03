// ir::lower: the module in a fixed order. The class layouts come first, in
// the checker's class order; then every function, every constructor and
// method of every class in declaration order, and the entry last (none in a
// library build). One
// Lowerer carries the per-program state (borrow inference, source
// inspections) across all of them.

#include "quidra/lowering.hpp"

#include "lowering/lowerer.hpp"
#include "quidra/ir/initialization_mask.hpp"
#include "lowering/user_sources.hpp"
#include "lowering/reference_storage.hpp"
#include "ir/fact_sources.hpp"
#include "semantics/interface_contracts.hpp"
#include "quidra/member_function_names.hpp"

#include <stdexcept>

namespace quidra::ir {

Module lower(
    const CheckedProgram& checked, const Expr* repl_expression,
    std::size_t replay_prefix_bytes, LoweringOptions options, CompileArtifact artifact) {
    lowering::Lowerer l(checked, repl_expression, replay_prefix_bytes, options);
    l.module().compiler_extensions = checked.compiler_extensions;
    l.module().declaration_catalog = checked.program.declaration_catalog;
    l.module().user_sources=lowering::user_source_table(checked.program);
    for(const auto& [name,info]:checked.classes){
        ClassLayout layout;layout.name=name;layout.standard_library=info.standard_library;
        for(const auto& field:info.fields){layout.field_names.push_back(field.name);layout.fields.push_back(field.type);if(field.reference)layout.reference_fields.insert(field.index);}
        if(checked.initialization_masked_classes.contains(name)){
            layout.field_names.emplace_back(initialization_mask_field);
            layout.fields.push_back(Type::simple(TypeKind::Nat64));
        }
        l.module().classes.push_back(std::move(layout));
    }
    for(const auto&f:checked.program.functions)l.lower_function(f);
    for(const auto&c:checked.program.classes){
        std::size_t constructors=0;
        for(const auto&m:c.methods){
            if(m.is_constructor){
                const auto internal=member_function_name::constructor(c.name,constructors++);
                if(checked.functions.contains(internal))l.lower_constructor(c.name,m,internal);
                continue;
            }
            const auto internal=member_function_name::method(c.name,m.name);
            if(checked.functions.contains(internal))l.lower_method(c.name,m);
        }
    }
    if(artifact!=CompileArtifact::Library)l.lower_main(checked.program.statements);
    l.flush_outlined_functions();
    lowering::place_reference_storage(l.module(), checked, options,
        [&](const auto& function,const auto& boundaries,const auto& removed) {
            l.binding_origins().remap_reference_storage(function,boundaries,removed);
        });
    if (checked.effects) {
        // Reference storage only appends hidden root arguments. Preserve all
        // original source indices and mark appended arguments as unmapped.
        for (const auto& function : l.module().functions) {
            const auto binding = l.summary_bindings().find(function.name);
            if (binding == l.summary_bindings().end()) continue;
            if (binding->second.semantic_parameter_for_ir_parameter.size() > function.parameters.size())
                throw std::logic_error("lowered summary signature lost a source parameter");
            binding->second.semantic_parameter_for_ir_parameter.resize(function.parameters.size());
        }
        l.module().summaries = std::make_shared<const analysis::ModuleSummaries>(
            analysis::adapt_summaries(l.module(), *checked.effects, l.summary_bindings()));
        facts::ResultExtentSources extent_sources;
        for (const auto& [name, function] : checked.functions)
            extent_sources.emplace(name, function.result_extents);
        l.module().fact_sources = std::make_shared<const facts::FactSources>(
            facts::adapt_fact_sources(l.module(), l.summary_bindings(), extent_sources));
    }
    const auto& interfaces = checked.interface_sources ? checked.interface_sources : checked.program.interface_sources;
    if (interfaces)
        l.module().interface_sources = semantics::enrich_interface_contracts(*interfaces,checked,l.module());
    l.module().binding_origins=l.binding_origins().finish(l.module(),l.summary_bindings(),
        checked.binding_entry_initialization.get());
    return std::move(l.module());
}

} // namespace quidra::ir
