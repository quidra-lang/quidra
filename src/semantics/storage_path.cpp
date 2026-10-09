// Storage paths (storage_path.hpp): construction, ordering, the overlap rule
// and the diagnostic text.

#include "semantics/storage_path.hpp"
#include <algorithm>
#include <tuple>

namespace quidra::semantics {

StorageRoot StorageRoot::local(std::string name) {
    StorageRoot root;
    root.kind = RootKind::Local;
    root.name = std::move(name);
    return root;
}

StorageRoot StorageRoot::parameter(std::size_t index) {
    StorageRoot root;
    root.kind = RootKind::Parameter;
    root.index = index;
    return root;
}

StorageRoot StorageRoot::shared(std::string provenance) {
    StorageRoot root;
    root.kind = RootKind::Shared;
    root.name = std::move(provenance);
    return root;
}

StorageRoot StorageRoot::ambient() {
    StorageRoot root;
    root.kind = RootKind::Ambient;
    return root;
}

StorageRoot StorageRoot::of_resource(ResourceKind kind) {
    StorageRoot root;
    root.kind = RootKind::Resource;
    root.resource = kind;
    return root;
}

bool operator==(const StorageRoot& left, const StorageRoot& right) {
    return left.kind == right.kind && left.name == right.name && left.index == right.index &&
           left.resource == right.resource;
}

bool operator<(const StorageRoot& left, const StorageRoot& right) {
    return std::tie(left.kind, left.name, left.index, left.resource) <
           std::tie(right.kind, right.name, right.index, right.resource);
}

PathStep PathStep::field_named(std::string name) {
    PathStep step;
    step.kind = StepKind::Field;
    step.field = std::move(name);
    return step;
}

PathStep PathStep::element(std::int64_t index) {
    PathStep step;
    step.kind = StepKind::Element;
    step.index = index;
    return step;
}

PathStep PathStep::any_element() {
    PathStep step;
    step.kind = StepKind::AnyElement;
    return step;
}

bool operator==(const PathStep& left, const PathStep& right) {
    return left.kind == right.kind && left.field == right.field && left.index == right.index;
}

bool operator<(const PathStep& left, const PathStep& right) {
    return std::tie(left.kind, left.field, left.index) <
           std::tie(right.kind, right.field, right.index);
}

StoragePath StoragePath::child(PathStep step) const {
    StoragePath path = *this;
    path.steps.push_back(std::move(step));
    return path;
}

StoragePath StoragePath::extended(const std::vector<PathStep>& more) const {
    StoragePath path = *this;
    path.steps.insert(path.steps.end(), more.begin(), more.end());
    return path;
}

bool operator==(const StoragePath& left, const StoragePath& right) {
    return left.root == right.root && left.steps == right.steps;
}

bool operator<(const StoragePath& left, const StoragePath& right) {
    if (left.root < right.root) return true;
    if (right.root < left.root) return false;
    return std::lexicographical_compare(left.steps.begin(), left.steps.end(),
                                        right.steps.begin(), right.steps.end());
}

namespace {

bool is_element(const PathStep& step) {
    return step.kind == StepKind::Element || step.kind == StepKind::AnyElement;
}

bool steps_may_meet(const PathStep& left, const PathStep& right) {
    if (left.kind == StepKind::Field || right.kind == StepKind::Field) {
        return left.kind == right.kind && left.field == right.field;
    }
    if (left.kind == StepKind::AnyElement || right.kind == StepKind::AnyElement) {
        return is_element(left) && is_element(right);
    }
    return left.index == right.index;
}

} // namespace

bool overlaps(const StoragePath& left, const StoragePath& right) {
    if (!(left.root == right.root)) return false;
    const auto common = std::min(left.steps.size(), right.steps.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (!steps_may_meet(left.steps[i], right.steps[i])) return false;
    }
    return true;
}

bool overlaps_any(const PathSet& paths, const StoragePath& path) {
    return std::any_of(paths.begin(), paths.end(),
                       [&](const StoragePath& candidate) { return overlaps(candidate, path); });
}

std::string describe(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Stdout: return "stdout";
    case ResourceKind::Stdin: return "stdin";
    case ResourceKind::Stderr: return "stderr";
    case ResourceKind::FileSystem: return "the file system";
    case ResourceKind::Process: return "processes";
    case ResourceKind::Network: return "the network";
    case ResourceKind::Clock: return "the clock";
    case ResourceKind::Environment: return "the environment";
    }
    return "a resource";
}

std::string describe(const StoragePath& path, const std::vector<std::string>& parameter_names) {
    std::string text;
    switch (path.root.kind) {
    case RootKind::Local:
        text = path.root.name;
        break;
    case RootKind::Parameter:
        if (path.root.index < parameter_names.size()) {
            text = parameter_names[path.root.index];
        } else {
            text = "parameter " + std::to_string(path.root.index);
        }
        break;
    case RootKind::Shared:
        text = "shared state of " + path.root.name;
        break;
    case RootKind::Ambient:
        text = "runtime state";
        break;
    case RootKind::Resource:
        text = describe(path.root.resource);
        break;
    }
    for (const auto& step : path.steps) {
        switch (step.kind) {
        case StepKind::Field:
            text += "." + step.field;
            break;
        case StepKind::Element:
            text += "[" + std::to_string(step.index) + "]";
            break;
        case StepKind::AnyElement:
            text += "[?]";
            break;
        }
    }
    return text;
}

} // namespace quidra::semantics
