#pragma once

// InitializationFlags: the run-time initialization checks of L13. A binding
// that some read may find uninitialized (CheckedProgram::initialization_flags)
// keeps a hidden bool local, false at its declaration and true from every
// place the checker lists where the binding becomes initialized (after a
// simple statement or a call expression). A read the checker could not
// prove initialized loads that flag and checks it (InitializedCheck), which
// fails with UNINITIALIZED naming the storage as written. Programs without
// such reads have no flags, and every member does nothing for them.
// Defined in initialization_flags.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "quidra/checker.hpp"
#include <string>
#include <unordered_map>

namespace quidra::lowering {

class InitializationFlags {
public:
    InitializationFlags(const CheckedProgram& checked, ir::FunctionBuilder& builder,
                        LocalScope& scope,bool check_every_reference_access=false)
        : checked_(checked), builder_(builder), scope_(scope),check_every_reference_access_(check_every_reference_access) {}

    // Whether the program keeps any flag. The lowering paths that read a
    // binding without lowering its name (statement idioms, in-place appends)
    // are not taken when it does.
    bool active() const {
        return !checked_.initialization_flags.empty() ||
               !checked_.initialization_masked_classes.empty() ||
               !checked_.runtime_bound_checks.empty() || check_every_reference_access_;
    }

    // The declaration of `name` by `declaration`: declares its flag, false,
    // when it keeps one.
    void declare(const Stmt& declaration, const std::string& name);
    // Sets the flag of the binding declared by `declaration`, when it keeps
    // one: the declaration gave the binding a value.
    void set(const Stmt& declaration);
    // The check of `read`, when the checker listed one for a binding.
    void check(const Expr& read);
    // The check of the field read `read` of `object`, when the checker listed
    // one: the field's bit in the object's initialization mask.
    void check_field(const Expr& read, ValueId object);
    // Sets the flags of the bindings `statement` or `expression` initializes,
    // when the current block continues.
    void for_iteration(const ForStmt& loop);
    void for_index_iteration(const ForIndexBinding& index);
    void after(const Stmt& statement);
    void after(const Expr& expression);

    const std::string* flag(const Stmt* declaration) const {
        const auto found=flags_.find(declaration);return found==flags_.end()?nullptr:&found->second;
    }
    void adopt(const Stmt* declaration,const std::string& storage){flags_[declaration]=storage;}
private:
    void set_all(const std::vector<const Stmt*>& declarations);

    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    bool check_every_reference_access_{};
    // The flag local of each flagged declaration lowered so far.
    std::unordered_map<const Stmt*, std::string> flags_;
};

} // namespace quidra::lowering
