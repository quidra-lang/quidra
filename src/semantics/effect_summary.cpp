// Effect summaries (effect_summary.hpp). A body is walked with the storage
// its names denote: locals of the invocation, parameters by index (a
// method's receiver is 0), and, through ref.Cell fields and the runtime
// handles, shared regions. Every read and write of a path rooted at an
// exposed parameter (an & parameter's target, a method's receiver) enters
// the summary; writes to locals and to by-value parameters are writes to
// copies and stay local. A call applies its callee's summary through the
// call's arguments, so summaries compose; summarize_effects orders the
// bodies bottom-up over the strongly connected components of the call graph
// and iterates each component to a fixed point.

#include "semantics/effect_summary.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"
#include <algorithm>
#include <functional>
#include <tuple>
#include <utility>
#include <variant>

namespace quidra::semantics {

bool operator==(const ResourceAccess& left, const ResourceAccess& right) {
    return left.kind == right.kind && left.use == right.use && left.origin == right.origin &&
           left.handle == right.handle;
}

bool operator<(const ResourceAccess& left, const ResourceAccess& right) {
    if (std::tie(left.kind, left.origin) != std::tie(right.kind, right.origin)) {
        return std::tie(left.kind, left.origin) < std::tie(right.kind, right.origin);
    }
    if (left.handle < right.handle) return true;
    if (right.handle < left.handle) return false;
    return left.use < right.use;
}

void record_resource(std::set<ResourceAccess>& accesses, ResourceAccess access) {
    for (auto it = accesses.begin(); it != accesses.end(); ++it) {
        if (it->kind != access.kind || it->origin != access.origin || !(it->handle == access.handle)) {
            continue;
        }
        if (it->use == ResourceUse::Exclusive || access.use == it->use) return;
        accesses.erase(it);
        break;
    }
    accesses.insert(std::move(access));
}

bool operator==(const ResultProvenance& left, const ResultProvenance& right) {
    return left.kind == right.kind && left.parameter == right.parameter;
}

bool operator==(const EffectCause& left, const EffectCause& right) {
    return left.span.start.line == right.span.start.line &&
           left.span.start.column == right.span.start.column &&
           left.description == right.description;
}

bool operator==(const EffectSummary& left, const EffectSummary& right) {
    return left.exposed_reads == right.exposed_reads &&
           left.exposed_writes == right.exposed_writes &&
           left.mutates_receiver == right.mutates_receiver &&
           left.mutates_shared_of_parameter == right.mutates_shared_of_parameter &&
           left.mutates_ambient == right.mutates_ambient &&
           left.shared_reads == right.shared_reads && left.resources == right.resources &&
           left.const_parameter_address_observed == right.const_parameter_address_observed &&
           left.result == right.result && left.first_hidden_mutation == right.first_hidden_mutation;
}

const EffectSummary* EffectSummaries::find(const std::string& function) const {
    const auto found = summaries_.find(function);
    return found == summaries_.end() ? nullptr : &found->second;
}

void EffectSummaries::set(std::string function, EffectSummary summary) {
    summaries_[std::move(function)] = std::move(summary);
}

bool EffectSummaries::isolated(const Expr& argument) const {
    return isolated_.contains(&argument);
}

void EffectSummaries::set_isolated(std::set<const Expr*> arguments) {
    isolated_ = std::move(arguments);
}

namespace {

// A ref.Cell instance: a class of the standard library by origin, never by
// name alone.
bool is_ref_cell(const CheckedProgram& checked, const Type& type) {
    if (type.kind != TypeKind::Class) return false;
    const auto info = checked.classes.find(type.class_name);
    return info != checked.classes.end() &&
           standard_class::is_ref_cell_instance(type.class_name, info->second.standard_library);
}

// The runtime handles whose state every copy shares and some method
// changes: an atomic counter's cell, a Target's gradient state, a file
// handle's position and open state.
bool is_shared_handle(const Type& type) {
    return type.kind == TypeKind::Class &&
           (type.class_name == standard_class::atomic_counter ||
            type.class_name == standard_class::autograd_target ||
            type.class_name == standard_class::file_handle);
}

bool shared_region_in(const CheckedProgram& checked, const Type& type,
                      std::set<std::string>& visiting) {
    if (is_ref_cell(checked, type) || is_shared_handle(type)) return true;
    if (type.kind == TypeKind::Array || type.kind == TypeKind::Tensor) {
        return type.first && shared_region_in(checked, *type.first, visiting);
    }
    if (type.kind == TypeKind::Union) {
        return std::any_of(type.cases.begin(), type.cases.end(), [&](const Type& alternative) {
            return shared_region_in(checked, alternative, visiting);
        });
    }
    if (type.kind != TypeKind::Class) return false;
    if (!visiting.insert(type.class_name).second) return false;
    const auto info = checked.classes.find(type.class_name);
    if (info == checked.classes.end()) return false;
    return std::any_of(info->second.fields.begin(), info->second.fields.end(),
                       [&](const ClassFieldType& field) {
                           return shared_region_in(checked, field.type, visiting);
                       });
}

} // namespace

bool contains_shared_region(const CheckedProgram& checked, const Type& type) {
    std::set<std::string> visiting;
    return shared_region_in(checked, type, visiting);
}

namespace {

using Provenance = ResultProvenance;

Provenance fresh() { return Provenance{Provenance::Kind::Fresh, 0}; }
Provenance existing() { return Provenance{Provenance::Kind::Existing, 0}; }
Provenance from_parameter(std::size_t index) {
    return Provenance{Provenance::Kind::Parameter, index};
}

// The join of two provenances: equal ones stay, different ones are Existing.
// An absent provenance is the bottom (not known yet).
std::optional<Provenance> join(const std::optional<Provenance>& left,
                               const std::optional<Provenance>& right) {
    if (!left) return right;
    if (!right) return left;
    if (*left == *right) return left;
    return existing();
}

// A place: storage the analysis can name. A place inside a shared region
// remembers the storage, in the function's own frame, it was reached from.
struct Place {
    StoragePath path;
    std::optional<StoragePath> shared_origin;

    friend bool operator<(const Place& left, const Place& right) {
        if (left.path < right.path) return true;
        if (right.path < left.path) return false;
        return left.shared_origin < right.shared_origin;
    }
};

// What an expression denotes as storage: the places it may be, or, when the
// analysis cannot tell, any storage at all.
struct Places {
    bool unknown{};
    std::set<Place> places;
};

struct Binding {
    enum class Kind : std::uint8_t { Value, Reference, ValueParameter, ReferenceParameter };
    Kind kind{Kind::Value};
    std::size_t parameter{};
    bool is_const{};
    const void* key{};
};

// One function, method or constructor of the program, or an extern.
struct Body {
    std::string name;
    const FunctionDecl* declaration{};
    const FunctionType* signature{};
    // The program's top-level statements, which have no summary of their
    // own but whose calls isolate arguments too.
    enum class Kind : std::uint8_t { Function, Method, Constructor, External, Entry } kind{Kind::Function};
    std::string class_name;
    const std::vector<StmtPtr>* statements{};
};

// A write made while the arguments of a call are evaluated.
struct LoggedWrite {
    bool unknown{};
    Place place;
};

// Applies `visit` to every call target an expression or statement may reach
// directly: functions, methods, constructors, the functions that indirect
// calls and task.all may run, and the field defaults a construction or a
// plain class declaration evaluates.
class CalleeCollector {
public:
    CalleeCollector(const CheckedProgram& checked, const std::set<std::string>& referenced,
                    std::set<std::string>& out)
        : checked_(checked), referenced_(referenced), out_(out) {}

    void block(const std::vector<StmtPtr>& body) {
        for (const auto& statement : body) stmt(*statement);
    }

    void class_defaults(const std::string& class_name) {
        if (!defaults_seen_.insert(class_name).second) return;
        const auto info = checked_.classes.find(class_name);
        if (info == checked_.classes.end()) return;
        for (const auto& field : info->second.fields) {
            if (field.default_value) expr(*field.default_value);
        }
    }

    void stmt(const Stmt& statement) {
        if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
            if (node->value) expr(*node->value);
            else if (const auto type = checked_.binding_types.find(&statement);
                     type != checked_.binding_types.end() && type->second.kind == TypeKind::Class) {
                class_defaults(type->second.class_name);
            }
        } else if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
            expr(*node->target);
            expr(*node->value);
        } else if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
            expr(*node->target);
        } else if (const auto* node = std::get_if<ReturnStmt>(&statement.data)) {
            if (node->value) expr(*node->value);
        } else if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
            expr(*node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
            expr(*node->condition);
            block(node->then_body);
            block(node->else_body);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
            block(node->body);
        } else if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
            expr(*node->condition);
            block(node->body);
        } else if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
            expr(*node->iterable);
            block(node->body);
        } else if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
            expr(*node->value);
            for (const auto& match_case : node->cases) block(match_case.body);
        }
    }

    void expr(const Expr& expression) {
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) expr(*item);
        } else if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : node->elements) expr(*item);
        } else if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
            expr(*node->base);
            for (const auto& item : node->items) {
                if (item.index) expr(*item.index);
                if (item.start) expr(*item.start);
                if (item.stop) expr(*item.stop);
                if (item.step) expr(*item.step);
            }
        } else if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
            expr(*node->base);
        } else if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
            expr(*node->operand);
        } else if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
            expr(*node->left);
            expr(*node->right);
        } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            expr(*node->value);
        } else if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
            for (const auto& item : node->conditions) expr(*item);
            for (const auto& item : node->values) expr(*item);
            expr(*node->otherwise);
        } else if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            call(expression, *node);
        } else if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            expr(*node->receiver);
            for (const auto& argument : node->args) expr(*argument.value);
            if (const auto method = checked_.method_calls.find(&expression);
                method != checked_.method_calls.end()) {
                target(method->second.internal_name);
            }
        }
    }

private:
    void target(const std::string& name) {
        out_.insert(name);
        // Defaults of omitted arguments are evaluated by the caller.
        const auto signature = checked_.functions.find(name);
        if (signature == checked_.functions.end()) return;
        for (const auto& parameter : signature->second.parameters) {
            if (parameter.default_value && defaults_seen_.insert(name + "#" + parameter.name).second) {
                expr(*parameter.default_value);
            }
        }
    }

    void call(const Expr& expression, const CallExpr& node) {
        for (const auto& argument : node.args) expr(*argument.value);
        const auto resolution = checked_.call_resolutions.find(&expression);
        if (resolution == checked_.call_resolutions.end()) return;
        switch (resolution->second.kind) {
        case CallKind::Function:
            target(resolution->second.target);
            break;
        case CallKind::FunctionValue:
            for (const auto& name : referenced_) out_.insert(name);
            break;
        case CallKind::Constructor:
            if (resolution->second.type.kind == TypeKind::Class) {
                class_defaults(resolution->second.target);
            }
            break;
        case CallKind::Builtin:
            if (resolution->second.builtin == BuiltinCallable::TaskAll) {
                for (const auto& name : referenced_) out_.insert(name);
            }
            break;
        case CallKind::NumericCast:
            break;
        }
    }

    const CheckedProgram& checked_;
    const std::set<std::string>& referenced_;
    std::set<std::string>& out_;
    std::set<std::string> defaults_seen_;
};

constexpr std::size_t max_path_steps = 8;

// The walks below name every kind of expression and statement: an effect
// they skipped would be lost from every summary. A new kind is added to
// CalleeCollector, BodyWalker::read_expr and provenance_of (expressions) or
// CalleeCollector::stmt and BodyWalker::walk_stmt (statements) before these
// counts move.
static_assert(std::variant_size_v<Expr::Data> == 18, "effect summaries: walk the new expression kind");
static_assert(std::variant_size_v<Stmt::Data> == 11, "effect summaries: walk the new statement kind");

// Walks one body and computes its summary from the current summaries of its
// callees.
class BodyWalker {
public:
    BodyWalker(const CheckedProgram& checked, const EffectSummaries& summaries,
               const std::set<std::string>& referenced, const Body& body,
               std::set<const Expr*>& isolated)
        : checked_(checked), summaries_(summaries), referenced_(referenced), body_(body),
          isolated_(isolated) {}

    EffectSummary summarize() {
        const auto count = body_.signature ? body_.signature->parameters.size() : 0;
        // The places of reference bindings and the provenance of locals only
        // grow; a body is walked again when one grew after it was read.
        for (int round = 0; round < 32; ++round) {
            summary_ = EffectSummary{};
            summary_.exposed_reads.assign(count, {});
            summary_.exposed_writes.assign(count, {});
            summary_.mutates_shared_of_parameter.assign(count, false);
            summary_.const_parameter_address_observed.assign(count, false);
            result_.reset();
            read_keys_.clear();
            grew_after_read_ = false;
            walk_body();
            if (!grew_after_read_) break;
        }
        summary_.result = result_.value_or(fresh());
        return std::move(summary_);
    }

private:
    bool is_method() const { return body_.kind == Body::Kind::Method; }
    bool is_constructor() const { return body_.kind == Body::Kind::Constructor; }

    // Whether the storage of parameter `index` belongs to the caller: an &
    // parameter's target, or a method's receiver.
    bool exposed(std::size_t index) const {
        if (is_method() && index == 0) return true;
        if (!body_.signature || index >= body_.signature->parameters.size()) return false;
        return body_.signature->parameters[index].writable;
    }

    std::vector<std::string> parameter_names() const {
        std::vector<std::string> names;
        if (!body_.signature) return names;
        for (const auto& parameter : body_.signature->parameters) names.push_back(parameter.name);
        if (is_method() && !names.empty()) names[0] = "this";
        return names;
    }

    void walk_body() {
        scopes_.clear();
        scopes_.emplace_back();
        if (body_.kind == Body::Kind::Entry) {
            if (body_.statements) walk_block(*body_.statements);
            return;
        }
        if (!body_.declaration || !body_.signature) return;
        const auto& parameters = body_.signature->parameters;
        const std::size_t first = is_method() ? 1 : 0;
        for (std::size_t i = first; i < parameters.size(); ++i) {
            Binding binding;
            binding.kind = parameters[i].writable ? Binding::Kind::ReferenceParameter
                                                  : Binding::Kind::ValueParameter;
            binding.parameter = i;
            binding.is_const = parameters[i].is_const;
            scopes_.back()[parameters[i].name] = binding;
        }
        if (is_constructor()) walk_class_defaults(body_.class_name, {});
        walk_block(body_.declaration->body);
    }

    // ---- names and places --------------------------------------------------

    const Binding* lookup(const std::string& name) const {
        for (std::size_t i = scopes_.size(); i > scope_floor_; --i) {
            const auto found = scopes_[i - 1].find(name);
            if (found != scopes_[i - 1].end()) return &found->second;
        }
        return nullptr;
    }

    void declare(const std::string& name, Binding binding) {
        scopes_.back()[name] = binding;
    }

    StoragePath receiver_path() const {
        if (is_method()) return StoragePath{StorageRoot::parameter(0), {}};
        return StoragePath{StorageRoot::local("this"), {}};
    }

    const Type* type_of(const Expr& expression) const {
        const auto found = checked_.expr_types.find(&expression);
        return found == checked_.expr_types.end() ? nullptr : &found->second;
    }

    const Places& reference_places(const void* key) {
        read_keys_.insert(key);
        return reference_targets_[key];
    }

    void add_reference_places(const void* key, const Places& places) {
        auto& current = reference_targets_[key];
        bool grew = false;
        if (places.unknown && !current.unknown) {
            current.unknown = true;
            grew = true;
        }
        for (const auto& place : places.places) grew = current.places.insert(place).second || grew;
        if (grew && read_keys_.contains(key)) grew_after_read_ = true;
    }

    std::optional<Provenance> local_provenance(const void* key) {
        read_keys_.insert(key);
        const auto found = provenance_.find(key);
        if (found == provenance_.end()) return std::nullopt;
        return found->second;
    }

    void add_local_provenance(const void* key, const std::optional<Provenance>& provenance) {
        if (!provenance) return;
        const auto found = provenance_.find(key);
        const auto joined = join(found == provenance_.end() ? std::nullopt
                                                            : std::optional<Provenance>(found->second),
                                 provenance);
        if (found != provenance_.end() && found->second == *joined) return;
        provenance_[key] = *joined;
        if (read_keys_.contains(key)) grew_after_read_ = true;
    }

    static PathStep index_step(const IndexPart& item) {
        if (!item.slice && item.index) {
            if (const auto* literal = std::get_if<IntegerExpr>(&item.index->data);
                literal && literal->fits_u64 &&
                literal->value <= static_cast<std::uint64_t>(INT64_MAX)) {
                return PathStep::element(static_cast<std::int64_t>(literal->value));
            }
        }
        return PathStep::any_element();
    }

    // The places an expression denotes as storage, or nothing for a value
    // that is not storage (a temporary).
    std::optional<Places> places_of(const Expr& expression) {
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (name->this_qualifier) {
                Places places;
                places.places.insert(
                    Place{receiver_path().child(PathStep::field_named(name->name)), std::nullopt});
                return places;
            }
            const auto* binding = lookup(name->name);
            if (!binding) return std::nullopt;
            Places places;
            switch (binding->kind) {
            case Binding::Kind::Value:
                places.places.insert(Place{StoragePath{StorageRoot::local(name->name), {}}, std::nullopt});
                return places;
            case Binding::Kind::ValueParameter:
            case Binding::Kind::ReferenceParameter:
                places.places.insert(
                    Place{StoragePath{StorageRoot::parameter(binding->parameter), {}}, std::nullopt});
                return places;
            case Binding::Kind::Reference:
                return reference_places(binding->key);
            }
            return std::nullopt;
        }
        if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
            auto base = places_of(*member->base);
            if (!base) return std::nullopt;
            const auto* base_type = type_of(*member->base);
            const bool enters_cell = base_type && is_ref_cell(checked_, *base_type);
            const auto names = enters_cell ? parameter_names() : std::vector<std::string>{};
            Places places;
            places.unknown = base->unknown;
            for (const auto& place : base->places) {
                if (enters_cell && !place.shared_origin) {
                    places.places.insert(Place{
                        StoragePath{StorageRoot::shared(describe(place.path, names)),
                                    {PathStep::field_named(member->name)}},
                        place.path});
                } else {
                    places.places.insert(
                        Place{place.path.child(PathStep::field_named(member->name)), place.shared_origin});
                }
            }
            return places;
        }
        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            auto base = places_of(*index->base);
            if (!base) return std::nullopt;
            std::vector<PathStep> steps;
            for (const auto& item : index->items) steps.push_back(index_step(item));
            Places places;
            places.unknown = base->unknown;
            for (const auto& place : base->places) {
                places.places.insert(Place{place.path.extended(steps), place.shared_origin});
            }
            return places;
        }
        return std::nullopt;
    }

    // ---- recording -----------------------------------------------------------

    // Records the first hidden mutation; `description` runs only for it.
    template <typename Describe>
    void note_hidden(SourceSpan span, Describe description) {
        if (!summary_.first_hidden_mutation) {
            summary_.first_hidden_mutation = EffectCause{span, std::string(description())};
        }
    }

    void hidden_ambient(SourceSpan span, const char* what) {
        summary_.mutates_ambient = true;
        note_hidden(span, [&] { return std::string(what); });
    }

    void hidden_ambient_through(SourceSpan span, const std::string& callee) {
        summary_.mutates_ambient = true;
        note_hidden(span, [&] { return "runtime state through '" + callee + "'"; });
    }

    void read_unknown() {
        for (std::size_t i = 0; i < summary_.exposed_reads.size(); ++i) {
            if (exposed(i)) summary_.exposed_reads[i].insert(StoragePath{StorageRoot::parameter(i), {}});
        }
    }

    void record_read(const Places& places) {
        if (places.unknown) read_unknown();
        for (const auto& place : places.places) {
            if (place.shared_origin) {
                summary_.shared_reads.insert(place.path);
                continue;
            }
            if (place.path.root.kind == RootKind::Parameter && exposed(place.path.root.index)) {
                summary_.exposed_reads[place.path.root.index].insert(place.path);
            }
        }
    }

    // A change of the shared regions reachable from `origin`, a path of this
    // frame. Whose state that is depends on the root: a parameter's (the
    // receiver's for a method), nobody else's for a fresh local, and
    // possibly anybody's for a local whose value already existed.
    void shared_mutation(const StoragePath& origin, SourceSpan span) {
        const auto names = [&] { return parameter_names(); };
        switch (origin.root.kind) {
        case RootKind::Parameter: {
            const auto index = origin.root.index;
            if (index < summary_.mutates_shared_of_parameter.size()) {
                summary_.mutates_shared_of_parameter[index] = true;
            }
            if (is_method() && index == 0) summary_.mutates_receiver = true;
            if (!exposed(index) || (is_method() && index == 0)) {
                note_hidden(span, [&] { return "shared state reachable from " + describe(origin, names()); });
            }
            return;
        }
        case RootKind::Local: {
            if (is_constructor() && origin.root.name == "this") return;
            const auto* binding = lookup(origin.root.name);
            const auto provenance = binding ? local_provenance(binding->key) : std::optional<Provenance>(existing());
            if (!provenance || provenance->kind == Provenance::Kind::Fresh) return;
            if (provenance->kind == Provenance::Kind::Parameter) {
                shared_mutation(StoragePath{StorageRoot::parameter(provenance->parameter), {}}, span);
                return;
            }
            summary_.mutates_ambient = true;
            note_hidden(span, [&] { return "shared state reachable from " + describe(origin, names()); });
            return;
        }
        case RootKind::Shared:
        case RootKind::Ambient:
        case RootKind::Resource:
            hidden_ambient(span, "runtime state");
            return;
        }
    }

    void write_unknown(SourceSpan span) {
        for (std::size_t i = 0; i < summary_.exposed_writes.size(); ++i) {
            if (exposed(i)) summary_.exposed_writes[i].insert(StoragePath{StorageRoot::parameter(i), {}});
        }
        if (is_method()) summary_.mutates_receiver = true;
        hidden_ambient(span, "storage of an unknown reference");
    }

    void record_write(const Places& places, SourceSpan span) {
        for (auto* log : write_logs_) {
            if (places.unknown) log->push_back(LoggedWrite{true, {}});
            for (const auto& place : places.places) log->push_back(LoggedWrite{false, place});
        }
        if (places.unknown) write_unknown(span);
        for (const auto& place : places.places) {
            if (place.shared_origin) {
                shared_mutation(*place.shared_origin, span);
                continue;
            }
            if (place.path.root.kind != RootKind::Parameter) continue;
            const auto index = place.path.root.index;
            if (!exposed(index)) continue;
            summary_.exposed_writes[index].insert(place.path);
            if (is_method() && index == 0) {
                summary_.mutates_receiver = true;
                note_hidden(span, [&] {
                    auto field = describe(StoragePath{StorageRoot::local({}), place.path.steps});
                    if (!field.empty() && field.front() == '.') field.erase(0, 1);
                    return "field '" + field + "'";
                });
            }
        }
    }

    // The address of a const parameter, or of a part of one, is observed.
    void observe_address(const Places& places) {
        for (const auto& place : places.places) {
            if (place.shared_origin || place.path.root.kind != RootKind::Parameter) continue;
            const auto index = place.path.root.index;
            if (!body_.signature || index >= body_.signature->parameters.size()) continue;
            if (is_method() && index == 0) continue;
            const auto& parameter = body_.signature->parameters[index];
            if (parameter.is_const && !parameter.writable) {
                summary_.const_parameter_address_observed[index] = true;
            }
        }
    }

    void record_resource_use(ResourceKind kind, ResourceUse use) {
        record_resource(summary_.resources, ResourceAccess{kind, use, ResourceOrigin::Direct, {}});
    }

    // ---- statements ------------------------------------------------------------

    void walk_block(const std::vector<StmtPtr>& body) {
        scopes_.emplace_back();
        for (const auto& statement : body) walk_stmt(*statement);
        scopes_.pop_back();
    }

    void walk_class_defaults(const std::string& class_name, const std::set<std::string>& supplied) {
        const auto info = checked_.classes.find(class_name);
        if (info == checked_.classes.end()) return;
        // A default sees no local of the code that constructs the value.
        const auto floor = scope_floor_;
        scope_floor_ = scopes_.size();
        scopes_.emplace_back();
        for (const auto& field : info->second.fields) {
            if (field.default_value && !supplied.contains(field.name)) read_expr(*field.default_value);
        }
        scopes_.pop_back();
        scope_floor_ = floor;
    }

    void walk_stmt(const Stmt& statement) {
        if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
            Binding binding;
            binding.key = &statement;
            binding.is_const = node->is_const;
            if (node->reference) {
                binding.kind = Binding::Kind::Reference;
                if (node->value) {
                    read_storage_operands(*node->value);
                    auto places = places_of(*node->value);
                    if (!places) places = Places{true, {}};
                    observe_address(*places);
                    add_reference_places(&statement, *places);
                }
                declare(node->name, binding);
                return;
            }
            std::optional<Provenance> provenance = fresh();
            if (node->value) {
                read_expr(*node->value);
                provenance = provenance_of(*node->value);
            } else if (const auto type = checked_.binding_types.find(&statement);
                       type != checked_.binding_types.end() && type->second.kind == TypeKind::Class) {
                walk_class_defaults(type->second.class_name, {});
            }
            binding.kind = Binding::Kind::Value;
            declare(node->name, binding);
            add_local_provenance(&statement, provenance);
        } else if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
            read_storage_operands(*node->target);
            auto target = places_of(*node->target);
            if (!node->compound_op.empty() && target) record_read(*target);
            read_expr(*node->value);
            if (target) {
                record_write(*target, statement.span);
                if (const auto* name = std::get_if<NameExpr>(&node->target->data);
                    name && !name->this_qualifier) {
                    if (const auto* binding = lookup(name->name);
                        binding && binding->kind == Binding::Kind::Value) {
                        add_local_provenance(binding->key, node->compound_op.empty()
                                                               ? provenance_of(*node->value)
                                                               : std::optional<Provenance>(fresh()));
                    }
                }
            }
        } else if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
            read_storage_operands(*node->target);
            auto places = places_of(*node->target);
            if (!places) places = Places{true, {}};
            if (const auto* binding = lookup(node->name);
                binding && binding->kind == Binding::Kind::Reference) {
                observe_address(*places);
                add_reference_places(binding->key, *places);
            }
        } else if (const auto* node = std::get_if<ReturnStmt>(&statement.data)) {
            if (node->value) {
                read_expr(*node->value);
                result_ = join(result_, provenance_of(*node->value));
            }
        } else if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
            read_expr(*node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
            read_expr(*node->condition);
            walk_block(node->then_body);
            walk_block(node->else_body);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
            walk_block(node->body);
        } else if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
            read_expr(*node->condition);
            walk_block(node->body);
        } else if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
            read_expr(*node->iterable);
            scopes_.emplace_back();
            Binding binding;
            binding.key = &statement;
            if (node->writable) {
                binding.kind = Binding::Kind::Reference;
                auto places = places_of(*node->iterable);
                if (!places) places = Places{true, {}};
                Places elements;
                elements.unknown = places->unknown;
                for (const auto& place : places->places) {
                    elements.places.insert(
                        Place{place.path.child(PathStep::any_element()), place.shared_origin});
                }
                add_reference_places(&statement, elements);
            } else {
                binding.kind = Binding::Kind::Value;
                add_local_provenance(&statement, provenance_of(*node->iterable));
            }
            declare(node->name, binding);
            walk_block(node->body);
            scopes_.pop_back();
        } else if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
            read_expr(*node->value);
            const auto provenance = provenance_of(*node->value);
            for (const auto& match_case : node->cases) {
                scopes_.emplace_back();
                if (match_case.binder) {
                    Binding binding;
                    binding.kind = Binding::Kind::Value;
                    binding.key = &match_case;
                    declare(*match_case.binder, binding);
                    add_local_provenance(&match_case, provenance);
                }
                walk_block(match_case.body);
                scopes_.pop_back();
            }
        }
    }

    // ---- expressions ---------------------------------------------------------

    // Evaluates the operands inside a storage expression (the indices of
    // `values[i].x[j]`, a temporary base) without reading the storage itself.
    void read_storage_operands(const Expr& expression) {
        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            read_storage_operands(*index->base);
            for (const auto& item : index->items) {
                if (item.index) read_expr(*item.index);
                if (item.start) read_expr(*item.start);
                if (item.stop) read_expr(*item.stop);
                if (item.step) read_expr(*item.step);
            }
            return;
        }
        if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
            read_storage_operands(*member->base);
            return;
        }
        if (std::holds_alternative<NameExpr>(expression.data)) return;
        read_expr(expression);
    }

    void read_expr(const Expr& expression) {
        if (std::holds_alternative<NameExpr>(expression.data) ||
            std::holds_alternative<MemberExpr>(expression.data) ||
            std::holds_alternative<IndexExpr>(expression.data)) {
            read_storage_operands(expression);
            if (const auto places = places_of(expression)) record_read(*places);
            return;
        }
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) read_expr(*item);
        } else if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : node->elements) read_expr(*item);
        } else if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
            if (node->op == "&" || node->op == "scan&") {
                read_storage_operands(*node->operand);
                if (const auto places = places_of(*node->operand)) observe_address(*places);
                return;
            }
            read_expr(*node->operand);
        } else if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
            read_expr(*node->left);
            read_expr(*node->right);
        } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            read_expr(*node->value);
        } else if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
            for (const auto& item : node->conditions) read_expr(*item);
            for (const auto& item : node->values) read_expr(*item);
            read_expr(*node->otherwise);
        } else if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            walk_call(expression, *node);
        } else if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            walk_method_call(expression, *node);
        }
    }

    // Where the value of an expression comes from. Absent while a local it
    // copies has no provenance yet.
    std::optional<Provenance> provenance_of(const Expr& expression) {
        if (std::holds_alternative<NameExpr>(expression.data) ||
            std::holds_alternative<MemberExpr>(expression.data) ||
            std::holds_alternative<IndexExpr>(expression.data)) {
            const auto places = places_of(expression);
            if (!places) {
                if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
                    return provenance_of(*index->base);
                }
                if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
                    return provenance_of(*member->base);
                }
                return fresh();
            }
            if (places->unknown) return existing();
            std::optional<Provenance> provenance;
            for (const auto& place : places->places) {
                provenance = join(provenance, place_provenance(place));
            }
            return provenance;
        }
        if (const auto* node = std::get_if<TryExpr>(&expression.data)) return provenance_of(*node->value);
        if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
            auto provenance = provenance_of(*node->otherwise);
            for (const auto& item : node->values) provenance = join(provenance, provenance_of(*item));
            return provenance;
        }
        if (const auto* node = std::get_if<CallExpr>(&expression.data)) {
            const auto resolution = checked_.call_resolutions.find(&expression);
            if (resolution == checked_.call_resolutions.end()) return existing();
            if (resolution->second.kind == CallKind::FunctionValue) return existing();
            if (resolution->second.kind != CallKind::Function) return fresh();
            const bool enclosing = checked_.method_calls.contains(&expression);
            return call_result(resolution->second.target, node->args, enclosing ? 1 : 0,
                               enclosing ? receiver_provenance() : std::optional<Provenance>{});
        }
        if (const auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
            const auto method = checked_.method_calls.find(&expression);
            if (method == checked_.method_calls.end()) return fresh();
            return call_result(method->second.internal_name, node->args, 1, provenance_of(*node->receiver));
        }
        return fresh();
    }

    std::optional<Provenance> receiver_provenance() const {
        if (is_method()) return from_parameter(0);
        return fresh();
    }

    std::optional<Provenance> place_provenance(const Place& place) {
        if (place.shared_origin) return existing();
        switch (place.path.root.kind) {
        case RootKind::Parameter:
            return from_parameter(place.path.root.index);
        case RootKind::Local: {
            if (is_constructor() && place.path.root.name == "this") return fresh();
            const auto* binding = lookup(place.path.root.name);
            if (!binding) return existing();
            return local_provenance(binding->key);
        }
        case RootKind::Shared:
        case RootKind::Ambient:
        case RootKind::Resource:
            return existing();
        }
        return existing();
    }

    std::optional<Provenance> call_result(const std::string& callee, const std::vector<CallArg>& args,
                                          std::size_t offset,
                                          const std::optional<Provenance>& receiver) {
        const auto* summary = summaries_.find(callee);
        if (!summary) return std::nullopt;  // a callee of this component, not summarized yet
        if (summary->result.kind != Provenance::Kind::Parameter) return summary->result;
        const auto index = summary->result.parameter;
        if (offset == 1 && index == 0) return receiver ? receiver : existing();
        const auto bound = bind_arguments(callee, args, offset);
        if (index < bound.size() && bound[index]) return provenance_of(*bound[index]->value);
        return existing();
    }

    // The argument bound to each parameter of `callee` (named, then by
    // position after `offset`); null where the default applies.
    std::vector<const CallArg*> bind_arguments(const std::string& callee, const std::vector<CallArg>& args,
                                               std::size_t offset) const {
        const auto signature = checked_.functions.find(callee);
        if (signature == checked_.functions.end()) return {};
        const auto& parameters = signature->second.parameters;
        std::vector<const CallArg*> bound(parameters.size(), nullptr);
        std::size_t positional = offset;
        for (const auto& argument : args) {
            std::size_t target = parameters.size();
            if (argument.name) {
                for (std::size_t i = offset; i < parameters.size(); ++i) {
                    if (parameters[i].name == *argument.name) {
                        target = i;
                        break;
                    }
                }
            } else {
                while (positional < parameters.size() && bound[positional]) ++positional;
                target = positional++;
            }
            if (target < bound.size()) bound[target] = &argument;
        }
        return bound;
    }

    // ---- calls ---------------------------------------------------------------

    void walk_call(const Expr& expression, const CallExpr& node) {
        const auto resolution = checked_.call_resolutions.find(&expression);
        if (resolution == checked_.call_resolutions.end()) {
            for (const auto& argument : node.args) read_expr(*argument.value);
            return;
        }
        switch (resolution->second.kind) {
        case CallKind::Function: {
            const bool enclosing = checked_.method_calls.contains(&expression);
            std::optional<Places> receiver;
            std::optional<Provenance> receiver_origin;
            if (enclosing) {
                Places places;
                places.places.insert(Place{receiver_path(), std::nullopt});
                receiver = places;
                receiver_origin = receiver_provenance();
            }
            apply_call(resolution->second.target, expression, node.args, enclosing ? 1 : 0, nullptr, receiver,
                       receiver_origin);
            return;
        }
        case CallKind::FunctionValue:
            for (const auto& argument : node.args) read_expr(*argument.value);
            if (const auto places = places_of_callee(resolution->second.target)) record_read(*places);
            for (const auto& function : referenced_) apply_unknown_call(function, expression, &node.args);
            return;
        case CallKind::Constructor:
            for (const auto& argument : node.args) read_expr(*argument.value);
            if (resolution->second.type.kind == TypeKind::Class) {
                std::set<std::string> supplied;
                for (const auto& argument : node.args) {
                    if (argument.name) supplied.insert(*argument.name);
                }
                walk_class_defaults(resolution->second.target, supplied);
            }
            return;
        case CallKind::NumericCast:
            for (const auto& argument : node.args) read_expr(*argument.value);
            return;
        case CallKind::Builtin:
            walk_builtin(expression, node, resolution->second.builtin);
            return;
        }
    }

    std::optional<Places> places_of_callee(const std::string& name) {
        const auto* binding = lookup(name);
        if (!binding) return std::nullopt;
        Places places;
        if (binding->kind == Binding::Kind::Reference) return reference_places(binding->key);
        if (binding->kind == Binding::Kind::Value) {
            places.places.insert(Place{StoragePath{StorageRoot::local(name), {}}, std::nullopt});
        } else {
            places.places.insert(Place{StoragePath{StorageRoot::parameter(binding->parameter), {}}, std::nullopt});
        }
        return places;
    }

    // The arguments of a by-value parameter whose shared regions the callee
    // changes: the regions reachable from the argument's storage, or from
    // where a temporary came from.
    void shared_mutation_of_argument(const Expr& argument, SourceSpan span) {
        if (const auto places = places_of(argument)) {
            if (places->unknown) hidden_ambient(span, "runtime state");
            for (const auto& place : places->places) {
                shared_mutation(place.shared_origin ? *place.shared_origin : place.path, span);
            }
            return;
        }
        const auto provenance = provenance_of(argument);
        if (!provenance || provenance->kind == Provenance::Kind::Fresh) return;
        if (provenance->kind == Provenance::Kind::Parameter) {
            shared_mutation(StoragePath{StorageRoot::parameter(provenance->parameter), {}}, span);
            return;
        }
        hidden_ambient(span, "shared state of a value that already exists");
    }

    // Callee paths rooted at its parameter, carried onto the caller's places
    // for that argument.
    static Places carried(const Places& places, const StoragePath& callee_path) {
        Places result;
        result.unknown = places.unknown;
        for (const auto& place : places.places) {
            auto path = place.path.extended(callee_path.steps);
            // Recursion through fields could lengthen paths without end; a
            // prefix covers every path beneath it, so cutting is sound.
            if (path.steps.size() > max_path_steps) path.steps.resize(max_path_steps);
            result.places.insert(Place{std::move(path), place.shared_origin});
        }
        return result;
    }

    void carry_resources(const EffectSummary& callee_summary, const std::string& callee,
                         const std::vector<std::optional<Places>>& arguments) {
        for (const auto& access : callee_summary.resources) {
            if (access.origin != ResourceOrigin::Handle) {
                record_resource(summary_.resources, access);
                continue;
            }
            if (access.handle.root.kind == RootKind::Parameter && access.handle.root.index < arguments.size() &&
                arguments[access.handle.root.index] && !arguments[access.handle.root.index]->unknown) {
                for (const auto& place : carried(*arguments[access.handle.root.index], access.handle).places) {
                    record_resource(summary_.resources,
                                    ResourceAccess{access.kind, access.use, ResourceOrigin::Handle, place.path});
                }
                continue;
            }
            // A handle the callee made: distinct from every handle here.
            auto fresh_handle = access;
            fresh_handle.handle.root = StorageRoot::local(callee + ":" + describe(access.handle));
            record_resource(summary_.resources, std::move(fresh_handle));
        }
    }

    // A call of `callee`: its arguments evaluated in the order the lowering
    // evaluates them (as written, then the defaults of omitted parameters,
    // then a method's receiver expression), then the callee's summary
    // carried through them, and the borrowed arguments it must isolate.
    void apply_call(const std::string& callee, const Expr& expression, const std::vector<CallArg>& args,
                    std::size_t offset, const Expr* receiver_expression, std::optional<Places> receiver,
                    std::optional<Provenance> receiver_origin) {
        const auto signature = checked_.functions.find(callee);
        if (signature == checked_.functions.end()) {
            for (const auto& argument : args) read_expr(*argument.value);
            if (receiver_expression) read_expr(*receiver_expression);
            hidden_ambient_through(expression.span, callee);
            return;
        }
        const auto& parameters = signature->second.parameters;
        const auto bound = bind_arguments(callee, args, offset);
        std::vector<std::optional<Places>> arguments(parameters.size());
        // Where the write log stood once each argument was evaluated.
        std::vector<std::size_t> evaluated(parameters.size());
        std::vector<LoggedWrite> log;
        write_logs_.push_back(&log);
        for (const auto& argument : args) {
            std::size_t i = parameters.size();
            for (std::size_t j = offset; j < bound.size(); ++j) {
                if (bound[j] == &argument) i = j;
            }
            if (i == parameters.size()) {
                read_expr(*argument.value);
                continue;
            }
            if (parameters[i].writable) {
                read_storage_operands(*argument.value);
                auto places = places_of(*argument.value);
                if (!places) places = Places{true, {}};
                observe_address(*places);
                arguments[i] = places;
            } else {
                read_expr(*argument.value);
                arguments[i] = places_of(*argument.value);
            }
            evaluated[i] = log.size();
        }
        for (std::size_t i = offset; i < parameters.size(); ++i) {
            if ((i < bound.size() && bound[i]) || !parameters[i].default_value) continue;
            const auto floor = scope_floor_;
            scope_floor_ = scopes_.size();
            scopes_.emplace_back();
            read_expr(*parameters[i].default_value);
            scopes_.pop_back();
            scope_floor_ = floor;
        }
        if (receiver_expression) {
            receiver = places_of(*receiver_expression);
            if (receiver) {
                read_storage_operands(*receiver_expression);
            } else {
                read_expr(*receiver_expression);
                receiver_origin = provenance_of(*receiver_expression);
            }
        }
        write_logs_.pop_back();
        if (offset == 1 && !arguments.empty()) arguments[0] = receiver;

        const auto* callee_summary = summaries_.find(callee);
        if (!callee_summary) return;
        isolate_arguments(*callee_summary, parameters, bound, arguments, evaluated, log, offset);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const bool receiver_parameter = offset == 1 && i == 0;
            if (!receiver_parameter && !parameters[i].writable) continue;
            if (!arguments[i]) continue;  // a temporary receiver: its changes are local
            if (i < callee_summary->exposed_reads.size()) {
                for (const auto& path : callee_summary->exposed_reads[i]) record_read(carried(*arguments[i], path));
            }
            if (i < callee_summary->exposed_writes.size()) {
                for (const auto& path : callee_summary->exposed_writes[i]) {
                    record_write(carried(*arguments[i], path), expression.span);
                }
            }
        }
        for (std::size_t i = 0; i < callee_summary->mutates_shared_of_parameter.size() && i < parameters.size();
             ++i) {
            if (!callee_summary->mutates_shared_of_parameter[i]) continue;
            if (offset == 1 && i == 0) {
                if (receiver) {
                    for (const auto& place : receiver->places) {
                        shared_mutation(place.shared_origin ? *place.shared_origin : place.path,
                                        expression.span);
                    }
                } else if (receiver_origin && receiver_origin->kind == Provenance::Kind::Parameter) {
                    shared_mutation(StoragePath{StorageRoot::parameter(receiver_origin->parameter), {}},
                                    expression.span);
                } else if (receiver_origin && receiver_origin->kind == Provenance::Kind::Existing) {
                    hidden_ambient(expression.span, "shared state of a value that already exists");
                }
                continue;
            }
            const auto* argument = i < bound.size() ? bound[i] : nullptr;
            if (argument) shared_mutation_of_argument(*argument->value, expression.span);
        }
        if (callee_summary->mutates_ambient) hidden_ambient_through(expression.span, callee);
        summary_.shared_reads.insert(callee_summary->shared_reads.begin(), callee_summary->shared_reads.end());
        carry_resources(*callee_summary, callee, arguments);
    }

    // A call through a function value: any function used as a value may
    // run, with the call's by-value arguments.
    void apply_unknown_call(const std::string& callee, const Expr& expression, const std::vector<CallArg>* args) {
        const auto* callee_summary = summaries_.find(callee);
        if (!callee_summary) return;
        for (std::size_t i = 0; i < callee_summary->mutates_shared_of_parameter.size(); ++i) {
            if (!callee_summary->mutates_shared_of_parameter[i]) continue;
            if (args && i < args->size()) {
                shared_mutation_of_argument(*(*args)[i].value, expression.span);
            } else {
                hidden_ambient_through(expression.span, callee);
            }
        }
        if (callee_summary->mutates_ambient) hidden_ambient_through(expression.span, callee);
        summary_.shared_reads.insert(callee_summary->shared_reads.begin(), callee_summary->shared_reads.end());
        carry_resources(*callee_summary, callee, {});
    }

    void walk_builtin(const Expr& expression, const CallExpr& node, std::optional<BuiltinCallable> builtin) {
        for (const auto& argument : node.args) {
            if (argument.writable) {
                read_storage_operands(*argument.value);
                if (const auto places = places_of(*argument.value)) {
                    record_read(*places);
                    record_write(*places, expression.span);
                }
            } else {
                read_expr(*argument.value);
            }
        }
        if (!builtin) return;
        switch (*builtin) {
        case BuiltinCallable::Print:
        case BuiltinCallable::Flush:
            record_resource_use(ResourceKind::Stdout, ResourceUse::Exclusive);
            break;
        case BuiltinCallable::Scan:
            record_resource_use(ResourceKind::Stdin, ResourceUse::Exclusive);
            if (const auto format = checked_.scan_formats.find(&expression);
                format != checked_.scan_formats.end()) {
                for (const auto* target : format->second.targets) {
                    if (!target) continue;
                    read_storage_operands(*target);
                    if (const auto places = places_of(*target)) record_write(*places, expression.span);
                }
            }
            break;
        case BuiltinCallable::FileRead:
        case BuiltinCallable::FileReadBin:
        case BuiltinCallable::FileExists:
        case BuiltinCallable::FileIsDirectory:
        case BuiltinCallable::FileList:
        case BuiltinCallable::FileOpen:
            record_resource_use(ResourceKind::FileSystem, ResourceUse::Shared);
            break;
        case BuiltinCallable::FileWrite:
        case BuiltinCallable::FileWriteBin:
        case BuiltinCallable::FileRemove:
        case BuiltinCallable::FileCopy:
        case BuiltinCallable::FileMove:
        case BuiltinCallable::FileMkdir:
        case BuiltinCallable::FileCreate:
        case BuiltinCallable::FileAppend:
            record_resource_use(ResourceKind::FileSystem, ResourceUse::Exclusive);
            break;
        case BuiltinCallable::ProcessRun:
        case BuiltinCallable::ProcessShell:
            record_resource_use(ResourceKind::Process, ResourceUse::Exclusive);
            record_resource_use(ResourceKind::FileSystem, ResourceUse::Exclusive);
            record_resource_use(ResourceKind::Network, ResourceUse::Exclusive);
            record_resource_use(ResourceKind::Stdin, ResourceUse::Exclusive);
            break;
        case BuiltinCallable::HttpGet:
            record_resource_use(ResourceKind::Network, ResourceUse::Exclusive);
            break;
        case BuiltinCallable::TimeNow:
        case BuiltinCallable::TimeSince:
            record_resource_use(ResourceKind::Clock, ResourceUse::Exclusive);
            break;
        case BuiltinCallable::TimeSleep:
            record_resource_use(ResourceKind::Clock, ResourceUse::Shared);
            break;
        case BuiltinCallable::EnvironmentGet:
        case BuiltinCallable::EnvironmentHas:
            record_resource_use(ResourceKind::Environment, ResourceUse::Shared);
            break;
        case BuiltinCallable::RandomInt:
        case BuiltinCallable::RandomFloat:
        case BuiltinCallable::RandomBool: {
            // The generator's methods advance their receiver's state.
            Places places;
            places.places.insert(Place{receiver_path().child(PathStep::field_named("$state")), std::nullopt});
            record_read(places);
            record_write(places, expression.span);
            break;
        }
        case BuiltinCallable::TaskAll:
            for (const auto& function : referenced_) apply_unknown_call(function, expression, nullptr);
            break;
        default:
            break;
        }
    }

    void walk_method_call(const Expr& expression, const MethodCallExpr& node) {
        if (const auto method = checked_.method_calls.find(&expression); method != checked_.method_calls.end()) {
            apply_call(method->second.internal_name, expression, node.args, 1, node.receiver.get(), std::nullopt,
                       std::nullopt);
            return;
        }
        // A method of a built-in type.
        read_expr(*node.receiver);
        for (const auto& argument : node.args) {
            if (argument.writable) {
                read_storage_operands(*argument.value);
                if (const auto places = places_of(*argument.value)) {
                    record_read(*places);
                    record_write(*places, expression.span);
                    // backward writes the gradient state its targets share.
                    for (const auto& place : places->places) {
                        shared_mutation(place.shared_origin ? *place.shared_origin : place.path, expression.span);
                    }
                }
            } else {
                read_expr(*argument.value);
            }
        }
        const auto* receiver_type = type_of(*node.receiver);
        if (!receiver_type) return;
        const auto& method = node.method;
        if (receiver_type->kind == TypeKind::Tensor) {
            if (method == "track" || method == "clear_grad") {
                if (const auto places = places_of(*node.receiver)) record_write(*places, expression.span);
            }
            return;
        }
        if (receiver_type->kind != TypeKind::Class) return;
        const auto& class_name = receiver_type->class_name;
        const auto receiver_places = places_of(*node.receiver);
        const auto mutate_shared = [&] {
            if (receiver_places) {
                for (const auto& place : receiver_places->places) {
                    shared_mutation(place.shared_origin ? *place.shared_origin : place.path, expression.span);
                }
                if (receiver_places->unknown) hidden_ambient(expression.span, "runtime state");
            } else {
                shared_mutation_of_argument(*node.receiver, expression.span);
            }
        };
        const auto read_shared = [&] {
            const auto names = parameter_names();
            if (!receiver_places) return;
            for (const auto& place : receiver_places->places) {
                summary_.shared_reads.insert(
                    StoragePath{StorageRoot::shared(describe(place.shared_origin ? *place.shared_origin : place.path,
                                                             names)),
                                {}});
            }
        };
        if (class_name == standard_class::file_handle) {
            const bool reads = method == "read" || method == "read_line" || method == "read_bin" || method == "seek";
            const bool writes = method == "write" || method == "write_line" || method == "flush" || method == "close";
            if (!reads && !writes) return;
            const auto use = writes ? ResourceUse::Exclusive : ResourceUse::Shared;
            if (receiver_places && !receiver_places->unknown) {
                for (const auto& place : receiver_places->places) {
                    record_resource(summary_.resources,
                                    ResourceAccess{ResourceKind::FileSystem, use, ResourceOrigin::Handle,
                                                   place.shared_origin ? *place.shared_origin : place.path});
                }
            } else {
                record_resource(summary_.resources,
                                ResourceAccess{ResourceKind::FileSystem, use, ResourceOrigin::Handle,
                                               StoragePath{StorageRoot::local(
                                                               "$handle@" + std::to_string(expression.span.start.line) +
                                                               ":" + std::to_string(expression.span.start.column)),
                                                           {}}});
            }
            mutate_shared();
            return;
        }
        if (class_name == standard_class::atomic_counter) {
            if (method == "add") mutate_shared();
            read_shared();
            return;
        }
        if (class_name == standard_class::autograd_target) {
            if (method == "clear_grad") mutate_shared();
            read_shared();
            return;
        }
    }

    // Two places may be the same storage: overlapping paths, or two places
    // in shared regions, since copies of a cell share one.
    static bool may_alias(const Place& left, const Place& right) {
        if (left.shared_origin && right.shared_origin) return true;
        if (left.shared_origin || right.shared_origin) return false;
        return overlaps(left.path, right.path);
    }

    static bool may_alias(const Places& left, const Places& right) {
        if (left.unknown || right.unknown) return true;
        for (const auto& place : left.places) {
            for (const auto& other : right.places) {
                if (may_alias(place, other)) return true;
            }
        }
        return false;
    }

    // D12: a by-value argument that names storage is passed borrowed, so it
    // must keep its value while the call runs. It is isolated (passed as a
    // copy) when an & argument of the call may be the same storage,
    // when the callee may write it (its summary's writes carried through the
    // & arguments and the receiver), when an argument or the receiver
    // evaluated after it may write it, or when it lies in a shared region
    // and the callee may change shared state. Shared regions inside the
    // argument are not a reason: a copy would share them as well.
    void isolate_arguments(const EffectSummary& callee, const std::vector<FunctionParameterType>& parameters,
                           const std::vector<const CallArg*>& bound,
                           const std::vector<std::optional<Places>>& arguments,
                           const std::vector<std::size_t>& evaluated, const std::vector<LoggedWrite>& log,
                           std::size_t offset) {
        bool changes_shared = callee.mutates_ambient;
        for (const bool mutates : callee.mutates_shared_of_parameter) changes_shared = changes_shared || mutates;
        std::vector<Places> written;
        for (std::size_t k = 0; k < parameters.size(); ++k) {
            const bool receiver_parameter = offset == 1 && k == 0;
            if (!receiver_parameter && !parameters[k].writable) continue;
            if (!arguments[k]) continue;
            if (!receiver_parameter) written.push_back(*arguments[k]);
            if (k < callee.exposed_writes.size()) {
                for (const auto& path : callee.exposed_writes[k]) written.push_back(carried(*arguments[k], path));
            }
        }
        for (std::size_t j = offset; j < parameters.size(); ++j) {
            if (parameters[j].writable || j >= bound.size() || !bound[j] || !arguments[j]) continue;
            const auto& places = *arguments[j];
            bool isolate = places.unknown;
            for (const auto& other : written) isolate = isolate || may_alias(places, other);
            for (const auto& place : places.places) isolate = isolate || (place.shared_origin && changes_shared);
            for (std::size_t w = evaluated[j]; !isolate && w < log.size(); ++w) {
                if (log[w].unknown) {
                    isolate = true;
                    break;
                }
                for (const auto& place : places.places) {
                    if (may_alias(place, log[w].place)) {
                        isolate = true;
                        break;
                    }
                }
            }
            if (isolate) isolated_.insert(bound[j]->value.get());
        }
    }

    const CheckedProgram& checked_;
    const EffectSummaries& summaries_;
    const std::set<std::string>& referenced_;
    const Body& body_;
    std::set<const Expr*>& isolated_;
    std::vector<std::vector<LoggedWrite>*> write_logs_;

    EffectSummary summary_;
    std::optional<Provenance> result_;
    std::vector<std::map<std::string, Binding>> scopes_;
    std::size_t scope_floor_{};
    std::map<const void*, Places> reference_targets_;
    std::map<const void*, Provenance> provenance_;
    std::set<const void*> read_keys_;
    bool grew_after_read_{};
};

EffectSummary external_summary(const FunctionType& signature) {
    EffectSummary summary;
    const auto count = signature.parameters.size();
    summary.exposed_reads.assign(count, {});
    summary.exposed_writes.assign(count, {});
    summary.mutates_shared_of_parameter.assign(count, false);
    summary.const_parameter_address_observed.assign(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        if (!signature.parameters[i].writable) continue;
        const StoragePath whole{StorageRoot::parameter(i), {}};
        summary.exposed_reads[i].insert(whole);
        if (!signature.parameters[i].is_const) summary.exposed_writes[i].insert(whole);
    }
    // Foreign code is opaque: its results are not known to be fresh.
    summary.result = existing();
    return summary;
}

// The summary that claims every effect a body could have: what a component
// that did not reach its fixed point gets, so that no effect is lost.
EffectSummary everything(const Body& body) {
    EffectSummary summary;
    const auto count = body.signature->parameters.size();
    summary.exposed_reads.assign(count, {});
    summary.exposed_writes.assign(count, {});
    summary.mutates_shared_of_parameter.assign(count, true);
    summary.const_parameter_address_observed.assign(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        const bool exposed = body.signature->parameters[i].writable ||
                             (body.kind == Body::Kind::Method && i == 0);
        if (!exposed) continue;
        const StoragePath whole{StorageRoot::parameter(i), {}};
        summary.exposed_reads[i].insert(whole);
        summary.exposed_writes[i].insert(whole);
    }
    summary.mutates_receiver = body.kind == Body::Kind::Method;
    summary.mutates_ambient = true;
    summary.result = existing();
    if (body.declaration) summary.first_hidden_mutation = EffectCause{body.declaration->span, "runtime state"};
    return summary;
}

void collect_function_references(const CheckedProgram& checked, std::set<std::string>& referenced) {
    for (const auto& [expression, name] : checked.function_references) {
        (void)expression;
        referenced.insert(name);
    }
}

} // namespace

EffectSummaries summarize_effects(const CheckedProgram& checked) {
    std::map<std::string, Body> bodies;
    for (const auto& function : checked.program.functions) {
        const auto signature = checked.functions.find(function.name);
        if (signature == checked.functions.end()) continue;
        Body body;
        body.name = function.name;
        body.declaration = &function;
        body.signature = &signature->second;
        body.kind = function.external_symbol ? Body::Kind::External : Body::Kind::Function;
        bodies.emplace(function.name, std::move(body));
    }
    for (const auto& class_decl : checked.program.classes) {
        std::size_t constructors = 0;
        for (const auto& method : class_decl.methods) {
            const auto name = method.is_constructor
                                  ? member_function_name::constructor(class_decl.name, constructors++)
                                  : member_function_name::method(class_decl.name, method.name);
            const auto signature = checked.functions.find(name);
            if (signature == checked.functions.end()) continue;
            Body body;
            body.name = name;
            body.declaration = &method;
            body.signature = &signature->second;
            body.kind = method.is_constructor ? Body::Kind::Constructor : Body::Kind::Method;
            body.class_name = class_decl.name;
            bodies.emplace(name, std::move(body));
        }
    }

    std::set<std::string> referenced;
    collect_function_references(checked, referenced);

    EffectSummaries summaries;
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>> edges;
    for (const auto& [name, body] : bodies) {
        if (body.kind == Body::Kind::External) {
            summaries.set(name, external_summary(*body.signature));
            continue;
        }
        names.push_back(name);
        std::set<std::string> callees;
        CalleeCollector collector(checked, referenced, callees);
        if (body.kind == Body::Kind::Constructor) collector.class_defaults(body.class_name);
        collector.block(body.declaration->body);
        auto& out = edges[name];
        for (const auto& callee : callees) {
            const auto found = bodies.find(callee);
            if (found != bodies.end() && found->second.kind != Body::Kind::External) out.push_back(callee);
        }
    }

    // Tarjan's strongly connected components; a component is complete only
    // after every component it calls, so the components come out bottom-up.
    std::map<std::string, std::size_t> index_of, low;
    std::set<std::string> on_stack;
    std::vector<std::string> stack;
    std::vector<std::vector<std::string>> components;
    std::size_t next_index = 0;
    std::function<void(const std::string&)> connect = [&](const std::string& name) {
        index_of[name] = low[name] = next_index++;
        stack.push_back(name);
        on_stack.insert(name);
        for (const auto& callee : edges[name]) {
            if (!index_of.contains(callee)) {
                connect(callee);
                low[name] = std::min(low[name], low[callee]);
            } else if (on_stack.contains(callee)) {
                low[name] = std::min(low[name], index_of[callee]);
            }
        }
        if (low[name] != index_of[name]) return;
        std::vector<std::string> component;
        while (true) {
            auto member = stack.back();
            stack.pop_back();
            on_stack.erase(member);
            component.push_back(member);
            if (member == name) break;
        }
        std::sort(component.begin(), component.end());
        components.push_back(std::move(component));
    };
    for (const auto& name : names) {
        if (!index_of.contains(name)) connect(name);
    }

    // Every walk adds the arguments it isolates. Summaries only grow while a
    // component iterates, and so do the arguments isolated with them, so the
    // union over the rounds is what the final summaries isolate.
    std::set<const Expr*> isolated;
    for (const auto& component : components) {
        const bool recursive = component.size() > 1 ||
                               std::find(edges[component.front()].begin(), edges[component.front()].end(),
                                         component.front()) != edges[component.front()].end();
        bool converged = !recursive;
        for (int round = 0; round < 64; ++round) {
            bool changed = false;
            for (const auto& name : component) {
                BodyWalker walker(checked, summaries, referenced, bodies.at(name), isolated);
                auto summary = walker.summarize();
                const auto* previous = summaries.find(name);
                if (!previous || !(*previous == summary)) {
                    summaries.set(name, std::move(summary));
                    changed = true;
                }
            }
            if (!recursive) break;
            if (!changed) {
                converged = true;
                break;
            }
        }
        if (!converged) {
            for (const auto& name : component) summaries.set(name, everything(bodies.at(name)));
            for (const auto& name : component) {
                BodyWalker walker(checked, summaries, referenced, bodies.at(name), isolated);
                (void)walker.summarize();
            }
        }
    }
    Body entry;
    entry.name = "$entry";
    entry.kind = Body::Kind::Entry;
    entry.statements = &checked.program.statements;
    BodyWalker walker(checked, summaries, referenced, entry, isolated);
    (void)walker.summarize();
    summaries.set_isolated(std::move(isolated));
    return summaries;
}

} // namespace quidra::semantics
