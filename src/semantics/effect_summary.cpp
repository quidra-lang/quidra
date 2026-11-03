// Effect summaries (effect_summary.hpp). A body is walked with the storage
// its names denote: locals of the invocation, parameters by index (a
// method's receiver is 0), and, through reference fields and the runtime
// handles, shared regions. Every read and write of a path rooted at an
// exposed parameter (an & parameter's target, a method's receiver) enters
// the summary; writes to locals and to by-value parameters are writes to
// copies and stay local. A call applies its callee's summary through the
// call's arguments, so summaries compose; summarize_effects orders the
// bodies bottom-up over the strongly connected components of the call graph
// and iterates each component to a fixed point.

#include "semantics/effect_summary.hpp"
#include "semantics/parallel_effects.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/standard_classes.hpp"
#include <algorithm>
#include <functional>
#include <tuple>
#include <utility>
#include <variant>

namespace quidra::semantics {

const ParallelBlockPlan* EffectSummaries::parallel_block(const Stmt& statement) const {
    const auto found=parallel_blocks_.find(&statement);
    return found==parallel_blocks_.end()?nullptr:found->second.get();
}

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
           left.result == right.result && left.first_hidden_mutation == right.first_hidden_mutation &&
           left.reference_target_escapes == right.reference_target_escapes && left.escaping_locals == right.escaping_locals && left.reference_links == right.reference_links &&
           left.reference_root_parameters == right.reference_root_parameters && left.pinned_complete_targets == right.pinned_complete_targets && left.reference_shared_writes == right.reference_shared_writes && left.reference_shared_reads == right.reference_shared_reads &&
           left.reference_shared_read_origins == right.reference_shared_read_origins && left.reference_shared_write_origins == right.reference_shared_write_origins;
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

template<class Field> bool reference_field(const Field& field) {
    if constexpr(requires{field.reference;}) return field.reference;
    else return false;
}

bool is_reference_member(const CheckedProgram& checked,const Type& type,const std::string& name) {
    if(type.kind!=TypeKind::Class)return false;
    const auto info=checked.classes.find(type.class_name);
    if(info==checked.classes.end())return false;
    for(const auto& field:info->second.fields)if(field.name==name)return reference_field(field);
    return false;
}

// The runtime handles whose state every copy shares and some method
// changes: a file handle's position and open state.
bool is_shared_handle(const Type& type) {
    return type.kind == TypeKind::Class &&
           (type.class_name == standard_class::file_handle ||
             type.class_name == standard_class::file_read ||
             type.class_name == standard_class::file_write ||
             type.class_name == standard_class::file_append);
}

bool shared_region_in(const CheckedProgram& checked, const Type& type,
                      std::set<std::string>& visiting) {
    if (is_shared_handle(type)) return true;
    if (type.kind == TypeKind::Array || type.kind == TypeKind::Tensor) {
        return type.first && shared_region_in(checked, *type.first, visiting);
    }
    if (type.kind == TypeKind::Tuple) {
        for(const auto& child:type.parameters)if(shared_region_in(checked, child, visiting))return true;
        return false;
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
                           return reference_field(field) || shared_region_in(checked, field.type, visiting);
                       });
}

} // namespace

bool contains_shared_region(const CheckedProgram& checked, const Type& type) {
    std::set<std::string> visiting;
    return shared_region_in(checked, type, visiting);
}

bool reference_target_compatible(const CheckedProgram& checked,const Type& left,const Type& right) {
    const auto contains=[&](const Type& container,const Type& value) {
        std::set<std::string> seen;
        const auto visit=[&](const auto& self,const Type& type)->bool {
            if(type==value)return true;
            if(type.kind==value.kind && (type.kind==TypeKind::Tensor || type.kind==TypeKind::Array) && type.first && value.first && *type.first==*value.first)return true;
            if(type.first && self(self,*type.first))return true;
            for(const auto& child:type.parameters)if(self(self,child))return true;
            for(const auto& child:type.cases)if(self(self,child))return true;
            if(type.kind==TypeKind::Class && seen.insert(type.class_name).second)
                if(const auto info=checked.classes.find(type.class_name);info!=checked.classes.end())
                    for(const auto& field:info->second.fields)if(!field.reference && self(self,field.type))return true;
            return false;
        };
        return visit(visit,container);
    };
    return contains(left,right) || contains(right,left);
}

const EffectSummary* reference_write_summary(const CheckedProgram& checked,const Expr& call) {
    if(!checked.effects)return nullptr;
    std::string target;
    if(const auto method=checked.method_calls.find(&call);method!=checked.method_calls.end())target=method->second.internal_name;
    else if(const auto function=checked.call_resolutions.find(&call);function!=checked.call_resolutions.end())target=function->second.target;
    return checked.effects->find(target);
}

bool reference_call_writes(const CheckedProgram& checked,const Expr& call,const Expr& target) {
    if(!checked.effects || !checked.effects->reference_target(target))return false;
    const auto type=checked.expr_types.find(&target);if(type==checked.expr_types.end())return false;
    if(const auto* writes=checked.effects->reference_call(call)) {
        for(const auto& [_,written]:writes->shared)if(reference_target_compatible(checked,type->second,written))return true;
        if(const auto* paths=checked.effects->reference_target_paths(target))
            for(const auto& [written,_]:writes->resolved)if(overlaps_any(*paths,written))return true;
        return false;
    }
    const auto* summary=reference_write_summary(checked,call);
    if(!summary)return false;
    return std::any_of(summary->reference_shared_writes.begin(),summary->reference_shared_writes.end(),[&](const auto& written){return reference_target_compatible(checked,type->second,written.second);});
}

namespace {

using Provenance = ResultProvenance;

// The resource kind of a built-in's resource (quidra/language.hpp), which
// lists them in ResourceKind's order.
ResourceKind resource_kind(BuiltinResource resource) {
    static_assert(static_cast<int>(BuiltinResource::Stdout) == static_cast<int>(ResourceKind::Stdout) &&
                  static_cast<int>(BuiltinResource::Stdin) == static_cast<int>(ResourceKind::Stdin) &&
                  static_cast<int>(BuiltinResource::Stderr) == static_cast<int>(ResourceKind::Stderr) &&
                  static_cast<int>(BuiltinResource::FileSystem) == static_cast<int>(ResourceKind::FileSystem) &&
                  static_cast<int>(BuiltinResource::Process) == static_cast<int>(ResourceKind::Process) &&
                  static_cast<int>(BuiltinResource::Network) == static_cast<int>(ResourceKind::Network) &&
                  static_cast<int>(BuiltinResource::Clock) == static_cast<int>(ResourceKind::Clock) &&
                  static_cast<int>(BuiltinResource::Environment) ==
                      static_cast<int>(ResourceKind::Environment));
    return static_cast<ResourceKind>(static_cast<std::uint8_t>(resource));
}

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
    std::optional<Type> reference_type;

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
    bool shape_variable{};
    const void* key{};
    std::optional<Type> value_type{};
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

// The constructor a construction runs: the instance the frontend selected,
// or the class's sole constructor. Empty for a class without one.
std::string constructor_of(const CheckedProgram& checked, const std::string& class_name, const CallExpr& node) {
    const auto info = checked.classes.find(class_name);
    if (info == checked.classes.end()) return {};
    if (!node.constructor.empty()) {
        const auto instance = info->second.constructor_instances.find(node.constructor);
        return instance == info->second.constructor_instances.end() ? std::string{} : instance->second;
    }
    return info->second.constructors.size() == 1 ? info->second.constructors.front() : std::string{};
}

// Applies `visit` to every call target an expression or statement may reach
// directly: functions, methods, constructors, the functions that indirect
// calls may run, and the field defaults a construction or a
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
            if(const auto found=checked_.shape_plans.find(&node->declared_type);
               found!=checked_.shape_plans.end()) {
                for(const auto& axis:found->second.axes)if(axis.capture)expr(*axis.capture);
            }else {
                for(const auto& dimension:node->declared_type.tensor_shape_expressions)
                    if(dimension)expr(*dimension);
                for(const auto& dimension:node->declared_type.dimension_expressions)
                    if(dimension)expr(*dimension);
            }
            if (node->value) expr(*node->value);
            else if (const auto type = checked_.binding_types.find(&statement);
                     type != checked_.binding_types.end() && type->second.kind == TypeKind::Class) {
                class_defaults(type->second.class_name);
            }
        } else if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
            if(node->pattern)visit_pattern(*node->pattern,[&](const Pattern& p){if(p.place)expr(*p.place);});
            else expr(*node->target_expr());
            expr(*node->value);
        } else if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
            if(node->place)expr(*node->place);
            expr(*node->target);
        } else if (const auto* node = std::get_if<ReturnStmt>(&statement.data)) {
            if (node->value) expr(*node->value);
        } else if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
            expr(*node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
            expr(*node->condition);
            block(node->then_body);
            block(node->else_body);
        } else if (const auto* node = std::get_if<ParallelStmt>(&statement.data)) {
            block(node->body);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
            block(node->body);
        } else if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
            expr(*node->condition);
            block(node->body);
        } else if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
            visit_for_header(*node,[&](const Expr& operand) { expr(operand); });
            block(node->body);
        } else if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
            expr(*node->value);
            for (const auto& match_case : node->cases) block(match_case.body);
        }
    }

    void expr(const Expr& expression) {
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) expr(*item);
        } else if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
            for (const auto& item : node->elements) expr(*item);
        } else if (std::holds_alternative<TupleProjectionExpr>(expression.data)) {
            // A projection names the already evaluated pattern subject;
            // it has no expression operand or call target to visit again.
            return;
        } else if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
            visit_array_comprehension(*node, [&](const Expr& child) { expr(child); });
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
        } else if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
            for (const auto& item : node->operands) expr(*item);
        } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            expr(*node->value);
        } else if (const auto* node = std::get_if<LiteralConstantExpr>(&expression.data)) {
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
                if (const auto constructor = constructor_of(checked_, resolution->second.target, node);
                    !constructor.empty())
                    target(constructor);
            }
            break;
        case CallKind::Builtin:

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
static_assert(std::variant_size_v<Expr::Data> == 23, "effect summaries: walk the new expression kind");
static_assert(std::variant_size_v<Stmt::Data> == 12, "effect summaries: walk the new statement kind");

// Walks one body and computes its summary from the current summaries of its
// callees.
class BodyWalker {
public:
    BodyWalker(const CheckedProgram& checked, const EffectSummaries& summaries,
               const std::set<std::string>& referenced, const std::set<std::string>& quiet_methods,
               const Body& body, std::set<const Expr*>& isolated,
               ParallelBlockPlans* parallel_plans=nullptr, bool validate_references=false,std::set<const Expr*>* reference_expressions=nullptr,
               std::map<const Expr*,PathSet>* reference_paths=nullptr,std::map<const Expr*,ReferenceCallWrites>* reference_calls=nullptr)
        : checked_(checked), summaries_(summaries), referenced_(referenced),
          quiet_methods_(quiet_methods), body_(body), isolated_(isolated), parallel_plans_(parallel_plans), validate_references_(validate_references),reference_expressions_(reference_expressions),reference_paths_(reference_paths),reference_calls_(reference_calls) {}

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
            summary_.reference_target_escapes.assign(count, false);
            summary_.reference_root_parameters.assign(count, false);
            conditional_depth_ = 0;
            saw_exit_ = false;
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

    void record_reference_escape(const Places& places, SourceSpan site) {
        if (validate_references_ && places.unknown) reference_error(site, "A reference field needs a known stable target.");
        for (const auto& place : places.places) {
            const auto& root = place.path.root;
            summary_.pinned_complete_targets.insert(place.path);
            if (validate_references_) {
                const auto stable_steps = [](const StoragePath& path) {
                    return std::all_of(path.steps.begin(), path.steps.end(), [](const auto& step) { return step.kind == StepKind::Field; });
                };
                if (!stable_steps(place.path) || (place.shared_origin && !stable_steps(*place.shared_origin)) ||
                    (is_constructor() && root.kind == RootKind::Local && root.name == "this"))
                    reference_error(site, "A reference field may target only a whole variable or a field path through class values.");
            }
            if (root.kind == RootKind::Local) summary_.escaping_locals.insert(root.name);
            if (root.kind == RootKind::Parameter && body_.signature && root.index < body_.signature->parameters.size()) {
                const auto& parameter = body_.signature->parameters[root.index];
                if (parameter.writable || (is_method() && root.index == 0)) summary_.reference_target_escapes[root.index] = summary_.reference_root_parameters[root.index] = true;
                else summary_.escaping_locals.insert(parameter.name);
            }
        }
    }

    [[noreturn]] void reference_error(SourceSpan site, std::string text) const {
        throw CompileError(Diagnostic{"REFERENCE_TARGET", std::move(text), site});
    }

    void record_link(const StoragePath& holder, const StoragePath& target, bool must, SourceSpan site) {
        const bool local_holder = holder.root.kind == RootKind::Local && holder.root.name != "$result" && holder.root.name != "this";
        const bool local_target = target.root.kind == RootKind::Local && target.root.name != "$result" && target.root.name != "this";
        if (validate_references_ && must && local_holder && local_target && holder.root == target.root)
            reference_error(site, "Interior self-reference: '" + describe(holder, parameter_names()) + "' would refer into the value that holds it.");
        if(holder.root.kind==RootKind::Local)if(const auto* binding=lookup(holder.root.name)) {
            Places targets;targets.places.insert(Place{target,std::nullopt,std::nullopt});
            add_held_targets(binding->key,targets);
        }
        ReferenceLink link{holder,target,must,site};
        if (std::find(summary_.reference_links.begin(), summary_.reference_links.end(), link) == summary_.reference_links.end())
            summary_.reference_links.push_back(std::move(link));
        for (const auto* path : {&holder,&target})
            if (path->root.kind == RootKind::Parameter && body_.signature && path->root.index < body_.signature->parameters.size() &&
                (body_.signature->parameters[path->root.index].writable || (is_method() && path->root.index == 0)))
                summary_.reference_root_parameters[path->root.index] = true;
    }

    void validate_complete_store(const Expr& target,const Places& places,SourceSpan site) {
        if(!validate_references_)return;
        const auto invalid=checked_.reference_invalidations.find(&target);if(invalid==checked_.reference_invalidations.end() || invalid->second.empty())return;
        const auto* final=summaries_.find(body_.name);
        for(const auto& place:places.places) {
            const auto pinned=[&](const PathSet& paths){return std::any_of(paths.begin(),paths.end(),[&](const auto& path){return overlaps(path,place.path);});};
            if(place.shared_origin || pinned(summary_.pinned_complete_targets) || (final && pinned(final->pinned_complete_targets)))
                reference_error(site,"'"+describe(place.path,parameter_names())+"' is bound by a reference field and must stay fully initialized; this store may leave a field uninitialized.");
        }
    }

    Places targets_of(const Expr& expression) {
        if (const auto* name = std::get_if<NameExpr>(&expression.data); name && !name->this_qualifier && !name->self_reference) {
            if (const auto* binding = lookup(name->name)) {
                read_keys_.insert(binding->key);
                if (const auto found = held_targets_.find(binding->key); found != held_targets_.end()) return found->second;
            }
        }
        read_keys_.insert(&expression);
        if (const auto found = held_targets_.find(&expression); found != held_targets_.end()) return found->second;
        Places result;
        const auto add = [&](const Expr& value) { auto targets=targets_of(value);result.unknown |= targets.unknown;result.places.insert(targets.places.begin(),targets.places.end()); };
        if (const auto* tuple = std::get_if<TupleExpr>(&expression.data)) for (const auto& value:tuple->elements) add(*value);
        else if (const auto* array = std::get_if<ArrayExpr>(&expression.data)) for (const auto& value:array->elements) add(*value);
        else if (const auto* generation = std::get_if<ArrayComprehensionExpr>(&expression.data)) add(*generation->value);
        else if (const auto* member = std::get_if<MemberExpr>(&expression.data)) add(*member->base);
        else if (const auto* index = std::get_if<IndexExpr>(&expression.data)) add(*index->base);
        else if (const auto* tried = std::get_if<TryExpr>(&expression.data)) add(*tried->value);
        return result;
    }

    void add_held_targets(const void* key, const Places& places) {
        auto& current=held_targets_[key];
        bool grew=places.unknown && !current.unknown;
        current.unknown |= places.unknown;
        for(const auto& place:places.places)grew=current.places.insert(place).second || grew;
        if(grew && read_keys_.contains(key))grew_after_read_=true;
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
        if(const auto* projection=std::get_if<TupleProjectionExpr>(&expression.data)) {
            auto base=places_of_callee(projection->subject);
            if(!base)return std::nullopt;
            Places result;result.unknown=base->unknown;for(auto place:base->places){for(auto index:projection->indices)place.path=place.path.child(PathStep::tuple_element(index));result.places.insert(std::move(place));}return result;
        }
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (name->self_reference) {
                Places places;
                places.places.insert(Place{receiver_path(), std::nullopt, std::nullopt});
                return places;
            }
            if (name->this_qualifier) {
                Places places;
                const auto path=receiver_path().child(PathStep::field_named(name->name));
                if(is_reference_member(checked_,Type::class_type(body_.class_name),name->name)) {
                    const auto type=type_of(expression);
                    places.places.insert(Place{StoragePath{StorageRoot::shared(describe(receiver_path(),parameter_names())),{PathStep::field_named(name->name)}},receiver_path(),type?std::optional<Type>(*type):std::nullopt});
                } else places.places.insert(Place{path,std::nullopt,std::nullopt});
                return places;
            }
            const auto* binding = lookup(name->name);
            if (!binding) return std::nullopt;
            Places places;
            switch (binding->kind) {
            case Binding::Kind::Value:
                places.places.insert(Place{StoragePath{StorageRoot::local(name->name), {}}, std::nullopt, std::nullopt});
                return places;
            case Binding::Kind::ValueParameter:
            case Binding::Kind::ReferenceParameter:
                places.places.insert(
                    Place{StoragePath{StorageRoot::parameter(binding->parameter), {}}, std::nullopt, std::nullopt});
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
            const bool enters_reference = base_type && is_reference_member(checked_, *base_type, member->name);
            const auto names = enters_reference ? parameter_names() : std::vector<std::string>{};
            Places places;
            places.unknown = base->unknown;
            for (const auto& place : base->places) {
                if (enters_reference) {
                    places.places.insert(Place{
                        StoragePath{StorageRoot::shared(describe(place.path, names)),
                                    {PathStep::field_named(member->name)}},
                        place.path,type_of(expression)?std::optional<Type>(*type_of(expression)):std::nullopt});
                } else {
                    places.places.insert(
                        Place{place.path.child(PathStep::field_named(member->name)), place.shared_origin,place.reference_type});
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
                places.places.insert(Place{place.path.extended(steps), place.shared_origin,place.reference_type});
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
        record_node_ambient();
        note_hidden(span, [&] { return std::string(what); });
    }

    void hidden_ambient_through(SourceSpan span, const std::string& callee) {
        summary_.mutates_ambient = true;
        record_node_ambient();
        note_hidden(span, [&] { return "runtime state through '" + callee + "'"; });
    }

    void record_node_unknown(bool writes) {
        for(auto* node:active_nodes_) {
            for(const auto& scope:scopes_) for(const auto& [name,binding]:scope) {
                const auto path=binding.kind==Binding::Kind::ValueParameter ||
                                binding.kind==Binding::Kind::ReferenceParameter
                    ?StoragePath{StorageRoot::parameter(binding.parameter),{}}
                    :StoragePath{StorageRoot::local(name),{}};
                (writes?node->writes:node->reads).insert(path);
            }
        }
    }
    bool reference_candidate(const Place& place) const {
        const auto* final=summaries_.find(body_.name);
        return place.shared_origin || (place.path.root.kind==RootKind::Parameter && exposed(place.path.root.index)) ||
            overlaps_any(summary_.pinned_complete_targets,place.path) || (final && overlaps_any(final->pinned_complete_targets,place.path));
    }

    std::optional<Type> place_type(const Place& place) const {
        Type type=Type::simple(TypeKind::Invalid);
        std::size_t first=0;
        if(place.reference_type) {type=*place.reference_type;first=place.path.steps.empty()?0:1;}
        else if(place.path.root.kind==RootKind::Parameter && body_.signature && place.path.root.index<body_.signature->parameters.size())type=body_.signature->parameters[place.path.root.index].type;
        else if(place.path.root.kind==RootKind::Local) {
            if(place.path.root.name=="this")type=Type::class_type(body_.class_name);
            else if(const auto* binding=lookup(place.path.root.name)) {
                if(binding->value_type) type=*binding->value_type;
                else if(const auto found=checked_.binding_types.find(static_cast<const Stmt*>(binding->key));found!=checked_.binding_types.end())type=found->second;
                else if(const auto found=checked_.pattern_subject_types.find(place.path.root.name);found!=checked_.pattern_subject_types.end())type=found->second;
            }
        }
        for(std::size_t i=first;i<place.path.steps.size();++i) {
            const auto& step=place.path.steps[i];
            if(step.kind==StepKind::Field && type.kind==TypeKind::Class) {
                const auto info=checked_.classes.find(type.class_name);if(info==checked_.classes.end())return std::nullopt;
                const auto field=std::find_if(info->second.fields.begin(),info->second.fields.end(),[&](const auto& field){return field.name==step.field;});
                if(field==info->second.fields.end())return std::nullopt;
                type=field->type;
            } else if(step.kind==StepKind::TupleElement && type.kind==TypeKind::Tuple && step.index>=0 && static_cast<std::size_t>(step.index)<type.parameters.size()) {
                auto child=type.parameters[static_cast<std::size_t>(step.index)];
                type=std::move(child);
            } else if((step.kind==StepKind::Element || step.kind==StepKind::AnyElement) && type.first) {
                auto child=*type.first;
                type=std::move(child);
            }
            else return std::nullopt;
        }
        if(type.kind==TypeKind::Invalid)return std::nullopt;
        return type;
    }

    std::optional<Places> resolved_reference_targets(const Place& place) {
        if(!place.reference_type || !place.shared_origin || place.shared_origin->root.kind!=RootKind::Local)return std::nullopt;
        const auto* binding=lookup(place.shared_origin->root.name);
        if(!binding)return std::nullopt;
        read_keys_.insert(binding->key);
        const auto held=held_targets_.find(binding->key);
        if(held!=held_targets_.end() && !held->second.unknown && !held->second.places.empty()) {
            Places targets;
            for(auto target:held->second.places) {
                if(!place.path.steps.empty())target.path=target.path.extended(std::vector<PathStep>(place.path.steps.begin()+1,place.path.steps.end()));
                targets.places.insert(std::move(target));
            }
            return targets;
        }
        const auto origin=local_provenance(binding->key);
        if((held==held_targets_.end() || !held->second.unknown) && origin && origin->kind==Provenance::Kind::Fresh)return Places{};
        return std::nullopt;
    }

    void record_reference_region(const Place& place,bool writes) {
        if(!place.reference_type || !place.shared_origin)return;
        if(const auto targets=resolved_reference_targets(place)) {
            for(const auto& target:targets->places)if(const auto type=place_type(target)) {
                if(target.path.root.kind==RootKind::Parameter && exposed(target.path.root.index))
                    (writes?summary_.exposed_writes[target.path.root.index]:summary_.exposed_reads[target.path.root.index]).insert(target.path);
                for(auto* node:active_nodes_)(writes?node->reference_resolved_writes:node->reference_resolved_reads).insert_or_assign(target.path,*type);
                if(writes && reference_call_)call_writes_[reference_call_].resolved.insert_or_assign(target.path,*type);
            }
            return;
        }
        const auto& type=*place.reference_type;
        auto& regions=writes?summary_.reference_shared_writes:summary_.reference_shared_reads;
        regions.emplace(type_name(type),type);
        if(writes && reference_call_)call_writes_[reference_call_].shared.emplace(type_name(type),type);
        (writes?summary_.reference_shared_write_origins:summary_.reference_shared_read_origins).insert_or_assign(*place.shared_origin,type);
        for(auto* node:active_nodes_)(writes?node->reference_shared_writes:node->reference_shared_reads).emplace(type_name(type),type);
    }

    void record_node_places(const Places& places,bool writes) {
        if(places.unknown) record_node_unknown(writes);
        for(const auto& place:places.places) {
            for(auto* node:active_nodes_) {
                if(!place.shared_origin && reference_candidate(place))if(const auto type=place_type(place))
                    (writes?node->reference_target_writes:node->reference_target_reads).insert_or_assign(place.path,*type);
                if(place.shared_origin) {
                    if(place.reference_type)continue;
                    node->shared.push_back(ParallelSharedAccess{*place.shared_origin,writes,
                        fresh_region_of(*place.shared_origin)});
                } else (writes?node->writes:node->reads).insert(place.path);
            }
        }
    }
    std::uint64_t fresh_region_of(const StoragePath& origin) const {
        if(origin.root.kind!=RootKind::Local) return 0;
        const auto* binding=lookup(origin.root.name);
        if(!binding) return 0;
        const auto found=fresh_shared_.find(binding->key);
        return found==fresh_shared_.end()?0:found->second;
    }
    std::uint64_t fresh_region_of(const Expr& expression) {
        if(const auto places=places_of(expression)) {
            if(places->unknown || places->places.size()!=1) return 0;
            const auto& place=*places->places.begin();
            return fresh_region_of(place.shared_origin?*place.shared_origin:place.path);
        }
        if(const auto* node=std::get_if<TryExpr>(&expression.data))
            return fresh_region_of(*node->value);
        const auto origin=provenance_of(expression);
        if(!origin || origin->kind!=Provenance::Kind::Fresh) return 0;
        auto [it,added]=fresh_expressions_.try_emplace(&expression,next_fresh_region_);
        if(added) ++next_fresh_region_;
        return it->second;
    }
    void set_fresh_region(const void* key,std::uint64_t identity) {
        const auto [it,added]=fresh_shared_.try_emplace(key,identity);
        if(!added && it->second!=identity) it->second=0;
    }
    void record_node_ambient() {
        for(auto* node:active_nodes_) node->mutates_ambient=true;
    }
    void record_resource_access(ResourceAccess access) {
        for(auto* node:active_nodes_) node->resources.push_back(access);
        record_resource(summary_.resources,std::move(access));
    }
    void read_unknown() {
        for (std::size_t i = 0; i < summary_.exposed_reads.size(); ++i) {
            if (exposed(i)) summary_.exposed_reads[i].insert(StoragePath{StorageRoot::parameter(i), {}});
        }
    }

    void record_read(const Places& places) {
        for(const auto& place:places.places)record_reference_region(place,false);
        record_node_places(places,false);
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
    void shared_mutation(const StoragePath& origin, SourceSpan span,bool legacy_shared=true) {
        if(legacy_shared)for(auto* node:active_nodes_)
            node->shared.push_back(ParallelSharedAccess{origin,true,fresh_region_of(origin)});
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
            if(binding) {
                const auto targets=held_targets_.find(binding->key);
                if(targets!=held_targets_.end() && !targets->second.unknown && !targets->second.places.empty()) {
                    for(const auto& target:targets->second.places) {
                        if(target.shared_origin) {shared_mutation(*target.shared_origin,span,legacy_shared);continue;}
                        if(target.path.root.kind==RootKind::Parameter && exposed(target.path.root.index)) {
                            summary_.exposed_writes[target.path.root.index].insert(target.path);
                            if(is_method() && target.path.root.index==0)shared_mutation(target.path,span,legacy_shared);
                        } else if(target.path.root.kind!=RootKind::Local && target.path.root.kind!=RootKind::Parameter)shared_mutation(target.path,span);
                    }
                    return;
                }
            }
            const auto provenance = binding ? local_provenance(binding->key) : std::optional<Provenance>(existing());
            if (!provenance || provenance->kind == Provenance::Kind::Fresh) return;
            if (provenance->kind == Provenance::Kind::Parameter) {
                shared_mutation(StoragePath{StorageRoot::parameter(provenance->parameter), {}}, span,legacy_shared);
                return;
            }
            summary_.mutates_ambient = true;
        record_node_ambient();
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
        for(const auto& place:places.places)record_reference_region(place,true);
        record_node_places(places,true);
        for (auto* log : write_logs_) {
            if (places.unknown) log->push_back(LoggedWrite{true, {}});
            for (const auto& place : places.places) log->push_back(LoggedWrite{false, place});
        }
        if (places.unknown) write_unknown(span);
        for (const auto& place : places.places) {
            if (place.shared_origin) {
                if(const auto targets=resolved_reference_targets(place)) {
                    // Resolved indirect paths are conflict metadata, never
                    // normal producer paths for a parallel dependency edge.
                    for(const auto& target:targets->places) {
                        if(target.path.root.kind==RootKind::Parameter && exposed(target.path.root.index))summary_.exposed_writes[target.path.root.index].insert(target.path);
                        if(target.shared_origin)shared_mutation(*target.shared_origin,span,false);
                    }
                    shared_mutation(*place.shared_origin,span,false);
                    continue;
                }
                shared_mutation(*place.shared_origin, span,!place.reference_type);
                continue;
            }
            if (place.path.root.kind != RootKind::Parameter) continue;
            const auto index = place.path.root.index;
            if (!exposed(index)) continue;
            summary_.exposed_writes[index].insert(place.path);
            if (is_method() && index == 0 && !quiet_) {
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
        record_resource_access( ResourceAccess{kind, use, ResourceOrigin::Direct, {}});
    }

    // ---- statements ------------------------------------------------------------

    void walk_block(const std::vector<StmtPtr>& body) {
        scopes_.emplace_back();
        for (const auto& statement : body) walk_stmt(*statement);
        scopes_.pop_back();
    }

    void walk_parallel(const Stmt& statement,const ParallelStmt& suite) {
        if(!parallel_plans_) {walk_block(suite.body);return;}
        auto plan=std::make_shared<ParallelBlockPlan>();
        plan->nodes.resize(suite.body.size());
        plan->captures.resize(suite.body.size());
        plan->worker_safe.resize(suite.body.size(),false);
        plan->fallible.resize(suite.body.size(),false);
        scopes_.emplace_back();
        const auto names=parameter_names();
        const auto capture_node=[&](std::size_t i) {
            std::map<std::string,bool> captures;
            const auto capture=[&](const StoragePath& path,bool writes) {
                std::string name;
                if(path.root.kind==RootKind::Local) name=path.root.name;
                else if(path.root.kind==RootKind::Parameter && path.root.index<names.size())
                    name=names[path.root.index];
                if(!name.empty() && (lookup(name) || name=="this"))
                    captures[name]=captures[name] || writes;
            };
            for(const auto& path:plan->nodes[i].address_roots) capture(path,false);
            for(const auto& path:plan->nodes[i].reads) capture(path,false);
            for(const auto& path:plan->nodes[i].writes) capture(path,true);
            for(const auto& shared:plan->nodes[i].shared) capture(shared.path,shared.writes);
            for(const auto& resource:plan->nodes[i].resources)
                if(resource.origin==ResourceOrigin::Handle) capture(resource.handle,true);
            for(const auto& [name,writes]:captures) {
                const auto* binding=lookup(name);
                const bool reference=binding && (binding->kind==Binding::Kind::Reference ||
                    binding->kind==Binding::Kind::ReferenceParameter);
                const auto* declaration=binding && !binding->shape_variable && (binding->kind==Binding::Kind::Value ||
                    binding->kind==Binding::Kind::Reference)
                    ?static_cast<const Stmt*>(binding->key):nullptr;
                if(declaration && !checked_.binding_types.contains(declaration)) declaration=nullptr;
                plan->captures[i].push_back(ParallelCapture{name,writes,declaration,reference,binding && binding->shape_variable});
            }
            const auto& effects=plan->nodes[i];
            plan->fallible[i]=effects.fallible;
            plan->worker_safe[i]=!effects.fallible && !effects.requires_managed_storage &&
                !effects.unproven_call && !effects.mutates_ambient && effects.shared.empty() &&
                effects.resources.empty() && std::none_of(plan->captures[i].begin(),
                    plan->captures[i].end(),[](const ParallelCapture& c){return c.source_reference;});
        };
        for(std::size_t i=0;i<suite.body.size();++i) {
            active_nodes_.push_back(&plan->nodes[i]);
            walk_stmt(*suite.body[i]);
            active_nodes_.pop_back();
            capture_node(i);
        }
        const auto dependencies=parallel_dependencies(plan->nodes,names,&checked_);
        if(dependencies.conflict) {
            const auto& c=*dependencies.conflict;
            const auto& first=*suite.body[c.first];
            const auto& second=*suite.body[c.second];
            std::string detail;
            if(c.kind==ParallelConflictKind::WriteWrite) detail="both write '"+c.storage+"'";
            else if(c.kind==ParallelConflictKind::ReadWrite) detail="one reads '"+c.storage+"' while the other writes it";
            else if(c.kind==ParallelConflictKind::Ambient) detail="both change runtime state";
            else if(c.handle) detail="both use file handle '"+c.storage+"'";
            else switch(c.resource) {
            case ResourceKind::Stdout:detail="both write standard output";break;
            case ResourceKind::Stdin:detail="both use standard input";break;
            case ResourceKind::Stderr:detail="both write standard error";break;
            case ResourceKind::FileSystem:detail="one changes the file system while the other uses it";break;
            case ResourceKind::Network:detail="both use the network";break;
            case ResourceKind::Clock:detail="both use the clock";break;
            case ResourceKind::Process:detail="both use processes";break;
            case ResourceKind::Environment:detail="one changes the environment while the other uses it";break;
            }
            throw CompileError(Diagnostic{"PARALLEL_CONFLICT",
                "Statements at "+std::to_string(first.span.start.line)+":"+
                    std::to_string(first.span.start.column)+" and "+
                    std::to_string(second.span.start.line)+":"+
                    std::to_string(second.span.start.column)+" may run at the same time and "+detail+".",
                second.span});
        }
        plan->predecessors=dependencies.predecessors;
        (*parallel_plans_)[&statement]=std::move(plan);
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

    void walk_expansion(const PatternExpansion& expansion,const std::optional<Places>& places,const std::optional<Provenance>& provenance) {
        Binding subject;subject.key=&expansion;subject.kind=Binding::Kind::Reference;declare(expansion.subject,subject);
        add_reference_places(&expansion,places?*places:Places{true,{}});add_local_provenance(&expansion,provenance);
        for(const auto& binding:expansion.bindings)walk_stmt(*binding);
    }

    void walk_stmt(const Stmt& statement) {
        const bool conditional = std::holds_alternative<IfStmt>(statement.data) || std::holds_alternative<WhileStmt>(statement.data) || std::holds_alternative<ForStmt>(statement.data) || std::holds_alternative<MatchStmt>(statement.data);
        struct Conditional { std::size_t& depth; bool active; ~Conditional(){if(active)--depth;} } guard{conditional_depth_,conditional};
        if (conditional) ++conditional_depth_;
        if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
            if(node->pattern){read_expr(*node->value);const auto& expansion=checked_.pattern_expansions.at(&statement);walk_expansion(expansion,places_of(*node->value),provenance_of(*node->value));return;}
            Binding binding;
            binding.key = &statement;
            binding.is_const = node->is_const;
            const auto binding_type=checked_.binding_types.find(&statement);
            for(auto* active:active_nodes_) {
                active->writes.insert(StoragePath{StorageRoot::local(node->name),{}});
                if(node->reference || binding_type==checked_.binding_types.end() ||
                   requires_lifetime_management(binding_type->second))
                    active->requires_managed_storage=true;
            }
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
            const ShapePlan* shape_plan=nullptr;
            if(const auto found=checked_.shape_plans.find(&node->declared_type);
               found!=checked_.shape_plans.end())shape_plan=&found->second;
            if(shape_plan) {
                for(const auto& axis:shape_plan->axes)
                    if(axis.capture && !axis.after_initializer)read_expr(*axis.capture);
                for(const auto& variable:shape_plan->variables)if(variable.read) {
                    Binding extent_binding;extent_binding.kind=Binding::Kind::Value;
                    extent_binding.key=&variable;extent_binding.is_const=true;extent_binding.shape_variable=true;
                    declare(variable.name,extent_binding);
                    for(auto* active:active_nodes_) {
                        active->writes.insert(StoragePath{StorageRoot::local(variable.name),{}});
                        active->requires_managed_storage=true;
                    }
                }
                for(const auto axis:shape_plan->after_order)
                    if(shape_plan->axes[axis].capture)read_expr(*shape_plan->axes[axis].capture);
            }else {
                for(const auto& expression:node->declared_type.tensor_shape_expressions)
                    if(expression)read_expr(*expression);
                for(const auto& expression:node->declared_type.dimension_expressions)
                    if(expression)read_expr(*expression);
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
            set_fresh_region(&statement,node->value?fresh_region_of(*node->value):0);
            for(auto* active:active_nodes_)
                active->writes.insert(StoragePath{StorageRoot::local(node->name),{}});
            if (node->value) add_held_targets(&statement,targets_of(*node->value));
        } else if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
            if(node->pattern){read_expr(*node->value);walk_expansion(checked_.pattern_expansions.at(&statement),places_of(*node->value),provenance_of(*node->value));return;}
            read_storage_operands(*node->target_expr());
            auto target = places_of(*node->target_expr());
            if (!node->compound_op.empty() && target) record_read(*target);
            read_expr(*node->value);
            if (target) {
                validate_complete_store(*node->target_expr(),*target,statement.span);
                const auto held = targets_of(*node->value);
                for (const auto& destination : target->places)
                    for (const auto& stored : held.places)
                        record_link(destination.path, stored.path, conditional_depth_ == 0 && !saw_exit_, statement.span);
                record_write(*target, statement.span);
                if (const auto* name = std::get_if<NameExpr>(&node->target_expr()->data);
                    name && !name->this_qualifier && !name->self_reference) {
                    if (const auto* binding = lookup(name->name);
                        binding && binding->kind == Binding::Kind::Value) {
                        set_fresh_region(binding->key,node->compound_op.empty()
                            ?fresh_region_of(*node->value):0);
                        add_held_targets(binding->key,targets_of(*node->value));
                        add_local_provenance(binding->key, node->compound_op.empty()
                                                               ? provenance_of(*node->value)
                                                               : std::optional<Provenance>(fresh()));
                    }
                }
            }
        } else if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
            read_storage_operands(*node->target);
            for(auto* active:active_nodes_) {
                active->writes.insert(StoragePath{StorageRoot::local(node->name),{}});
                active->requires_managed_storage=true;
            }
            auto places = places_of(*node->target);
            if (!places) places = Places{true, {}};
            if (node->place) {
                record_reference_escape(*places, statement.span);
                const auto holder_root = is_constructor() ? StoragePath{StorageRoot::local("$result"),{}} : receiver_path();
                for (const auto& target : places->places) record_link(holder_root, target.path, conditional_depth_ == 0 && !saw_exit_, statement.span);
                const auto* field=std::get_if<NameExpr>(&node->place->data);
                Places holder;holder.places.insert(Place{receiver_path().child(PathStep::field_named(field->name)),std::nullopt,std::nullopt});
                record_write(holder,statement.span);
                return;
            }
            if (const auto* binding = lookup(node->name);
                binding && binding->kind == Binding::Kind::Reference) {
                observe_address(*places);
                add_reference_places(binding->key, *places);
            }
        } else if (const auto* node = std::get_if<ReturnStmt>(&statement.data)) {
            if (node->value) {
                read_expr(*node->value);
                result_ = join(result_, provenance_of(*node->value));
                for (const auto& stored : targets_of(*node->value).places)
                    record_link(StoragePath{StorageRoot::local("$result"),{}}, stored.path, conditional_depth_ == 0 && !saw_exit_, statement.span);
            }
            saw_exit_ = true;
        } else if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
            read_expr(*node->value);
        } else if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
            read_expr(*node->condition);
            walk_block(node->then_body);
            walk_block(node->else_body);
        } else if (const auto* node = std::get_if<ParallelStmt>(&statement.data)) {
            walk_parallel(statement,*node);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
            walk_block(node->body);
        } else if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
            read_expr(*node->condition);
            walk_block(node->body);
        } else if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
            visit_for_header(*node,[&](const Expr& operand) { read_expr(operand); });
            scopes_.emplace_back();
            Binding binding;
            binding.key = &statement;
            std::optional<Places> iteration_places;
            if (node->writable) {
                binding.kind = Binding::Kind::Reference;
                const auto plan=checked_.traversal_plans.find(node->iterable.get());
                const Expr* root=node->iterable.get();
                if(plan!=checked_.traversal_plans.end()) {
                    if(const auto* call=std::get_if<CallExpr>(&root->data);call&&!call->args.empty())root=call->args.front().value.get();
                    else if(const auto* method=std::get_if<MethodCallExpr>(&root->data))root=method->receiver.get();
                }
                const auto root_binding=checked_.traversal_bindings.find(node->iterable.get());
                if(root_binding!=checked_.traversal_bindings.end())root=root_binding->second.root;
                auto places=root_binding!=checked_.traversal_bindings.end()&&!root_binding->second.borrowed
                    ?std::optional<Places>{Places{false,{}}}:places_of(*root);
                if(!places)places=Places{true,{}};
                Places elements;elements.unknown=places->unknown;
                for(const auto& place:places->places) {
                    if(plan==checked_.traversal_plans.end()) elements.places.insert(Place{place.path.child(PathStep::any_element()),place.shared_origin,place.reference_type});
                    else {
                        const auto add=[&](const auto& self,const TraversalNode& tree,StoragePath path)->void {
                            if(tree.kind==TraversalNode::Kind::Visit){elements.places.insert(Place{std::move(path),place.shared_origin,place.reference_type});return;}
                            if(tree.kind==TraversalNode::Kind::Field)path=path.child(PathStep::field_named(tree.name));
                            if(tree.kind==TraversalNode::Kind::TupleElement)path=path.child(PathStep::tuple_element(tree.index));
                            if(tree.kind==TraversalNode::Kind::Array)path=path.child(PathStep::any_element());
                            for(const auto& child:tree.children)self(self,child,path);
                        };
                        auto prefix=place.path;
                        if(root_binding!=checked_.traversal_bindings.end())for(const auto& step:root_binding->second.prefix) {
                            if(step.kind==TraversalNode::Kind::Field)prefix=prefix.child(PathStep::field_named(step.name));
                            else if(step.kind==TraversalNode::Kind::TupleElement)prefix=prefix.child(PathStep::tuple_element(step.index));
                            else prefix=prefix.child(PathStep::any_element());
                        }
                        add(add,plan->second.tree,std::move(prefix));
                    }
                }
                iteration_places=elements;
                add_reference_places(&statement,elements);
            } else {
                binding.kind = Binding::Kind::Value;
                add_local_provenance(&statement, provenance_of(*node->iterable));
            }
            if(node->pattern) {
                auto places=iteration_places;
                if(!node->writable) {
                    places=places_of(*node->iterable);
                    if(places){Places elements;elements.unknown=places->unknown;for(const auto& p:places->places)elements.places.insert(Place{p.path.child(PathStep::any_element()),p.shared_origin,p.reference_type});places=std::move(elements);}
                }
                walk_expansion(checked_.control_patterns.at(&*node->pattern),places,provenance_of(*node->iterable));
            }
            else declare(node->name,binding);
            for(const auto& extra:node->collections) {
                if(extra.name=="_") continue;
                Binding value;
                value.kind=Binding::Kind::Value;
                value.key=&extra;
                add_local_provenance(&extra,provenance_of(*extra.iterable));
                declare(extra.name,value);
            }
            walk_block(node->body);
            scopes_.pop_back();
        } else if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
            read_expr(*node->value);
            const auto provenance = provenance_of(*node->value);
            for (const auto& match_case : node->cases) {
                scopes_.emplace_back();
                if(match_case.pattern)walk_expansion(checked_.control_patterns.at(&*match_case.pattern),places_of(*node->value),provenance);
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
    void capture_storage_address(const std::string& name) {
        const auto* binding=lookup(name);
        if(!binding) return;
        if(binding->kind==Binding::Kind::Value && !binding->shape_variable) {
            const auto* declaration=static_cast<const Stmt*>(binding->key);
            if(checked_.binding_types.contains(declaration))
                if(const auto* value=std::get_if<BindingStmt>(&declaration->data);value && value->literal_constant) return;
        }
        const auto root=binding->kind==Binding::Kind::ValueParameter ||
                        binding->kind==Binding::Kind::ReferenceParameter
            ?StoragePath{StorageRoot::parameter(binding->parameter),{}}
            :StoragePath{StorageRoot::local(name),{}};
        for(auto* node:active_nodes_) node->address_roots.insert(root);
    }

    void read_storage_operands(const Expr& expression) {
        if (const auto* projection=std::get_if<TupleProjectionExpr>(&expression.data)) {
            capture_storage_address(projection->subject);
            if(const auto* binding=lookup(projection->subject);binding &&
               (binding->kind==Binding::Kind::Reference ||
                binding->kind==Binding::Kind::ReferenceParameter)) {
                const auto holder=binding->kind==Binding::Kind::ReferenceParameter
                    ?StoragePath{StorageRoot::parameter(binding->parameter),{}}
                    :StoragePath{StorageRoot::local(projection->subject),{}};
                for(auto* active:active_nodes_) active->reads.insert(holder);
            }
            return;
        }
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
        if (const auto* name=std::get_if<NameExpr>(&expression.data)) {
            if(name->this_qualifier || name->self_reference) {
                for(auto* node:active_nodes_) node->address_roots.insert(receiver_path());
            } else capture_storage_address(name->name);
            if(const auto* binding=lookup(name->name);binding &&
               (binding->kind==Binding::Kind::Reference ||
                binding->kind==Binding::Kind::ReferenceParameter)) {
                const auto holder=binding->kind==Binding::Kind::ReferenceParameter
                    ?StoragePath{StorageRoot::parameter(binding->parameter),{}}
                    :StoragePath{StorageRoot::local(name->name),{}};
                for(auto* active:active_nodes_) active->reads.insert(holder);
            }
            return;
        }
        read_expr(expression);
    }

    void mark_target_expression(const Expr& expression,const Places& places) {
        if(!reference_expressions_)return;
        for(const auto& place:places.places)if(reference_candidate(place)) {
            reference_expressions_->insert(&expression);
            if(reference_paths_)(*reference_paths_)[&expression].insert(place.path);
        }
    }

    void read_expr(const Expr& expression) {
        const auto* value_type=type_of(expression);
        for(auto* active:active_nodes_) {
            if(!value_type || requires_lifetime_management(*value_type))
                active->requires_managed_storage=true;
            if(std::holds_alternative<CallExpr>(expression.data) ||
               std::holds_alternative<MethodCallExpr>(expression.data)) active->unproven_call=true;
            if(std::holds_alternative<TryExpr>(expression.data)) active->fallible=true;
        }
        if (std::holds_alternative<TupleProjectionExpr>(expression.data) ||
            std::holds_alternative<NameExpr>(expression.data) ||

            std::holds_alternative<MemberExpr>(expression.data) ||
            std::holds_alternative<IndexExpr>(expression.data)) {
            read_storage_operands(expression);
            if (const auto places = places_of(expression)) {mark_target_expression(expression,*places);record_read(*places);}
            return;
        }
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) read_expr(*item);
        } else if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
            for (const auto& item : node->elements) read_expr(*item);
        } else if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
            read_expr(*node->source);
            scopes_.emplace_back();
            Binding binding;
            binding.kind = Binding::Kind::Value;
            binding.key = &expression;
            binding.value_type = *checked_.expr_types.at(node->source.get()).first;
            declare(node->name, binding);
            add_local_provenance(&expression, provenance_of(*node->source));
            add_held_targets(&expression, targets_of(*node->source));
            if (node->condition) read_expr(*node->condition);
            read_expr(*node->value);
            const auto held = targets_of(*node->value);
            for (auto* active : active_nodes_) {
                const auto private_copy = [&](const StoragePath& path) {
                    return path.root.kind == RootKind::Local && path.root.name == node->name;
                };
                std::erase_if(active->reads, private_copy);
                std::erase_if(active->writes, private_copy);
                std::erase_if(active->address_roots, private_copy);
            }
            scopes_.pop_back();
            held_targets_[&expression] = held;
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
        } else if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
            for (const auto& item : node->operands) read_expr(*item);
        } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            read_expr(*node->value);
        } else if (const auto* node = std::get_if<LiteralConstantExpr>(&expression.data)) {
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
        if (std::holds_alternative<TupleExpr>(expression.data)) {
            // Constructing an unboxed tuple creates a fresh value. Its
            // element effects and held reference targets are walked separately.
            return fresh();
        }
        if (std::holds_alternative<TupleProjectionExpr>(expression.data) ||
            std::holds_alternative<NameExpr>(expression.data) ||
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
                places.places.insert(Place{receiver_path(), std::nullopt, std::nullopt});
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
                construction(expression, node, resolution->second.target);
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

    // The hidden changes of the constructor a construction runs (L1's
    // constructor addendum): the value it creates is fresh, but the shared
    // regions of its by-value arguments and runtime state are not.
    void construction(const Expr& expression, const CallExpr& node, const std::string& class_name) {
        const auto constructor = constructor_of(checked_, class_name, node);
        if (constructor.empty()) return;
        const auto* callee_summary = summaries_.find(constructor);
        const auto signature = checked_.functions.find(constructor);
        if (!callee_summary || signature == checked_.functions.end()) return;
        const auto& parameters = signature->second.parameters;
        const auto bound = bind_arguments(constructor, node.args, 0);
        for (std::size_t i = 0; i < callee_summary->mutates_shared_of_parameter.size() && i < parameters.size();
             ++i) {
            if (!callee_summary->mutates_shared_of_parameter[i] || parameters[i].writable) continue;
            const auto* argument = i < bound.size() ? bound[i] : nullptr;
            if (argument) shared_mutation_of_argument(*argument->value, expression.span);
        }
        if (callee_summary->mutates_ambient) hidden_ambient_through(expression.span, constructor);
    }

    std::optional<Places> places_of_callee(const std::string& name) {
        const auto* binding = lookup(name);
        if (!binding) return std::nullopt;
        Places places;
        if (binding->kind == Binding::Kind::Reference) return reference_places(binding->key);
        if (binding->kind == Binding::Kind::Value) {
            places.places.insert(Place{StoragePath{StorageRoot::local(name), {}}, std::nullopt, std::nullopt});
        } else {
            places.places.insert(Place{StoragePath{StorageRoot::parameter(binding->parameter), {}}, std::nullopt, std::nullopt});
        }
        return places;
    }

    // The arguments of a by-value parameter whose shared regions the callee
    // changes: the regions reachable from the argument's storage, or from
    // where a temporary came from.
    void shared_mutation_of_argument(const Expr& argument, SourceSpan span,bool legacy_shared=true) {
        if (const auto places = places_of(argument)) {
            if (places->unknown) hidden_ambient(span, "runtime state");
            for (const auto& place : places->places) {
                shared_mutation(place.shared_origin ? *place.shared_origin : place.path, span,legacy_shared);
            }
            return;
        }
        const auto provenance = provenance_of(argument);
        if (!provenance || provenance->kind == Provenance::Kind::Fresh) return;
        if (provenance->kind == Provenance::Kind::Parameter) {
            shared_mutation(StoragePath{StorageRoot::parameter(provenance->parameter), {}}, span,legacy_shared);
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
            result.places.insert(Place{std::move(path), place.shared_origin,place.reference_type});
        }
        return result;
    }

    void carry_reference_regions(const EffectSummary& callee,const std::vector<std::optional<Places>>& arguments,const Expr& call) {
        const auto* saved=reference_call_;reference_call_=&call;
        call_writes_.try_emplace(&call);
        const auto carry=[&](const auto& origins,const auto& regions,bool writes) {
            for(const auto& [origin,type]:origins) {
                if(origin.root.kind==RootKind::Parameter && origin.root.index<arguments.size() && arguments[origin.root.index]) {
                    for(const auto& holder:carried(*arguments[origin.root.index],origin).places) {
                        Place region{StoragePath{StorageRoot::shared(describe(holder.path,parameter_names())),{}},holder.path,type};
                        record_reference_region(region,writes);
                    }
                } else {
                    (writes?summary_.reference_shared_writes:summary_.reference_shared_reads).emplace(type_name(type),type);
                    if(writes)call_writes_[&call].shared.emplace(type_name(type),type);
                    (writes?summary_.reference_shared_write_origins:summary_.reference_shared_read_origins).insert_or_assign(origin,type);
                    for(auto* node:active_nodes_)(writes?node->reference_shared_writes:node->reference_shared_reads).emplace(type_name(type),type);
                }
            }
            // Unknown extern/callee regions can lack a recoverable origin.
            for(const auto& [name,type]:regions)if(std::none_of(origins.begin(),origins.end(),[&](const auto& item){return type_name(item.second)==name;})) {
                (writes?summary_.reference_shared_writes:summary_.reference_shared_reads).emplace(name,type);
                if(writes)call_writes_[&call].shared.emplace(name,type);
                for(auto* node:active_nodes_)(writes?node->reference_shared_writes:node->reference_shared_reads).emplace(name,type);
            }
        };
        carry(callee.reference_shared_read_origins,callee.reference_shared_reads,false);
        carry(callee.reference_shared_write_origins,callee.reference_shared_writes,true);
        if(reference_calls_)(*reference_calls_)[&call]=call_writes_.at(&call);
        reference_call_=saved;
    }

    void carry_resources(const EffectSummary& callee_summary, const std::string& callee,
                         const std::vector<std::optional<Places>>& arguments) {
        for (const auto& access : callee_summary.resources) {
            if (access.origin != ResourceOrigin::Handle) {
                record_resource_access( access);
                continue;
            }
            if (access.handle.root.kind == RootKind::Parameter && access.handle.root.index < arguments.size() &&
                arguments[access.handle.root.index] && !arguments[access.handle.root.index]->unknown) {
                for (const auto& place : carried(*arguments[access.handle.root.index], access.handle).places) {
                    record_resource_access(
                                    ResourceAccess{access.kind, access.use, ResourceOrigin::Handle, place.path});
                }
                continue;
            }
            // A handle the callee made: distinct from every handle here.
            auto fresh_handle = access;
            fresh_handle.handle.root = StorageRoot::local(callee + ":" + describe(access.handle));
            record_resource_access( std::move(fresh_handle));
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
        for(std::size_t i=0;i<bound.size();++i)if(bound[i] && arguments[i])validate_complete_store(*bound[i]->value,*arguments[i],expression.span);
        if(receiver_expression && receiver)validate_complete_store(*receiver_expression,*receiver,expression.span);
        const auto substitute = [&](const StoragePath& path) {
            Places out;
            if (path.root.kind == RootKind::Parameter && path.root.index < arguments.size() && arguments[path.root.index])
                out = carried(*arguments[path.root.index],path);
            else if (path.root.kind == RootKind::Shared) out.unknown = true;
            return out;
        };
        for (const auto& link : callee_summary->reference_links) {
            const auto targets = substitute(link.target);
            if (link.holder.root.kind == RootKind::Local && link.holder.root.name == "$result") {
                add_held_targets(&expression,targets);
                continue;
            }
            const auto holders = substitute(link.holder);
            for(const auto& holder:holders.places)if(holder.path.root.kind==RootKind::Local)
                if(const auto* binding=lookup(holder.path.root.name))add_held_targets(binding->key,targets);
            for (const auto& holder : holders.places) for (const auto& target : targets.places)
                record_link(holder.path,target.path,link.must && conditional_depth_ == 0 && !saw_exit_,expression.span);
        }
        carry_reference_regions(*callee_summary,arguments,expression);
        isolate_arguments(*callee_summary, parameters, bound, arguments, evaluated, log, offset,&expression);
        // A query of the standard library (a method without the mutation
        // marker) may update caches inside its receiver: real writes, for
        // isolation, but no change of the value, so no hidden mutation.
        struct Quiet {
            bool& flag;
            bool saved;
            ~Quiet() { flag = saved; }
        } quiet{quiet_, quiet_};
        quiet_ = quiet_ || quiet_methods_.contains(callee);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const bool receiver_parameter = offset == 1 && i == 0;
            if (!receiver_parameter && !parameters[i].writable) continue;
            if (!arguments[i]) continue;  // a temporary receiver: its changes are local
            if (i < callee_summary->reference_target_escapes.size() && callee_summary->reference_target_escapes[i])
                record_reference_escape(*arguments[i], expression.span);
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
            const bool legacy_shared=std::none_of(callee_summary->reference_shared_write_origins.begin(),callee_summary->reference_shared_write_origins.end(),[&](const auto& item){return item.first.root.kind==RootKind::Parameter && item.first.root.index==i;});
            if (offset == 1 && i == 0) {
                if (receiver) {
                    for (const auto& place : receiver->places) {
                        shared_mutation(place.shared_origin ? *place.shared_origin : place.path,
                                        expression.span,legacy_shared);
                    }
                } else if (receiver_origin && receiver_origin->kind == Provenance::Kind::Parameter) {
                    shared_mutation(StoragePath{StorageRoot::parameter(receiver_origin->parameter), {}},
                                    expression.span,legacy_shared);
                } else if (receiver_origin && receiver_origin->kind == Provenance::Kind::Existing) {
                    hidden_ambient(expression.span, "shared state of a value that already exists");
                }
                continue;
            }
            const auto* argument = i < bound.size() ? bound[i] : nullptr;
            if (argument) shared_mutation_of_argument(*argument->value, expression.span,legacy_shared);
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
        carry_reference_regions(*callee_summary,{},expression);
        carry_resources(*callee_summary, callee, {});
    }

    void walk_builtin(const Expr& expression, const CallExpr& node, std::optional<BuiltinCallable> builtin) {
        if(builtin==BuiltinCallable::ReflectEach) {
            for(const auto& argument:node.args)read_storage_operands(*argument.value);
            return;
        }
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
        if (const auto* effect = builtin_callable_effect(*builtin)) {
            for (unsigned char i = 0; i < effect->count; ++i) {
                record_resource_use(resource_kind(effect->resources[i].resource),
                                    effect->resources[i].use == BuiltinResourceUse::Exclusive
                                        ? ResourceUse::Exclusive
                                        : ResourceUse::Shared);
            }
            if (effect->advances_receiver) {
                // The generator's methods advance their receiver's state.
                Places places;
                places.places.insert(Place{receiver_path().child(PathStep::field_named("$state")), std::nullopt, std::nullopt});
                record_read(places);
                record_write(places, expression.span);
            }
        }
        switch (*builtin) {
        case BuiltinCallable::Scan:
            if (const auto format = checked_.scan_formats.find(&expression);
                format != checked_.scan_formats.end()) {
                for (const auto* target : format->second.targets) {
                    if (!target) continue;
                    read_storage_operands(*target);
                    if (const auto places = places_of(*target)) record_write(*places, expression.span);
                }
            }
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
                    // Explicit targets are ordinary writes. Only an actual
                    // shared region carries a hidden mutation through copies.
                    const auto target_type=type_of(*argument.value);
                    for(const auto& place:places->places)
                        if(place.shared_origin||(target_type&&contains_shared_region(checked_,*target_type)))
                            shared_mutation(place.shared_origin?*place.shared_origin:place.path,expression.span);
                }
            } else {
                read_expr(*argument.value);
            }
        }
        const auto* receiver_type = type_of(*node.receiver);
        if (!receiver_type) return;
        const auto& method = node.method;
        if (receiver_type->kind == TypeKind::Tensor) {
            const auto* effect = standard_method_effect("tensor", method);
            if (effect && effect->change == StandardMethodChange::Receiver) {
                if (const auto places = places_of(*node.receiver)) record_write(*places, expression.span);
            }
            return;
        }
        if (receiver_type->kind != TypeKind::Class) return;
        const auto& class_name = receiver_type->class_name;
        const auto* effect = standard_method_effect(class_name, method);
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
        if (class_name == standard_class::file_handle ||
            class_name == standard_class::file_read ||
            class_name == standard_class::file_write ||
            class_name == standard_class::file_append) {
            if (!effect || effect->file == StandardMethodFileAccess::None) return;
            const auto use =
                effect->file == StandardMethodFileAccess::Write ? ResourceUse::Exclusive : ResourceUse::Shared;
            if (receiver_places && !receiver_places->unknown) {
                for (const auto& place : receiver_places->places) {
                    record_resource_access(
                                    ResourceAccess{ResourceKind::FileSystem, use, ResourceOrigin::Handle,
                                                   place.shared_origin ? *place.shared_origin : place.path});
                }
            } else {
                record_resource_access(
                                ResourceAccess{ResourceKind::FileSystem, use, ResourceOrigin::Handle,
                                               StoragePath{StorageRoot::local(
                                                               "$handle@" + std::to_string(expression.span.start.line) +
                                                               ":" + std::to_string(expression.span.start.column)),
                                                           {}}});
            }
            mutate_shared();
            return;
        }
        if(class_name==standard_class::autograd_target) {
            if(effect&&effect->change==StandardMethodChange::Receiver&&receiver_places)
                record_write(*receiver_places,expression.span);
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
                           std::size_t offset,const Expr* expression=nullptr) {
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
            const auto* final=summaries_.find(body_.name);
            const bool pinned=std::any_of(places.places.begin(),places.places.end(),[&](const auto& place){return place.shared_origin || (place.path.root.kind==RootKind::Parameter && exposed(place.path.root.index)) || (final && overlaps_any(final->pinned_complete_targets,place.path));});
            if(pinned)if(const auto type=type_of(*bound[j]->value)) {
                const auto adapted=expression?call_writes_.find(expression):call_writes_.end();
                const auto& shared=adapted==call_writes_.end()?callee.reference_shared_writes:adapted->second.shared;
                for(const auto& [_,written]:shared)if(reference_target_compatible(checked_,*type,written))isolate=true;
                if(adapted!=call_writes_.end())for(const auto& [written,_]:adapted->second.resolved)
                    for(const auto& place:places.places)if(overlaps(written,place.path))isolate=true;
            }
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
    // The methods of standard-library classes that are not declared with the
    // mutation marker.
    const std::set<std::string>& quiet_methods_;
    const Body& body_;
    std::set<const Expr*>& isolated_;
    std::vector<std::vector<LoggedWrite>*> write_logs_;

    ParallelBlockPlans* parallel_plans_{};
    std::vector<ParallelNodeEffects*> active_nodes_;
    std::map<const void*,std::uint64_t> fresh_shared_;
    std::map<const Expr*,std::uint64_t> fresh_expressions_;
    std::uint64_t next_fresh_region_{1};
    EffectSummary summary_;
    std::optional<Provenance> result_;
    std::vector<std::map<std::string, Binding>> scopes_;
    std::size_t scope_floor_{};
    std::map<const void*, Places> reference_targets_;
    std::map<const void*, Provenance> provenance_;
    std::set<const void*> read_keys_;
    bool grew_after_read_{};
    // Carrying the writes of one of quiet_methods_ (apply_call).
    bool quiet_{};
    bool validate_references_{};
    std::set<const Expr*>* reference_expressions_{};
    std::map<const Expr*,PathSet>* reference_paths_{};
    std::map<const Expr*,ReferenceCallWrites>* reference_calls_{};
    const Expr* reference_call_{};
    std::map<const Expr*,ReferenceCallWrites> call_writes_;
    std::size_t conditional_depth_{};
    bool saw_exit_{};
    std::map<const void*,Places> held_targets_;
};

// An extern changes its exposed `&` parameters, and runtime state when it is
// declared with the mutation marker: its declaration is all the compiler
// knows of foreign code.
EffectSummary external_summary(const FunctionType& signature, const FunctionDecl* declaration) {
    EffectSummary summary;
    const auto count = signature.parameters.size();
    summary.exposed_reads.assign(count, {});
    summary.exposed_writes.assign(count, {});
    summary.mutates_shared_of_parameter.assign(count, false);
    summary.const_parameter_address_observed.assign(count, false);
    summary.reference_target_escapes.assign(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        if (!signature.parameters[i].writable) continue;
        const StoragePath whole{StorageRoot::parameter(i), {}};
        summary.exposed_reads[i].insert(whole);
        if (!signature.parameters[i].is_const) summary.exposed_writes[i].insert(whole);
    }
    // Foreign code is opaque: its results are not known to be fresh.
    summary.result = existing();
    if (declaration && declaration->mutation_marker) {
        summary.mutates_ambient = true;
        summary.first_hidden_mutation = EffectCause{declaration->span, "runtime state"};
    }
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
    // The queries of the standard library: their caches change, their values
    // do not.
    std::set<std::string> quiet_methods;
    for (const auto& class_decl : checked.program.classes) {
        if (!class_decl.standard_library) continue;
        for (const auto& method : class_decl.methods) {
            if (!method.is_constructor && !method.mutation_marker)
                quiet_methods.insert(member_function_name::method(class_decl.name, method.name));
        }
    }

    EffectSummaries summaries;
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>> edges;
    for (const auto& [name, body] : bodies) {
        if (body.kind == Body::Kind::External) {
            summaries.set(name, external_summary(*body.signature, body.declaration));
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
                BodyWalker walker(checked, summaries, referenced, quiet_methods, bodies.at(name), isolated);
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
                BodyWalker walker(checked, summaries, referenced, quiet_methods, bodies.at(name), isolated);
                (void)walker.summarize();
            }
        }
    }
    ParallelBlockPlans parallel_plans;
    for(const auto& [name,body]:bodies) {
        (void)name;
        if(body.kind==Body::Kind::External) continue;
        BodyWalker nodes(checked,summaries,referenced,quiet_methods,body,isolated,&parallel_plans);
        (void)nodes.summarize();
    }
    Body entry;
    entry.name = "$entry";
    entry.kind = Body::Kind::Entry;
    entry.statements = &checked.program.statements;
    BodyWalker walker(checked, summaries, referenced, quiet_methods, entry, isolated,&parallel_plans);
    summaries.set(entry.name, walker.summarize());
    std::set<const Expr*> reference_expressions;
    std::map<const Expr*,PathSet> reference_paths;
    std::map<const Expr*,ReferenceCallWrites> reference_calls;
    // Validation runs once after all summaries have reached their fixed point.
    for (const auto& [name, body] : bodies) {
        if (body.kind == Body::Kind::External) continue;
        BodyWalker validation(checked,summaries,referenced,quiet_methods,body,isolated,nullptr,true,&reference_expressions,&reference_paths,&reference_calls);
        (void)validation.summarize();
    }
    BodyWalker entry_validation(checked,summaries,referenced,quiet_methods,entry,isolated,nullptr,true,&reference_expressions,&reference_paths,&reference_calls);
    (void)entry_validation.summarize();
    summaries.set_reference_targets(std::move(reference_expressions));
    summaries.set_reference_flow(std::move(reference_paths),std::move(reference_calls));
    BodyWalker entry_nodes(checked,summaries,referenced,quiet_methods,entry,isolated,&parallel_plans);
    (void)entry_nodes.summarize();
    summaries.set_isolated(std::move(isolated));
    summaries.set_parallel_blocks(std::move(parallel_plans));
    return summaries;
}

} // namespace quidra::semantics
