#pragma once

// StorageWrites: whether running an expression or a statement may write the
// storage that a place is rooted in, for the compound assignments whose store
// must reach the target's current storage (assignment_lowering.cpp) and for
// the value collection loops that must keep iterating the array they started
// with, and for the reference loops over a reference binding that check the
// array they iterate after the statements that may replace or resize it
// (control_flow_lowering.cpp). Every answer is a sound over-approximation:
// "none" means the storage cannot change; anything else only costs the
// lowering a second resolution of the place, a copy or a check.
//
// A place is a root binding (a local, a reference, a `&` parameter or a field
// of the receiver) followed by fields and array elements. An expression may
// write storage on the place's path:
//   - through the root binding itself (BorrowInference's mutation query: `&`
//     arguments, receivers whose receiver_effect writes, file handles and
//     tensor state);
//   - inside a method, by an implicit-receiver call whose receiver_effect
//     writes, when the root is a receiver field or a reference;
//   - through a shared region, by a call that receives a value holding a
//     ref.Cell, when the path holds one;
//   - through another binding that may alias the path: a writable argument,
//     a writing receiver, an assignment or a writable loop rooted in another
//     binding, when one of the two bindings is a reference or a `&`
//     parameter and the other binding's type can hold a value on the path
//     (for a loop, an element of the iterated array too).
// Defined in storage_writes.cpp.

#include "ir/function_builder.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quidra::lowering {

class StorageWrites {
public:
    StorageWrites(
        const CheckedProgram& checked, const BorrowInference& borrows,
        const ir::FunctionBuilder& builder, const LocalScope& scope)
        : checked_(checked), borrows_(borrows), builder_(builder),
          scope_(scope) {}

    // Whether evaluating `value` may write the root storage of the element
    // or field `target`: the root binding or any field or element on the way
    // to the target's slot.
    bool value_may_write_target_root(const Expr& target, const Expr& value) const;

    // How running `statement` may write the array that a value collection
    // loop over `iterable` reads, or the storage on its path: not at all;
    // only by replacing the array or a binding or field it is reached
    // through (an assignment to it), which leaves the old array intact; or
    // by writing the array's elements in place (anything else).
    enum class IterableWrite { none, replaces, in_place };
    IterableWrite statement_writes_iterable(const Stmt& statement, const Expr& iterable) const;

    // Whether `iterable` names a reference binding: a reference local or a
    // `&` parameter.
    bool names_reference_binding(const Expr& iterable) const;
    // Whether running `statement` may replace or resize the array that the
    // reference binding `iterable` designates (names_reference_binding), by
    // writing through another binding: a call with a writable argument or a
    // writing receiver (the implicit receiver included), or an assignment,
    // whose root binding may alias it and whose type can hold the array. An
    // assignment to storage the reference's target lies in (a reference
    // local's `matrix` in `&r = &matrix[0]`) is one of them. Writes of the
    // array's elements through the binding itself do not replace or resize
    // it.
    bool statement_may_reshape_iterable(const Stmt& statement, const Expr& iterable) const;

private:
    // A place's root binding and the types of the storage on its path.
    struct RootedPath {
        const Expr* root{};
        std::string name;
        std::vector<Type> types;
    };

    std::optional<RootedPath> target_path(const Expr& target) const;
    std::optional<RootedPath> iterable_path(const Expr& iterable) const;
    bool expression_may_write_path(const Expr& expression, const RootedPath& path) const;
    // The writes of one expression node that BorrowInference's query does
    // not see: implicit-receiver calls, shared regions and aliases.
    bool node_may_write_path(
        const Expr& node, const RootedPath& path, bool receiver_rooted,
        bool shared) const;
    bool path_receiver_rooted(const RootedPath& path) const;
    bool path_holds_shared_region(const RootedPath& path) const;
    bool statement_may_write_path(const Stmt& statement, const RootedPath& path) const;
    // An assignment (with `writable_loops`, a writable loop too) in `statement`
    // or a statement nested in it, rooted in another binding that may alias
    // the path.
    bool statement_writes_through_alias(
        const Stmt& statement, const RootedPath& path, bool writable_loops) const;
    IterableWrite classify_iterable_write(
        const Stmt& statement, const Expr& iterable, const RootedPath& path) const;
    bool is_reference_binding(const std::string& name, const Expr& root) const;
    bool is_receiver_field(const Expr& root) const;
    bool path_may_be_aliased_by(
        const RootedPath& path, const std::string& name, const Expr& root) const;
    bool type_can_hold(
        const Type& holder, const Type& held,
        std::unordered_set<std::string>& active) const;
    bool type_holds_shared_region(
        const Type& type, std::unordered_set<std::string>& active) const;
    bool expression_type_holds_shared_region(const Expr& expression) const;

    const CheckedProgram& checked_;
    const BorrowInference& borrows_;
    const ir::FunctionBuilder& builder_;
    const LocalScope& scope_;
};

} // namespace quidra::lowering
