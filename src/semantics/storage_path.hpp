#pragma once

// Abstract storage for the analyses after type checking: a root and the
// steps from it to a part of its storage. Effect summaries, argument
// isolation and the later mutation, conflict and loop analyses all speak in
// these paths, so they agree on what may overlap (storage_path.cpp).
//
// Owns: StorageRoot, PathStep, StoragePath, the overlap rule and the text of
// a path in diagnostics.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace quidra::semantics {

// The external resources a program touches. External I/O is never a hidden
// mutation of program state; it enters conflict detection only.
enum class ResourceKind : std::uint8_t {
    Stdout,
    Stdin,
    Stderr,
    FileSystem,
    Process,
    Network,
    Clock,
    Environment,
};

enum class RootKind : std::uint8_t {
    // A local of this invocation (a binding, a loop variable, a match binder,
    // the receiver a constructor builds).
    Local,
    // Parameter `index` of the function; a method's receiver is index 0.
    Parameter,
    // A shared region: storage whose identity survives copying, reached from
    // the storage `name` describes.
    Shared,
    // Runtime state behind `.` externs.
    Ambient,
    // An external resource of kind `resource`.
    Resource,
};

struct StorageRoot {
    RootKind kind{RootKind::Local};
    // Local: the binding's name. Shared: where the region was reached from.
    std::string name;
    // Parameter: its index.
    std::size_t index{};
    // Resource: its kind.
    ResourceKind resource{ResourceKind::Stdout};

    static StorageRoot local(std::string name);
    static StorageRoot parameter(std::size_t index);
    static StorageRoot shared(std::string provenance);
    static StorageRoot ambient();
    static StorageRoot of_resource(ResourceKind kind);

    friend bool operator==(const StorageRoot& left, const StorageRoot& right);
    friend bool operator<(const StorageRoot& left, const StorageRoot& right);
};

enum class StepKind : std::uint8_t {
    // A class field, by name.
    Field,
    // An array or tensor element at a constant index.
    Element,
    // An array or tensor element at an index the analysis does not know.
    AnyElement,
};

struct PathStep {
    StepKind kind{StepKind::Field};
    std::string field;
    std::int64_t index{};

    static PathStep field_named(std::string name);
    static PathStep element(std::int64_t index);
    static PathStep any_element();

    friend bool operator==(const PathStep& left, const PathStep& right);
    friend bool operator<(const PathStep& left, const PathStep& right);
};

struct StoragePath {
    StorageRoot root;
    std::vector<PathStep> steps;

    // This path followed by `step`, and by `more`.
    StoragePath child(PathStep step) const;
    StoragePath extended(const std::vector<PathStep>& more) const;

    friend bool operator==(const StoragePath& left, const StoragePath& right);
    friend bool operator<(const StoragePath& left, const StoragePath& right);
};

// Ordered, so that everything computed from paths comes out in one order.
using PathSet = std::set<StoragePath>;

// Two paths overlap when they have the same root and one is a prefix of the
// other, where an element at an unknown index matches every element step.
// Different fields, different constant elements and different roots never
// overlap.
bool overlaps(const StoragePath& left, const StoragePath& right);
bool overlaps_any(const PathSet& paths, const StoragePath& path);

// The path as a diagnostic names it: `values[0].x`, `this.items[?]`,
// `parameter 2`, `shared state of c`, `stdout`. `parameter_names` names the
// parameters by index when given.
std::string describe(const StoragePath& path,
                     const std::vector<std::string>& parameter_names = {});
std::string describe(ResourceKind kind);

} // namespace quidra::semantics
