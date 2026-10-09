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
                        LocalScope& scope)
        : checked_(checked), builder_(builder), scope_(scope) {}

    // Whether the program keeps any flag. The lowering paths that read a
    // binding without lowering its name (statement idioms, in-place appends)
    // are not taken when it does.
    bool active() const {
        return !checked_.initialization_flags.empty() ||
               !checked_.initialization_masked_classes.empty();
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
    void after(const Stmt& statement);
    void after(const Expr& expression);

private:
    void set_all(const std::vector<const Stmt*>& declarations);

    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    // The flag local of each flagged declaration lowered so far.
    std::unordered_map<const Stmt*, std::string> flags_;
};

} // namespace quidra::lowering
