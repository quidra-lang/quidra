#pragma once

// LocalScope: the source locals and references of the function being
// lowered, and the IR locals and references that hold them.
//
// A source name maps to the IR local (or IR reference) that stores it, and
// every IR local and reference has a type. Binding a source name again
// rebinds it: every binding is an assignment, and the last one wins.
//
// A branch, loop body or match case starts from the scope its construct
// started with: LocalNamesSnapshot saves the IR locals and the source local
// names, and restore() brings them back (references are not saved there).
// The main guard saves and restores all four maps (FullScopeSnapshot).
// Restores are explicit calls, never destructors.
//
// local_type(ir_name) is the type slot of an IR local, reached through
// operator[]: assigning to it declares a hidden IR local (the last assignment
// wins), and reading it inserts a default Type for a name the scope does not
// know yet. The *_at() reads require the name to be known.

#include "ir/function_builder.hpp"
#include "quidra/types.hpp"
#include <string>
#include <unordered_map>
#include <utility>

namespace quidra::lowering {

class LocalScope {
public:
    struct LocalNamesSnapshot {
        std::unordered_map<std::string, Type> locals;
        std::unordered_map<std::string, std::string> local_names;
    };
    struct FullScopeSnapshot {
        std::unordered_map<std::string, Type> locals;
        std::unordered_map<std::string, std::string> local_names;
        std::unordered_map<std::string, Type> references;
        std::unordered_map<std::string, std::string> reference_names;
    };

    // Binding a source name takes the next hidden name of `builder` for its IR
    // local ($local.NAME.N) or reference ($ref.NAME.N), and returns it.
    std::string bind_source_local(ir::FunctionBuilder& builder, const std::string& source_name, const Type& type) {
        const auto ir_name = builder.hidden("local." + source_name);
        bind_local(source_name, ir_name, type);
        return ir_name;
    }
    std::string bind_source_reference(ir::FunctionBuilder& builder, const std::string& source_name, const Type& type) {
        const auto ir_name = builder.hidden("ref." + source_name);
        bind_reference(source_name, ir_name, type);
        return ir_name;
    }
    // A parameter, or a constructor's receiver: the IR local has the source
    // name.
    void bind_parameter(const std::string& name, const Type& type) {
        locals_[name] = type;
        local_names_[name] = name;
    }

    bool has_source_local(const std::string& source_name) const { return local_names_.contains(source_name); }
    const std::string& source_local(const std::string& source_name) const { return local_names_.at(source_name); }
    const Type& source_local_type(const std::string& source_name) const { return locals_.at(source_local(source_name)); }
    bool is_source_reference(const std::string& source_name) const { return reference_names_.contains(source_name); }
    const std::string& source_reference(const std::string& source_name) const { return reference_names_.at(source_name); }

    Type& local_type(const std::string& ir_name) { return locals_[ir_name]; }
    const Type& local_type_at(const std::string& ir_name) const { return locals_.at(ir_name); }
    const Type& reference_type_at(const std::string& ir_name) const { return references_.at(ir_name); }

    void reset() {
        locals_.clear();
        local_names_.clear();
        reference_names_.clear();
        references_.clear();
    }

    LocalNamesSnapshot snapshot_local_names() const { return LocalNamesSnapshot{locals_, local_names_}; }
    void restore(const LocalNamesSnapshot& snapshot) {
        locals_ = snapshot.locals;
        local_names_ = snapshot.local_names;
    }
    FullScopeSnapshot snapshot_full() const {
        return FullScopeSnapshot{locals_, local_names_, references_, reference_names_};
    }
    void restore(FullScopeSnapshot&& snapshot) {
        locals_ = std::move(snapshot.locals);
        local_names_ = std::move(snapshot.local_names);
        references_ = std::move(snapshot.references);
        reference_names_ = std::move(snapshot.reference_names);
    }

private:
    // Binds `source_name` to the IR local `ir_name` of `type`.
    void bind_local(const std::string& source_name, const std::string& ir_name, const Type& type) {
        local_names_[source_name] = ir_name;
        locals_[ir_name] = type;
    }
    // Binds `source_name` to the IR reference `ir_name` of `type`.
    void bind_reference(const std::string& source_name, const std::string& ir_name, const Type& type) {
        reference_names_[source_name] = ir_name;
        references_[ir_name] = type;
    }

    std::unordered_map<std::string, Type> locals_;
    std::unordered_map<std::string, std::string> local_names_;
    std::unordered_map<std::string, std::string> reference_names_;
    std::unordered_map<std::string, Type> references_;
};

} // namespace quidra::lowering
