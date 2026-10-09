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
#include "quidra/member_function_names.hpp"

namespace quidra::ir {

Module lower(
    const CheckedProgram& checked, const Expr* repl_expression,
    std::size_t replay_prefix_bytes, LoweringOptions options, CompileArtifact artifact) {
    lowering::Lowerer l(checked, repl_expression, replay_prefix_bytes, options);
    l.module().compiler_extensions = checked.compiler_extensions;
    l.module().user_sources=lowering::user_source_table(checked.program);
    for(const auto& [name,info]:checked.classes){
        ClassLayout layout;layout.name=name;layout.standard_library=info.standard_library;
        for(const auto& field:info.fields){layout.field_names.push_back(field.name);layout.fields.push_back(field.type);}
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
    return std::move(l.module());
}

} // namespace quidra::ir
