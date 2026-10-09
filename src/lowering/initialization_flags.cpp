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
    const auto found = checked_.initialization_checks.find(&read);
    if (found == checked_.initialization_checks.end() ||
        found->second.subject != InitSubject::binding) return;
    const auto flag = flags_.find(found->second.binding);
    if (flag == flags_.end()) return;
    auto value = builder_.fresh();
    builder_.emit(LoadLocal{value, flag->second, Type::simple(TypeKind::Bool)});
    builder_.emit(InitializedCheck{
        value, ir_subject(found->second.subject), found->second.path,
        static_cast<std::uint32_t>(read.span.start.line),
        static_cast<std::uint32_t>(read.span.start.column)});
}

void InitializationFlags::check_field(const Expr& read, ValueId object) {
    const auto found = checked_.initialization_checks.find(&read);
    if (found == checked_.initialization_checks.end() ||
        found->second.subject != InitSubject::field) return;
    const auto& access = checked_.field_accesses.at(&read);
    const auto mask_index = checked_.classes.at(access.owner).fields.size();
    const auto word = Type::simple(TypeKind::Nat64);
    auto mask = builder_.fresh();
    builder_.emit(FieldGet{mask, object, mask_index, word});
    auto bit = builder_.fresh();
    builder_.emit(ConstantInt{bit, std::to_string(std::uint64_t{1} << access.index), word});
    auto selected = builder_.fresh();
    builder_.emit(Binary{selected, "AND", mask, bit, word, word});
    auto zero = builder_.fresh();
    builder_.emit(ConstantInt{zero, "0", word});
    auto set = builder_.fresh();
    builder_.emit(Binary{set, "!=", selected, zero, word, Type::simple(TypeKind::Bool)});
    builder_.emit(InitializedCheck{
        set, ir_subject(found->second.subject), found->second.path,
        static_cast<std::uint32_t>(read.span.start.line),
        static_cast<std::uint32_t>(read.span.start.column)});
}

void InitializationFlags::set_all(const std::vector<const Stmt*>& declarations) {
    if (current_block_ended(builder_)) return;
    for (const auto* declaration : declarations) set(*declaration);
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
