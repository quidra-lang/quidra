#pragma once

// Effect summaries: what every function and method of a checked program may
// read and write of its caller's storage, which shared regions and hidden
// runtime state it may change, which external resources it uses, whether it
// observes the address of a const parameter, and where its result comes
// from. One analysis for every consumer (argument isolation of const
// values, mutation markers, parallel conflicts, the loop predicates), so
// that they never disagree about what a call may touch
// (effect_summary.cpp).
//
// Owns: EffectSummary, EffectSummaries and summarize_effects, which the
// checker runs at the end of a check; the result is CheckedProgram::effects.

#include "quidra/checker.hpp"
#include "semantics/storage_path.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace quidra::semantics {

// Shared commutes with every other shared use of the kind; Exclusive must be
// ordered against every other use, except that accesses through distinct
// handles are never ordered.
enum class ResourceUse : std::uint8_t { Shared, Exclusive };

// Direct for path-based file operations and every other operation;
// Handle for an operation through a file.Handle value, whose storage path
// `handle` names. A handle that does not escape a callee is summarized as a
// fresh handle: a Local root no caller path overlaps.
enum class ResourceOrigin : std::uint8_t { Direct, Handle };

struct ResourceAccess {
    ResourceKind kind{ResourceKind::Stdout};
    ResourceUse use{ResourceUse::Shared};
    ResourceOrigin origin{ResourceOrigin::Direct};
    StoragePath handle;

    friend bool operator==(const ResourceAccess& left, const ResourceAccess& right);
    friend bool operator<(const ResourceAccess& left, const ResourceAccess& right);
};

// Adds `access` to `accesses`, keeping one access per kind and origin: the
// stronger use wins.
void record_resource(std::set<ResourceAccess>& accesses, ResourceAccess access);

// Where a function's result comes from: a value it created, a copy of (part
// of) parameter `parameter`, or anything else that exists already.
struct ResultProvenance {
    enum class Kind : std::uint8_t { Fresh, Parameter, Existing };
    Kind kind{Kind::Fresh};
    std::size_t parameter{};

    friend bool operator==(const ResultProvenance& left, const ResultProvenance& right);
};

// The first hidden mutation of a function, for diagnostics: where it happens
// and what it changes ("field 'value'", "shared state of c", "runtime state
// through 'f'").
struct EffectCause {
    SourceSpan span{};
    std::string description;

    friend bool operator==(const EffectCause& left, const EffectCause& right);
};

struct EffectSummary {
    // Per parameter index, the paths the function may read and write through
    // that parameter's storage: the targets of its & parameters, and, for a
    // method, the receiver (index 0). By-value parameters are copies and
    // keep these sets empty.
    std::vector<PathSet> exposed_reads, exposed_writes;
    // A method that may write its receiver or the shared regions reachable
    // from it. A constructor's receiver is the value it creates, never
    // existing state.
    bool mutates_receiver{};
    // Per parameter, whether the shared regions reachable from it may
    // change: from a by-value parameter's value, from an & parameter's
    // target, from a method's receiver (index 0, which also sets
    // mutates_receiver).
    std::vector<bool> mutates_shared_of_parameter;
    // Runtime state that no parameter exposes: through a `.` extern, shared
    // state of unknown origin, or a callee that does.
    bool mutates_ambient{};
    // The shared regions the function may read, for read/write conflicts.
    PathSet shared_reads;
    // External I/O: never a hidden mutation, always a conflict reason.
    std::set<ResourceAccess> resources;
    // Per parameter, whether the function observes the address of a const
    // parameter or of a part of one (`&b`, `&b[0]`, `&b.f`).
    std::vector<bool> const_parameter_address_observed;
    ResultProvenance result;
    std::optional<EffectCause> first_hidden_mutation;

    friend bool operator==(const EffectSummary& left, const EffectSummary& right);
};

// Every function, method and constructor of a program by its internal name
// (`f`, `$method.C.m`, `$construct.C.0`), externs included. Moved, never
// copied.
class EffectSummaries {
public:
    EffectSummaries() = default;
    EffectSummaries(const EffectSummaries&) = delete;
    EffectSummaries& operator=(const EffectSummaries&) = delete;
    EffectSummaries(EffectSummaries&&) = default;
    EffectSummaries& operator=(EffectSummaries&&) = default;

    // Null for a name the program does not define.
    const EffectSummary* find(const std::string& function) const;
    const std::map<std::string, EffectSummary>& all() const { return summaries_; }
    void set(std::string function, EffectSummary summary);

    // The by-value call arguments (their expressions) that name storage the
    // call may write while it runs (semantics/argument_isolation.hpp).
    bool isolated(const Expr& argument) const;
    void set_isolated(std::set<const Expr*> arguments);

private:
    std::map<std::string, EffectSummary> summaries_;
    // Looked up by node, never iterated.
    std::set<const Expr*> isolated_;
};

// Whether values of `type` hold a shared region: a ref.Cell, the gradient
// state of an autograd.Target, an atomic.Counter or the runtime state of a
// file.Handle, directly or through fields, array elements and union cases.
// Mutating a copy of any other value is always local.
bool contains_shared_region(const CheckedProgram& checked, const Type& type);

// Computes the summary of every function of `checked`, bottom-up over the
// strongly connected components of the call graph, iterated to a fixed point
// inside each component, and, from the summaries, the arguments every call
// of the program, top-level code included, must isolate.
EffectSummaries summarize_effects(const CheckedProgram& checked);

} // namespace quidra::semantics
