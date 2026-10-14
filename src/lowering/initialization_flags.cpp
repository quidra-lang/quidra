// InitializationFlags: see initialization_flags.hpp.

#include "lowering/initialization_flags.hpp"

#include "lowering/block_end.hpp"

#include <cstdint>

namespace quidra::lowering {

namespace {

ir::InitializedSubject ir_subject(InitSubject subject) {
    switch (subject) {
    case InitSubject::binding: return ir::InitializedSubject::binding;
    case InitSubject::field: return ir::InitializedSubject::field;
    case InitSubject::argument: return ir::InitializedSubject::argument;
    }
    return ir::InitializedSubject::binding;
}

std::string reference_path(const Expr& expression) {
    if(const auto* name=std::get_if<NameExpr>(&expression.data))return (name->this_qualifier?"this.":"")+name->name;
    if(const auto* member=std::get_if<MemberExpr>(&expression.data))return reference_path(*member->base)+"."+member->name;
    if(const auto* index=std::get_if<IndexExpr>(&expression.data))return reference_path(*index->base)+"[?]";
    return "reference";
}

} // namespace

void InitializationFlags::declare(const Stmt& declaration, const std::string& name) {
    if (!checked_.initialization_flags.contains(&declaration)) return;
    const auto flag = builder_.hidden("init." + name);
    const auto type = Type::simple(TypeKind::Bool);
    scope_.local_type(flag) = type;
    builder_.emit(DeclareLocal{flag, type, {}, 0, 0});
    builder_.emit(StoreLocal{flag, builder_.const_bool(false), type});
    flags_[&declaration] = flag;
}

void InitializationFlags::set(const Stmt& declaration) {
    const auto found = flags_.find(&declaration);
    if (found == flags_.end()) return;
    builder_.emit(StoreLocal{found->second, builder_.const_bool(true), Type::simple(TypeKind::Bool)});
}

void InitializationFlags::check(const Expr& read) {
    const auto bound=checked_.runtime_bound_checks.find(&read);
    const auto* name=std::get_if<NameExpr>(&read.data);
    if (!checked_.field_accesses.contains(&read) &&
        (bound!=checked_.runtime_bound_checks.end() ||
         (check_every_reference_access_ && name && scope_.is_source_reference(name->name)))) {
        const auto path=bound!=checked_.runtime_bound_checks.end()?bound->second:name->name;
        auto address=builder_.fresh();
        builder_.emit(ReferenceAddress{address,scope_.source_reference(path)});
        builder_.emit(ReferenceBoundCheck{address,path,
            {static_cast<std::uint32_t>(read.span.start.line),static_cast<std::uint32_t>(read.span.start.column)}});
    }
    const auto found = checked_.initialization_checks.find(&read);
    if (found == checked_.initialization_checks.end() ||
        found->second.subject != InitSubject::binding) return;
    const auto flag = flags_.find(found->second.binding);
    if (flag == flags_.end()) return;
    auto value = builder_.fresh();
    builder_.emit(LoadLocal{value, flag->second, Type::simple(TypeKind::Bool)});
    builder_.emit(InitializedCheck{
        value, ir_subject(found->second.subject), found->second.path,
        {static_cast<std::uint32_t>(read.span.start.line),
        static_cast<std::uint32_t>(read.span.start.column)}});
}

void InitializationFlags::check_field(const Expr& read, ValueId object) {
    const auto bound=checked_.runtime_bound_checks.find(&read);
    const auto access=checked_.field_accesses.find(&read);
    if(access!=checked_.field_accesses.end() && checked_.classes.at(access->second.owner).fields.at(access->second.index).reference &&
       (bound!=checked_.runtime_bound_checks.end() || check_every_reference_access_)) {
        auto address=builder_.fresh();builder_.emit(FieldReferenceAddress{address,object,access->second.index});
        builder_.emit(ReferenceBoundCheck{address,reference_path(read),
            {static_cast<std::uint32_t>(read.span.start.line),static_cast<std::uint32_t>(read.span.start.column)}});
    }
    if(const auto access=checked_.field_accesses.find(&read);access!=checked_.field_accesses.end() &&
       checked_.classes.at(access->second.owner).fields.at(access->second.index).reference)return;
    const auto found = checked_.initialization_checks.find(&read);
    if (found == checked_.initialization_checks.end() ||
        found->second.subject != InitSubject::field) return;
    const auto& field_access = checked_.field_accesses.at(&read);
    const auto mask_index = checked_.classes.at(field_access.owner).fields.size();
    const auto word = Type::simple(TypeKind::Nat64);
    auto mask = builder_.fresh();
    builder_.emit(FieldGet{mask, object, mask_index, word});
    auto bit = builder_.fresh();
    builder_.emit(ConstantInt{bit, std::to_string(std::uint64_t{1} << field_access.index), word});
    auto selected = builder_.fresh();
    builder_.emit(Binary{selected, "AND", mask, bit, word, word});
    auto zero = builder_.fresh();
    builder_.emit(ConstantInt{zero, "0", word});
    auto set = builder_.fresh();
    builder_.emit(Binary{set, "!=", selected, zero, word, Type::simple(TypeKind::Bool)});
    builder_.emit(InitializedCheck{
        set, ir_subject(found->second.subject), found->second.path,
        {static_cast<std::uint32_t>(read.span.start.line),
        static_cast<std::uint32_t>(read.span.start.column)}});
}

void InitializationFlags::set_all(const std::vector<const Stmt*>& declarations) {
    if (current_block_ended(builder_)) return;
    for (const auto* declaration : declarations) set(*declaration);
}

void InitializationFlags::for_iteration(const ForStmt& loop) {
    if(const auto declaration=checked_.iteration_initializes.find(&loop);declaration!=checked_.iteration_initializes.end())
        set(*declaration->second);
    for(const auto& binding:loop.collections)
        if(const auto declaration=checked_.collection_iteration_initializes.find(&binding);declaration!=checked_.collection_iteration_initializes.end())
            set(*declaration->second);
}

void InitializationFlags::after(const Stmt& statement) {
    if (flags_.empty()) return;
    if (const auto found = checked_.statement_initializes.find(&statement);
        found != checked_.statement_initializes.end())
        set_all(found->second);
}

void InitializationFlags::after(const Expr& expression) {
    if (flags_.empty()) return;
    if (const auto found = checked_.expression_initializes.find(&expression);
        found != checked_.expression_initializes.end())
        set_all(found->second);
}

} // namespace quidra::lowering
