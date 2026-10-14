#include "ir/analysis/object_bindings.hpp"
#include "ir/analysis/inline_scopes.hpp"
#include "ir/inline_binding_origins.hpp"
#include "quidra/ir/initialization_mask.hpp"
#include "quidra/numeric_types.hpp"
#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace quidra::ir::analysis {
namespace {
struct Work {
  std::uint64_t count{}, limit;
  bool exhausted{};
  bool take() {
    if (count >= limit) {
      exhausted = true;
      return false;
    }
    ++count;
    return true;
  }
};
struct RootLess {
  bool operator()(const ObjectRoot &a, const ObjectRoot &b) const {
    return std::tie(a.kind, a.function, a.at.block, a.at.index, a.parameter,
                    a.class_identity, a.constructor_declaration,
                    a.constructor_function, a.constructor_arguments) <
           std::tie(b.kind, b.function, b.at.block, b.at.index, b.parameter,
                    b.class_identity, b.constructor_declaration,
                    b.constructor_function, b.constructor_arguments);
  }
};
const BindingClass *layout(const BindingSources &sources, const Type &type) {
  if (type.kind != TypeKind::Class)
    return nullptr;
  const auto it = sources.classes.find(type.class_name);
  return it == sources.classes.end() ? nullptr : &it->second;
}
const BindingClass *layout(const BindingSources &sources,
                           const ClassIdentity &identity) {
  const BindingClass *result = nullptr;
  for (const auto &[name, value] : sources.classes) {
    (void)name;
    if (value.identity == identity) {
      if (result)
        return nullptr;
      result = &value;
    }
  }
  return result;
}
Type root_type(const BindingSources &sources, const ObjectRoot &root) {
  if (!root.class_identity)
    return {};
  const auto *identity = layout(sources, *root.class_identity);
  if (!identity || !identity->complete)
    return {};
  for (const auto &[name, value] : sources.classes)
    if (&value == identity)
      return Type::class_type(name);
  return {};
}
Type path_type(const BindingSources &sources, const ObjectPath &path) {
  auto type = root_type(sources, path.root);
  for (const auto &step : path.steps) {
    if (step.kind == semantics::StepKind::Field) {
      const auto *c = layout(sources, type);
      if (!c || !c->complete)
        return {};
      const auto f =
          std::find_if(c->fields.begin(), c->fields.end(),
                       [&](const auto &v) { return v.name == step.field; });
      if (f == c->fields.end())
        return {};
      type = f->type;
    } else if (step.kind == semantics::StepKind::TupleElement &&
               step.index >= 0 &&
               static_cast<std::uint64_t>(step.index) < type.parameters.size())
      type = type.parameters[static_cast<std::size_t>(step.index)];
    else
      return {};
  }
  return type;
}
bool prefix(const ObjectPath &a, const ObjectPath &b) {
  return a.root == b.root && a.steps.size() <= b.steps.size() &&
         std::equal(a.steps.begin(), a.steps.end(), b.steps.begin());
}
bool may_prefix(const ObjectPath &a, const ObjectPath &b) {
  if (a.root == b.root)
    return prefix(a, b);
  // Formal references can alias, including differently typed enclosing and
  // nested subobjects. Different indices/types alone prove no disjointness.
  return a.root.kind == ObjectRoot::Parameter &&
         b.root.kind == ObjectRoot::Parameter;
}
const FunctionBindingSource *origin(const BindingSources &sources,
                                    const Function &fn) {
  const auto p = sources.functions.find(fn.name);
  return p == sources.functions.end() ? nullptr : &p->second;
}
bool exact_owner(const FunctionBindingSource &source,
                 const BindingClass &owner) {
  return source.complete &&
         source.declaration.package == owner.identity.package &&
         source.declaration.module_path == owner.identity.module_path &&
         source.declaration.owner_class == owner.identity.source_name &&
         source.declaration.generic_args == owner.identity.generic_arguments;
}
// A by-value scalar cannot carry a class/function/aggregate/reference
// identity. This is a typed ABI property, not recognition of root names.
bool nonstructural_scalar(const Type &type) {
  if (type.first || !type.parameters.empty() || !type.cases.empty())
    return false;
  if (type.kind == TypeKind::Bool)
    return true;
  const auto *info = numeric_kind_info(type.kind);
  return info && info->family != NumericFamily::Complex;
}
bool scalar_abi_matches(const Type &actual, const Type &formal) {
  if (!nonstructural_scalar(actual) || !nonstructural_scalar(formal))
    return false;
  if (actual == formal)
    return true;
  // Core serial producers use nat64 while existing hidden formals use
  // int64; both are exactly the same fixed integer ABI word. No pointer,
  // boxed/floating or structural reinterpretation is inferred here.
  const auto *a = numeric_kind_info(actual.kind);
  const auto *b = numeric_kind_info(formal.kind);
  return a && b && a->width && a->width == b->width &&
         a->family != NumericFamily::Real && b->family != NumericFamily::Real;
}
// Resolve the concrete ABI once for the call. Both identity transfer and
// published evidence use the same original target and actual-site facts.
template <class ReadFact>
const Function *exact_call_target(const Module &module, const Call &call,
                                  ReadFact &&read) {
  const Function *target = nullptr;
  for (const auto &candidate : module.functions)
    if (candidate.name == call.callee) {
      if (target)
        return nullptr;
      target = &candidate;
    }
  if (!target || target->parameters.size() != call.args.size() ||
      target->result != call.result)
    return nullptr;
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    const auto &arg = call.args[i];
    const auto fact = read(arg.writable_address.value_or(arg.value));
    if (target->parameters[i].type.kind == TypeKind::Class &&
        fact.object_type != target->parameters[i].type)
      return nullptr;
  }
  return target;
}
bool unmapped_scalar_argument(const Call &call, std::size_t ir,
                              const Function *target,
                              const FunctionSummary &summary,
                              const BindingSources &sources,
                              const ObjectIdentityFact &value) {
  if (!target || ir >= call.args.size() || !summary.semantic ||
      !summary.effects_complete || (summary.effects & effect_bit::Opaque) ||
      summary.semantic_parameter_for_ir_parameter.size() != call.args.size() ||
      summary.semantic_parameter_for_ir_parameter[ir])
    return false;
  const auto *captured = origin(sources, *target);
  const auto &argument = call.args[ir];
  return captured && captured->complete &&
         captured->availability.parameter_map &&
         captured->semantic_parameter_for_ir_parameter ==
             summary.semantic_parameter_for_ir_parameter &&
         !target->parameters[ir].writable && !target->parameters[ir].borrowed &&
         !argument.writable_address && !value.address && !value.object &&
         !value.allocation && value.references.empty() &&
         scalar_abi_matches(value.type, target->parameters[ir].type);
}
bool owned_scope(const Module &module, const Function &fn,
                 const ScopedBindingSource &row) {
  if (!module.binding_origins ||
      module.binding_origins->class_fields !=
          canonical_encode_value(module.classes).bytes ||
      !valid_scoped_binding_source(module, fn, row))
    return false;
  const auto owner = module.binding_origins->functions.find(fn.name);
  if (owner == module.binding_origins->functions.end() ||
      owner->second.body_fields != binding_origin_body_fields(fn))
    return false;
  const auto actual = owner->second.scopes.find(row.scope);
  return actual != owner->second.scopes.end() &&
         canonical_encode_value(actual->second).bytes ==
             canonical_encode_value(row).bytes;
}
const ScopedBindingSource *scope_row(const Module &module,
                                     const BindingSources &sources,
                                     const Function &fn, std::uint64_t id) {
  const auto host = sources.scopes.find(fn.name);
  if (host == sources.scopes.end())
    return nullptr;
  const auto row = host->second.find(id);
  return row != host->second.end() && owned_scope(module, fn, row->second)
             ? &row->second
             : nullptr;
}
std::optional<std::uint64_t> slot_scope(const Function &fn,
                                        std::string_view name) {
  std::optional<std::uint64_t> found;
  for (const auto &scope : fn.inline_scopes)
    for (const auto &slot : scope.slots)
      if (slot.name == name) {
        if (found)
          return {};
        found = scope.identity;
      }
  return found.value_or(0);
}
bool descendant(const Function &fn, std::uint64_t scope, std::uint64_t parent) {
  std::set<std::uint64_t> seen;
  while (scope) {
    if (!seen.insert(scope).second)
      return false;
    if (scope == parent)
      return true;
    const auto it =
        std::find_if(fn.inline_scopes.begin(), fn.inline_scopes.end(),
                     [&](const auto &row) { return row.identity == scope; });
    if (it == fn.inline_scopes.end())
      return false;
    scope = it->parent;
  }
  return false;
}
struct Env {
  std::map<ValueId, ObjectIdentityFact> values;
  std::map<std::string, ObjectIdentityFact, std::less<>> locals, references;
  std::map<ObjectRoot, std::map<std::size_t, ObjectPath>, RootLess> fields;
  std::set<std::string, std::less<>> empty_locals, empty_references;
  bool operator==(const Env &) const = default;
};
struct InlineBirthState {
  InlineConstructorBirth witness;
  ObjectIdentityFact value;
  bool operator==(const InlineBirthState &) const = default;
};
struct Engine {
  const Module &module;
  const Function &fn;
  const ModuleSummaries &summaries;
  const BindingSources &sources;
  const ConstructorIdentities &constructors;
  Work &work;
  const FunctionBindingSource *source;
  ObjectIdentityAnalysis result;
  std::vector<std::optional<Env>> exits;
  std::map<std::uint64_t, const ScopedBindingSource *> verified_scopes;
  std::map<std::uint64_t, InlineBirthState> inline_contracts;
  Engine(const Module &m, std::size_t f, const ModuleSummaries &s,
         const BindingSources &b, const ConstructorIdentities &c, Work &w)
      : module(m), fn(m.functions.at(f)), summaries(s), sources(b),
        constructors(c), work(w), source(origin(b, fn)) {
    result.cfg = build_cfg(fn);
    result.dominance = dominators(result.cfg);
    result.loops = build_loops(result.cfg, result.dominance);
    result.scopes.resize(fn.blocks.size());
    exits.resize(fn.blocks.size());
    const auto uses = def_use(fn, &module);
    for (const auto &v : uses.values)
      if (v.definitions.size() == 1)
        result.definitions.emplace(v.value, v.definitions.front());
    bool classes_owned = false;
    if (module.interface_sources && module.binding_origins &&
        sources.scopes.contains(fn.name)) {
      const auto read = read_binding_origins(module);
      const auto actual = adapt_binding_sources(
          module, summaries, *module.interface_sources, read.inputs);
      classes_owned = canonical_encode_value(actual.classes).bytes ==
                      canonical_encode_value(sources.classes).bytes;
    }
    if (const auto rows = sources.scopes.find(fn.name);
        classes_owned && rows != sources.scopes.end())
      for (const auto &[id, row] : rows->second) {
        if (!work.take())
          break;
        if (id == row.scope && owned_scope(module, fn, row))
          verified_scopes.emplace(id, &row);
      }
  }
  const ScopedBindingSource *verified_scope(std::uint64_t id) const {
    const auto found = verified_scopes.find(id);
    return found == verified_scopes.end() ? nullptr : found->second;
  }
  ObjectRoot root(ObjectRoot::Kind kind, InstrRef at, const Type &type) const {
    ObjectRoot r;
    r.kind = kind;
    r.function = fn.name;
    r.at = at;
    if (const auto *c = layout(sources, type))
      r.class_identity = c->identity;
    return r;
  }
  ObjectIdentityFact get(ValueId value, const Env &env) const {
    const auto p = env.values.find(value);
    if (p == env.values.end())
      return {};
    auto fact = p->second;
    if (fact.object && fact.object->steps.empty())
      if (const auto refs = env.fields.find(fact.object->root);
          refs != env.fields.end())
        fact.references = refs->second;
    return fact;
  }
  void put(ValueId value, ObjectIdentityFact fact, Env &env, InstrRef at) {
    if (!value)
      return;
    env.values.insert_or_assign(value, fact);
    result.scopes[at.block].changes[value].emplace_back(at.index + 1,
                                                        std::move(fact));
  }
  void event(InstrRef at, std::optional<ObjectPath> path,
             ObjectBindingGap gap) {
    result.events.push_back({at, std::move(path), gap});
  }
  void refresh(Env &env, InstrRef at) {
    for (auto &[id, fact] : env.values) {
      fact = get(id, env);
      result.scopes[at.block].changes[id].emplace_back(at.index + 1, fact);
    }
    for (auto &[name, fact] : env.locals) {
      (void)name;
      if (fact.object && fact.object->steps.empty())
        if (const auto p = env.fields.find(fact.object->root);
            p != env.fields.end())
          fact.references = p->second;
    }
  }
  void unknown(Env &env, InstrRef at, ObjectBindingGap why) {
    env.empty_locals.clear();
    env.empty_references.clear();
    event(at, {}, why);
    for (auto &[id, f] : env.values) {
      if (f.object) {
        f.available = false;
        f.references.clear();
        result.scopes[at.block].changes[id].emplace_back(at.index + 1, f);
      }
    }
    for (auto &[name, f] : env.locals) {
      (void)name;
      if (f.object) {
        f.available = false;
        f.references.clear();
      }
    }
    for (auto &[name, f] : env.references) {
      (void)name;
      f.available = false;
      f.references.clear();
    }
    env.fields.clear();
  }
  void write(Env &env, InstrRef at, const ObjectIdentityFact &object,
             const std::vector<semantics::PathStep> &steps,
             ObjectBindingGap why) {
    if (!object.available || !object.object) {
      unknown(env, at, ObjectBindingGap::AmbiguousIdentity);
      return;
    }
    auto path = *object.object;
    path.steps.insert(path.steps.end(), steps.begin(), steps.end());
    event(at, path, why);
    if (steps.empty()) {
      for (auto &[id, f] : env.values)
        if (f.object && prefix(path, *f.object)) {
          f.available = false;
          f.references.clear();
          result.scopes[at.block].changes[id].emplace_back(at.index + 1, f);
        }
      for (auto &[name, f] : env.locals) {
        (void)name;
        if (f.object && prefix(path, *f.object)) {
          f.available = false;
          f.references.clear();
        }
      }
      env.fields.erase(path.root);
    } else if (object.object->steps.empty() &&
               steps.front().kind == semantics::StepKind::Field) {
      const auto *c = layout(sources, object.object_type);
      if (!c) {
        unknown(env, at, ObjectBindingGap::MissingClass);
        return;
      }
      const auto field =
          std::find_if(c->fields.begin(), c->fields.end(), [&](const auto &f) {
            return f.name == steps.front().field;
          });
      if (field == c->fields.end()) {
        unknown(env, at, ObjectBindingGap::MissingClass);
        return;
      }
      if (field->reference)
        env.fields[object.object->root].erase(
            static_cast<std::size_t>(field - c->fields.begin()));
    }
    refresh(env, at);
  }
  Env external() const {
    Env env;
    for (std::uint32_t i = 0; i < fn.parameters.size(); ++i) {
      const auto &p = fn.parameters[i];
      ObjectIdentityFact f;
      f.type = f.object_type = p.type;
      if (p.type.kind == TypeKind::Class) {
        auto r = root(ObjectRoot::Parameter, {}, p.type);
        r.parameter = i;
        f.object = ObjectPath{r, {}};
        f.available =
            layout(sources, p.type) && layout(sources, p.type)->complete;
      }
      if (source && source->availability.parameter_map &&
          i < source->semantic_parameter_for_ir_parameter.size() &&
          source->semantic_parameter_for_ir_parameter[i])
        f.holders.push_back(
            {semantics::StorageRoot::parameter(
                 *source->semantic_parameter_for_ir_parameter[i]),
             {}});
      env.locals.emplace(p.name, std::move(f));
    }
    return env;
  }
  Env incoming(BlockId block) const {
    auto entry = external();
    std::vector<const Env *> inputs;
    if (block == 0)
      inputs.push_back(&entry);
    for (const auto p : result.cfg.pred[block])
      if (result.cfg.reachable[p] && exits[p])
        inputs.push_back(&*exits[p]);
    if (inputs.empty())
      return {};
    auto env = *inputs.front();
    auto meet = [&](auto &map, auto member) {
      for (auto it = map.begin(); it != map.end();) {
        bool equal = true;
        for (const auto *input : inputs) {
          const auto &other = input->*member;
          const auto p = other.find(it->first);
          if (p == other.end() || p->second != it->second) {
            equal = false;
            break;
          }
        }
        if (equal)
          ++it;
        else
          it = map.erase(it);
      }
    };
    meet(env.values, &Env::values);
    meet(env.locals, &Env::locals);
    meet(env.references, &Env::references);
    meet(env.fields, &Env::fields);
    auto empty_meet = [&](auto &values, auto member) {
      for (auto it = values.begin(); it != values.end();) {
        const bool all =
            std::all_of(inputs.begin(), inputs.end(), [&](const auto *input) {
              return (input->*member).contains(*it);
            });
        if (all)
          ++it;
        else
          it = values.erase(it);
      }
    };
    empty_meet(env.empty_locals, &Env::empty_locals);
    empty_meet(env.empty_references, &Env::empty_references);
    return env;
  }
  std::optional<ObjectPath> substitute(const ObjectPath &formal,
                                       const Call &call, const Env &env,
                                       const std::string &callee) const {
    if (formal.root.kind != ObjectRoot::Parameter ||
        formal.root.function != callee ||
        formal.root.parameter >= call.args.size())
      return {};
    const auto &argument = call.args[formal.root.parameter];
    const auto value =
        get(argument.writable_address.value_or(argument.value), env);
    if (!value.available || !value.object)
      return {};
    auto path = *value.object;
    path.steps.insert(path.steps.end(), formal.steps.begin(),
                      formal.steps.end());
    return path;
  }
  void call(const Call &call, Env &env, InstrRef at) {
    // Capture exact argument identity before any described writes.
    ObjectIdentityFact output;
    output.type = call.result;
    const auto contract = constructors.find(call.callee);
    const auto *target = exact_call_target(
        module, call, [&](ValueId id) { return get(id, env); });
    const bool exact_target = target != nullptr;
    if (exact_target && contract != constructors.end() &&
        contract->second.available) {
      const auto &birth = contract->second;
      output.object_type = birth.object_type;
      auto r = root(ObjectRoot::ConstructorCall, at, birth.object_type);
      r.constructor_function = call.callee;
      r.constructor_declaration = declaration_key(birth.declaration);
      r.constructor_arguments = birth.declaration.generic_args;
      output.allocation = r;
      output.object = ObjectPath{r, {}};
      output.available = true;
      for (const auto &[field, path] : birth.references) {
        const auto bound = substitute(path, call, env, call.callee);
        if (bound)
          output.references.emplace(field, *bound);
        else
          output.available = false;
      }
    }
    const auto *summary = summaries.find(call.callee);
    if (!summary || !summary->semantic || !summary->effects_complete ||
        (summary->effects & effect_bit::Opaque) ||
        summary->semantic_parameter_for_ir_parameter.size() != call.args.size())
      unknown(env, at, ObjectBindingGap::UnknownCall);
    else {
      const auto &semantic = *summary->semantic;
      if (semantic.mutates_ambient || !semantic.reference_shared_writes.empty())
        unknown(env, at, ObjectBindingGap::UnboundedSharedWrite);
      for (std::size_t ir = 0; ir < call.args.size(); ++ir) {
        const auto mapped = summary->semantic_parameter_for_ir_parameter[ir];
        if (!mapped) {
          if (unmapped_scalar_argument(call, ir, target, *summary, sources,
                                       get(call.args[ir].value, env)))
            continue;
          unknown(env, at, ObjectBindingGap::MissingEffect);
          continue;
        }
        if (*mapped >= semantic.exposed_writes.size()) {
          unknown(env, at, ObjectBindingGap::MissingEffect);
          continue;
        }
        const auto &arg = call.args[ir];
        const auto value = get(arg.writable_address.value_or(arg.value), env);
        const auto p = *mapped;
        const auto &paths = semantic.exposed_writes[p];
        for (const auto &path : paths) {
          if (path.root.kind != semantics::RootKind::Parameter ||
              path.root.index != p) {
            unknown(env, at, ObjectBindingGap::MissingEffect);
            continue;
          }
          write(env, at, value, path.steps, ObjectBindingGap::BindingWrite);
        }
        const bool shared = p < semantic.mutates_shared_of_parameter.size() &&
                            semantic.mutates_shared_of_parameter[p];
        const bool escapes = p < semantic.reference_target_escapes.size() &&
                             semantic.reference_target_escapes[p];
        bool bounded_constructor_reference = false;
        if (output.available && value.object)
          for (const auto &[field, target_path] : output.references) {
            (void)field;
            bounded_constructor_reference |= target_path == *value.object;
          }
        if (escapes && !bounded_constructor_reference)
          event(at, value.object, ObjectBindingGap::Escaped);
        if ((shared || (p == 0 && semantic.mutates_receiver)) &&
            paths.empty() && semantic.reference_shared_write_origins.empty())
          event(at, value.object, ObjectBindingGap::UnboundedSharedWrite);
      }
      for (const auto &[path, type] : semantic.reference_shared_write_origins) {
        (void)type;
        if (path.root.kind != semantics::RootKind::Parameter) {
          unknown(env, at, ObjectBindingGap::UnboundedSharedWrite);
          continue;
        }
        std::optional<std::size_t> actual;
        for (std::size_t ir = 0;
             ir < summary->semantic_parameter_for_ir_parameter.size(); ++ir)
          if (summary->semantic_parameter_for_ir_parameter[ir] ==
              path.root.index) {
            if (actual) {
              actual.reset();
              break;
            }
            actual = ir;
          }
        if (!actual) {
          unknown(env, at, ObjectBindingGap::MissingEffect);
          continue;
        }
        const auto &arg = call.args[*actual];
        write(env, at, get(arg.writable_address.value_or(arg.value), env),
              path.steps, ObjectBindingGap::BindingWrite);
      }
    }
    if (output.available && output.object)
      env.fields[output.object->root] = output.references;
    put(call.out, std::move(output), env, at);
  }
  const ScopedBindingSource *context(InstrRef at) const {
    const auto *scope = inline_binding_scope(fn, at.block);
    return scope ? verified_scope(scope->identity) : nullptr;
  }
  const FunctionBindingSource *context_source(InstrRef at) const {
    if (inline_binding_scope(fn, at.block)) {
      const auto *row = context(at);
      return row ? &row->source : nullptr;
    }
    return source;
  }
  void holder(ObjectIdentityFact &fact, std::string_view name, InstrRef at,
              bool parameter_alias = false) {
    const auto owner = slot_scope(fn, name);
    if (!owner) {
      fact.holders.clear();
      fact.scoped_holders.clear();
      return;
    }
    if (!parameter_alias) {
      fact.holders.clear();
      fact.scoped_holders.clear();
    }
    if (!*owner) {
      if (source && source->availability.local_storage_paths)
        if (const auto p = source->local_storage_paths.find(name);
            p != source->local_storage_paths.end())
          fact.holders.push_back(p->second);
      return;
    }
    const auto *row = verified_scope(*owner);
    if (!row || !row->source.availability.local_storage_paths)
      return;
    const auto path = row->source.local_storage_paths.find(name);
    if (path == row->source.local_storage_paths.end())
      return;
    ScopedStoragePath scoped{fn.name, *owner, path->second};
    if (std::find(fact.scoped_holders.begin(), fact.scoped_holders.end(),
                  scoped) == fact.scoped_holders.end())
      fact.scoped_holders.push_back(std::move(scoped));
    (void)at;
  }
  bool alias_parameter(std::string_view name, InstrRef at) const {
    const auto owner = slot_scope(fn, name);
    const auto *row = owner && *owner ? verified_scope(*owner) : nullptr;
    if (!row)
      return false;
    return std::any_of(
        row->parameters.begin(), row->parameters.end(), [&](const auto &p) {
          return p.storage == name && p.at == at && (p.borrowed || p.writable);
        });
  }
  bool synthetic_reference(InstrRef at, std::string_view name) const {
    // Captured-empty authored rebind events distinguish lowered iteration
    // aliases from a source-level persistent reference reassignment.
    const auto *row = context(at);
    const auto owner = slot_scope(fn, name);
    if (!row || !row->source.availability.rebind_events || !owner ||
        *owner != row->scope)
      return false;
    const auto &frame = inline_scope(fn, row->scope);
    if (std::none_of(frame.slots.begin(), frame.slots.end(),
                     [&](const auto &slot) {
                       return slot.name == name &&
                              slot.kind == InlineSlotKind::PinnedReference;
                     }))
      return false;
    return std::none_of(
        row->source.rebind_events.begin(), row->source.rebind_events.end(),
        [&](const auto &event) {
          return event.kind == BindingRebindEvent::Kind::Reference &&
                 event.at == at && event.local == name;
        });
  }
  bool result_transfer(std::string_view name, InstrRef at) const {
    const auto rows = sources.scopes.find(fn.name);
    if (rows == sources.scopes.end())
      return false;
    for (const auto &[id, row] : rows->second) {
      (void)id;
      if (!verified_scope(row.scope) || !row.returns_complete)
        continue;
      for (const auto &ret : row.returns)
        if (ret.load && *ret.load == at && ret.storage &&
            *ret.storage == name && ret.load_move_out)
          return true;
    }
    return false;
  }
  void holder_write(InstrRef at, const ObjectIdentityFact &fact) {
    if (fact.object && fact.object_type.kind == TypeKind::Class)
      event(at, fact.object, ObjectBindingGap::HolderWrite);
    else if (fact.type.kind == TypeKind::Class ||
             fact.object_type.kind == TypeKind::Class)
      event(at, {}, ObjectBindingGap::HolderWrite);
  }
  bool cleanup_unknown(const InlineScope &scope, const Env &env) const {
    for (const auto &slot : scope.slots) {
      if (slot.kind == InlineSlotKind::OwnedLocal &&
          !env.empty_locals.contains(slot.name) &&
          unknown_cleanup_type(slot.type))
        return true;
      if (slot.kind == InlineSlotKind::PinnedReference &&
          !env.empty_references.contains(slot.name) &&
          unknown_cleanup_type(slot.type))
        return true;
    }
    // No node shadow ownership theorem is supplied by source/binding metadata.
    if (scope.node_cleanup && inline_cleanup_unknown(fn, scope))
      return true;
    return false;
  }
  void end_scope(Env &env, InstrRef at, std::uint64_t id) {
    const auto erase = [&](auto &fact) {
      std::erase_if(fact.scoped_holders,
                    [&](const auto &path) { return path.scope == id; });
    };
    for (auto &[value, fact] : env.values) {
      erase(fact);
      result.scopes[at.block].changes[value].emplace_back(at.index + 1, fact);
    }
    for (auto &[name, fact] : env.locals) {
      (void)name;
      erase(fact);
    }
    for (auto &[name, fact] : env.references) {
      (void)name;
      erase(fact);
    }
  }
  const InlineBirthState *returned_birth(InstrRef at) const {
    for (const auto &[scope, birth] : inline_contracts) {
      (void)scope;
      if (birth.witness.completion == at)
        return &birth;
    }
    return nullptr;
  }
  bool outer_result_owned(const ScopedBindingSource &row,
                          const InlineReturnBinding &ret) const {
    if (!ret.storage || !ret.store || !ret.load || !ret.output ||
        ret.store_borrowed || ret.store_replace_without_release ||
        !ret.load_move_out || ret.type != row.result_type)
      return false;
    const auto owner = slot_scope(fn, *ret.storage);
    if (!owner || *owner != row.parent)
      return false;
    if (*owner) {
      const auto &parent = inline_scope(fn, *owner);
      return std::any_of(
          parent.slots.begin(), parent.slots.end(), [&](const auto &s) {
            return s.name == *ret.storage && s.type == row.result_type &&
                   s.kind == InlineSlotKind::OwnedLocal;
          });
    }
    for (BlockId b = 0; b < fn.blocks.size(); ++b)
      for (std::uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i)
        if (const auto *decl =
                std::get_if<DeclareLocal>(&fn.blocks[b].instructions[i]);
            decl && decl->name == *ret.storage &&
            decl->type == row.result_type &&
            result.dominance.dominates(InstrRef{b, i}, row.begin))
          return true;
    return false;
  }
  std::map<std::uint64_t, InlineBirthState> births() {
    std::map<std::uint64_t, InlineBirthState> next;
    for (const auto &[id, ptr] : verified_scopes) {
      if (!work.take())
        return {};
      const auto &row = *ptr;
      if (!row.returns_complete || row.returns.empty() ||
          row.source.declaration.kind !=
              DeclarationIdentity::Kind::Constructor ||
          row.source.declaration.external)
        continue;
      // The owned original concrete ABI is verified against its exact target.
      const Function *target = nullptr;
      bool unique = true;
      for (const auto &f : module.functions) {
        if (!work.take())
          return {};
        if (f.name == row.source_function) {
          if (target)
            unique = false;
          target = &f;
        }
      }
      if (!unique || !target || !target->declaration ||
          !same_declaration(*target->declaration, row.source.declaration) ||
          target->declaration->generic_args !=
              row.source.declaration.generic_args ||
          target->result != row.result_type ||
          target->parameters.size() != row.parameters.size())
        continue;
      bool abi = true;
      for (std::size_t p = 0; p < row.parameters.size(); ++p) {
        if (!work.take())
          return {};
        const auto &formal = target->parameters[p];
        const auto &actual = row.parameters[p];
        const auto &frame = inline_scope(fn, id);
        const auto slot = std::find_if(
            frame.slots.begin(), frame.slots.end(),
            [&](const auto &s) { return s.name == actual.storage; });
        abi &= actual.ir_parameter == p && actual.type == formal.type &&
               actual.writable == formal.writable &&
               actual.borrowed == formal.borrowed &&
               slot != frame.slots.end() && slot->is_const == formal.is_const &&
               slot->target_escapes == formal.target_escapes;
      }
      if (!abi)
        continue;
      InlineBirthState candidate;
      candidate.witness.scope = id;
      candidate.witness.declaration = row.source.declaration;
      candidate.witness.result_type = row.result_type;
      candidate.witness.parameters = row.parameters;
      candidate.witness.returns = row.returns;
      bool first = true, bad = false, has_error = false;
      for (const auto &ret : row.returns) {
        if (!work.take())
          return {};
        if (ret.kind != InlineReturnBinding::Kind::Value ||
            !outer_result_owned(row, ret)) {
          bad = true;
          break;
        }
        const auto value = result.at(*ret.store, ret.value);
        if (value.error_return) {
          has_error = true;
          continue;
        }
        const auto *type = layout(sources, value.object_type);
        if (!value.available || !value.object || !value.allocation ||
            value.object->root != *value.allocation ||
            !value.object->steps.empty() ||
            value.allocation->kind != ObjectRoot::Allocation ||
            value.allocation->function != fn.name || !type || !type->complete ||
            !type->ordinary_value_layout || !exact_owner(row.source, *type) ||
            (value.type.kind == TypeKind::Union && !value.value_carrier) ||
            (value.type.kind != TypeKind::Union &&
             value.type != value.object_type)) {
          bad = true;
          break;
        }
        const auto allocation = value.allocation->at;
        if (allocation.block >= fn.blocks.size() ||
            allocation.index >=
                fn.blocks[allocation.block].instructions.size() ||
            !result.dominance.dominates(row.begin, allocation)) {
          bad = true;
          break;
        }
        const auto *make = std::get_if<ClassMake>(
            &fn.blocks[allocation.block].instructions[allocation.index]);
        const auto *frame = inline_binding_scope(fn, allocation.block);
        if (!make || make->type != value.object_type || !frame ||
            !descendant(fn, frame->identity, id)) {
          bad = true;
          break;
        }
        if (first) {
          candidate.value = value;
          candidate.witness.allocation = *value.allocation;
          candidate.witness.object_type = value.object_type;
          candidate.witness.completion = *ret.load;
          first = false;
        } else if (candidate.witness.allocation != *value.allocation ||
                   candidate.value.references != value.references ||
                   candidate.witness.object_type != value.object_type ||
                   candidate.witness.completion != *ret.load) {
          bad = true;
          break;
        }
        candidate.witness.successful_returns.push_back(*ret.store);
      }
      if (bad || first)
        continue;
      // A second possible returned-class allocation is not a unique birth.
      for (BlockId b = 0; b < fn.blocks.size() && !bad; ++b) {
        const auto *frame = inline_binding_scope(fn, b);
        if (!frame || !descendant(fn, frame->identity, id))
          continue;
        for (std::uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i) {
          if (!work.take())
            return {};
          if (const auto *make =
                  std::get_if<ClassMake>(&fn.blocks[b].instructions[i]);
              make && make->type == candidate.witness.object_type &&
              InstrRef{b, i} != candidate.witness.allocation.at)
            bad = true;
        }
      }
      for (const auto &event : result.events) {
        if (!work.take())
          return {};
        const auto *frame = inline_binding_scope(fn, event.at.block);
        if (!frame || !descendant(fn, frame->identity, id))
          continue;
        if (!event.object ||
            (event.object->root == candidate.witness.allocation &&
             (event.gap == ObjectBindingGap::Container ||
              event.gap == ObjectBindingGap::Escaped ||
              event.gap == ObjectBindingGap::ParallelCapture ||
              event.gap == ObjectBindingGap::UnboundedSharedWrite)))
          bad = true;
      }
      if (bad)
        continue;
      candidate.value.type = row.result_type;
      candidate.value.holders.clear();
      candidate.value.scoped_holders.clear();
      candidate.value.conditional_carrier = has_error;
      candidate.value.value_carrier =
          !has_error && row.result_type.kind == TypeKind::Union;
      next.emplace(id, std::move(candidate));
    }
    return next;
  }
  void instruction(const Instruction &instruction, Env &env, InstrRef at) {
    if (inline_binding_scope(fn, at.block) && !context(at))
      unknown(env, at, ObjectBindingGap::MissingOrigin);
    std::visit(
        [&](const auto &n) {
          using T = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<T, ConstantInt> ||
                        std::is_same_v<T, ConstantFloat> ||
                        std::is_same_v<T, ConstantExact>) {
            ObjectIdentityFact f;
            if (nonstructural_scalar(n.type))
              f.type = n.type;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, ConstantBool>) {
            ObjectIdentityFact f;
            f.type = Type::simple(TypeKind::Bool);
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, RootSerial> ||
                               std::is_same_v<T, FieldReferenceRoot>) {
            ObjectIdentityFact f;
            f.type = Type::simple(TypeKind::Nat64);
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, LoadLocal>) {
            const auto p = env.locals.find(n.name);
            auto f = p == env.locals.end() ? ObjectIdentityFact{} : p->second;
            if (const auto *birth = returned_birth(at)) {
              f = birth->value;
              if (f.object)
                env.fields[f.object->root] = f.references;
            }
            f.type = n.type;
            if (f.object && f.object->steps.empty())
              if (const auto refs = env.fields.find(f.object->root);
                  refs != env.fields.end())
                f.references = refs->second;
            put(n.out, f, env, at);
            if (n.move_out) {
              if (!result_transfer(n.name, at))
                holder_write(at, f);
              env.locals.erase(n.name);
              env.empty_locals.insert(n.name);
            }
          } else if constexpr (std::is_same_v<T, StoreLocal>) {
            auto f = get(n.value, env);
            if (const auto old = env.locals.find(n.name);
                old != env.locals.end() && old->second.object != f.object)
              event(at, old->second.object, ObjectBindingGap::HolderWrite);
            f.type = n.type;
            holder(f, n.name, at, alias_parameter(n.name, at));
            env.empty_locals.erase(n.name);
            env.locals.insert_or_assign(n.name, std::move(f));
          } else if constexpr (std::is_same_v<T, AddressLocal>) {
            const auto p = env.locals.find(n.name);
            auto f = p == env.locals.end() ? ObjectIdentityFact{} : p->second;
            f.address = true;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, InlineScopeCleanup>) {
            const auto &scope = inline_scope(fn, n.scope);
            if (cleanup_unknown(scope, env))
              unknown(env, at, ObjectBindingGap::UnboundedSharedWrite);
            for (const auto &slot : scope.slots)
              if (slot.kind == InlineSlotKind::OwnedLocal) {
                const auto p = env.locals.find(slot.name);
                if (p != env.locals.end()) {
                  holder_write(at, p->second);
                  env.locals.erase(p);
                  env.empty_locals.insert(slot.name);
                }
              }
          } else if constexpr (std::is_same_v<T, InlineScopeEnd>) {
            end_scope(env, at, n.scope);
          } else if constexpr (std::is_same_v<T, DeclareLocal>) {
            if (const auto p = env.locals.find(n.name); p != env.locals.end()) {
              holder_write(at, p->second);
              env.locals.erase(p);
            }
            env.empty_locals.insert(n.name);
          } else if constexpr (std::is_same_v<T, DeclareReference>) {
            if (const auto p = env.references.find(n.name);
                p != env.references.end()) {
              holder_write(at, p->second);
              env.references.erase(p);
            }
            env.empty_references.insert(n.name);
          } else if constexpr (std::is_same_v<T, InlineBindAddress>) {
            auto f = get(n.address, env);
            f.address = false;
            f.type = f.object_type = n.type;
            holder(f, n.name, at, alias_parameter(n.name, at));
            env.empty_locals.erase(n.name);
            env.locals.insert_or_assign(n.name, std::move(f));
          } else if constexpr (std::is_same_v<T, BindReference>) {
            auto f = get(n.address, env);
            if (const auto old = env.references.find(n.name);
                old != env.references.end() && old->second.object != f.object &&
                !synthetic_reference(at, n.name))
              holder_write(at, old->second);
            if (!n.address)
              env.empty_references.insert(n.name);
            else
              env.empty_references.erase(n.name);
            env.references.insert_or_assign(n.name, std::move(f));
          } else if constexpr (std::is_same_v<T, ReferenceAddress>) {
            const auto p = env.references.find(n.name);
            put(n.out,
                p == env.references.end() ? ObjectIdentityFact{} : p->second,
                env, at);
          } else if constexpr (std::is_same_v<T, LoadReference>) {
            const auto p = env.references.find(n.name);
            auto f =
                p == env.references.end() ? ObjectIdentityFact{} : p->second;
            f.address = false;
            f.type = n.type;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, LoadAddress>) {
            auto f = get(n.address, env);
            f.address = false;
            f.type = n.type;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, ClassMake>) {
            ObjectIdentityFact f;
            f.type = f.object_type = n.type;
            f.available =
                layout(sources, n.type) && layout(sources, n.type)->complete;
            f.allocation = root(ObjectRoot::Allocation, at, n.type);
            f.object = ObjectPath{*f.allocation, {}};
            env.fields[f.object->root] = {};
            for (const auto value : n.fields)
              if (value) {
                const auto child = get(*value, env);
                if (child.object && child.object_type.kind == TypeKind::Class)
                  event(at, child.object, ObjectBindingGap::Container);
              }
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, BindFieldReference>) {
            const auto object = get(n.object, env), value = get(n.address, env);
            const auto *c = layout(sources, object.object_type);
            if (object.available && object.object &&
                object.object->steps.empty() && c &&
                n.index < c->fields.size() && c->fields[n.index].reference &&
                value.available && value.address && value.object) {
              auto path = *object.object;
              path.steps.push_back(
                  semantics::PathStep::field_named(c->fields[n.index].name));
              event(at, path, ObjectBindingGap::BindingWrite);
              // A fresh result retained by another holder is no longer a
              // private allocation, even if its own reference fields are fixed.
              if (value.allocation && value.object->root != object.object->root)
                event(at, value.object, ObjectBindingGap::Escaped);
              env.fields[object.object->root].insert_or_assign(n.index,
                                                               *value.object);
              refresh(env, at);
            } else
              unknown(env, at, ObjectBindingGap::AmbiguousIdentity);
          } else if constexpr (std::is_same_v<T, FieldReferenceAddress>) {
            const auto object = get(n.object, env);
            ObjectIdentityFact f;
            if (const auto p = object.references.find(n.index);
                object.available && p != object.references.end()) {
              f.object = p->second;
              f.type = f.object_type = path_type(sources, p->second);
              f.available = f.type.kind != TypeKind::Invalid;
              f.address = true;
            }
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, FieldGet> ||
                               std::is_same_v<T, AddressField>) {
            const auto object = get(n.object, env);
            ObjectIdentityFact f;
            const auto *c = layout(sources, object.object_type);
            if (object.available && object.object && c && c->complete &&
                n.index < c->fields.size()) {
              if (c->fields[n.index].reference) {
                if (const auto p = object.references.find(n.index);
                    p != object.references.end())
                  f.object = p->second;
              } else {
                f.object = *object.object;
                f.object->steps.push_back(
                    semantics::PathStep::field_named(c->fields[n.index].name));
              }
              f.type = f.object_type = c->fields[n.index].type;
              f.available = f.object.has_value();
              f.address = std::is_same_v<T, AddressField>;
            }
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, FieldSet>) {
            const auto object = get(n.object, env);
            const auto *c = layout(sources, object.object_type);
            const auto physical =
                std::find_if(module.classes.begin(), module.classes.end(),
                             [&](const auto &v) {
                               return v.name == object.object_type.class_name;
                             });
            const bool mask =
                c && physical != module.classes.end() &&
                has_initialization_mask(*physical) &&
                initialization_mask_index(*physical) == n.index &&
                source_field_count(*physical) == c->fields.size() &&
                n.index < physical->fields.size() &&
                physical->fields[n.index].kind == TypeKind::Nat64 &&
                n.field_type.kind == TypeKind::Nat64 &&
                !physical->reference_fields.contains(n.index);
            if (mask) { /* Typed hidden initialization metadata cannot rebind a
                           field. */
            } else if (c && n.index < c->fields.size())
              write(env, at, object,
                    {semantics::PathStep::field_named(c->fields[n.index].name)},
                    ObjectBindingGap::BindingWrite);
            else
              unknown(env, at, ObjectBindingGap::MissingClass);
            const auto value = get(n.value, env);
            if (value.object && value.object_type.kind == TypeKind::Class)
              event(at, value.object, ObjectBindingGap::Container);
          } else if constexpr (std::is_same_v<T, Retain>) {
            auto f = get(n.value, env);
            f.type = n.type;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, Clone>) {
            auto f = get(n.value, env);
            f.type = f.object_type = n.type;
            const auto *c = layout(sources, n.type);
            if (n.type.kind == TypeKind::Class && f.available && c &&
                c->ordinary_value_layout) {
              f.allocation = root(ObjectRoot::Copy, at, n.type);
              f.object = ObjectPath{*f.allocation, {}};
              f.holders.clear();
              env.fields[f.object->root] = f.references;
            } else
              f.available = false;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, VariantMake>) {
            auto f = get(n.payload, env);
            f.type = n.container_type;
            f.error_return = n.payload_type.kind == TypeKind::Error;
            f.value_carrier =
                n.payload_type.kind == TypeKind::Class && f.available;
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, VariantPayload>) {
            auto f = get(n.container, env);
            f.type = n.payload_type;
            if (f.conditional_carrier) {
              if (n.payload_type == f.object_type) {
                f.conditional_carrier = false;
                f.value_carrier = true;
              } else {
                f.available = false;
                f.object.reset();
                f.allocation.reset();
                f.references.clear();
                f.value_carrier = false;
                f.error_return = n.payload_type.kind == TypeKind::Error;
              }
            }
            put(n.out, std::move(f), env, at);
          } else if constexpr (std::is_same_v<T, Call>)
            call(n, env, at);
          else if constexpr (std::is_same_v<T, IndirectCall>)
            unknown(env, at, ObjectBindingGap::UnknownCall);
          else if constexpr (std::is_same_v<T, ParallelBlock>)
            unknown(env, at, ObjectBindingGap::ParallelCapture);
          else if constexpr (std::is_same_v<T, StoreAddress>) {
            write(env, at, get(n.address, env), {},
                  ObjectBindingGap::HolderWrite);
          } else if constexpr (std::is_same_v<T, StoreReference>) {
            const auto p = env.references.find(n.name);
            if (p == env.references.end())
              unknown(env, at, ObjectBindingGap::AmbiguousIdentity);
            else
              write(env, at, p->second, {}, ObjectBindingGap::HolderWrite);
          } else if constexpr (std::is_same_v<T, ArrayMake> ||
                               std::is_same_v<T, TupleMake>) {
            for (const auto id : n.elements) {
              const auto f = get(id, env);
              if (f.object && f.object_type.kind == TypeKind::Class)
                event(at, f.object, ObjectBindingGap::Container);
            }
          } else if constexpr (std::is_same_v<T, ArraySet>) {
            const auto f = get(n.value, env);
            if (f.object && f.object_type.kind == TypeKind::Class)
              event(at, f.object, ObjectBindingGap::Container);
          } else if constexpr (std::is_same_v<T, Return>)
            result.returns.push_back(get(n.value, env));
        },
        instruction);
  }
  ObjectIdentityAnalysis run() {
    for (;;) {
      exits.assign(fn.blocks.size(), std::nullopt);
      result.complete = false;
      bool changed = true;
      while (changed) {
        changed = false;
        result.events.clear();
        result.returns.clear();
        for (const auto b : result.cfg.rpo) {
          if (!work.take())
            return std::move(result);
          auto env = incoming(b);
          auto &s = result.scopes[b];
          s.incoming = env.values;
          s.changes.clear();
          s.instruction_count =
              static_cast<std::uint32_t>(fn.blocks[b].instructions.size());
          for (std::uint32_t i = 0; i < s.instruction_count; ++i) {
            if (!work.take())
              return std::move(result);
            instruction(fn.blocks[b].instructions[i], env, {b, i});
          }
          if (!exits[b] || *exits[b] != env) {
            exits[b] = std::move(env);
            changed = true;
          }
        }
      }
      result.complete = true;
      auto next = births();
      if (work.exhausted) {
        result.complete = false;
        result.inline_births.clear();
        return std::move(result);
      }
      if (next == inline_contracts) {
        for (const auto &[id, state] : next) {
          (void)id;
          result.inline_births.push_back(state.witness);
        }
        return std::move(result);
      }
      inline_contracts = std::move(next);
    }
  }
};
void gap(ObjectBindingResult &result, ObjectBindingGap value) {
  if (std::find(result.gaps.begin(), result.gaps.end(), value) ==
      result.gaps.end())
    result.gaps.push_back(value);
}
bool constructor(const Function &fn, const BindingSources &sources,
                 const ObjectIdentityAnalysis &facts,
                 ConstructorIdentity &contract) {
  const auto *source = origin(sources, fn);
  if (!source || !source->complete ||
      source->declaration.kind != DeclarationIdentity::Kind::Constructor ||
      !facts.complete)
    return false;
  bool first = true;
  for (const auto &value : facts.returns) {
    if (value.error_return)
      continue;
    const auto *c = layout(sources, value.object_type);
    if (!value.available || !value.allocation || !c || !c->complete ||
        !exact_owner(*source, *c) || value.allocation->function != fn.name ||
        (value.allocation->kind != ObjectRoot::Allocation &&
         value.allocation->kind != ObjectRoot::Copy &&
         value.allocation->kind != ObjectRoot::ConstructorCall))
      return false;
    for (const auto &event : facts.events)
      if (event.object && event.object->root == *value.allocation &&
          (event.gap == ObjectBindingGap::Container ||
           event.gap == ObjectBindingGap::Escaped ||
           event.gap == ObjectBindingGap::ParallelCapture ||
           event.gap == ObjectBindingGap::UnboundedSharedWrite))
        return false;
    if (first) {
      contract.object_type = value.object_type;
      contract.references = value.references;
      first = false;
    } else if (contract.object_type != value.object_type ||
               contract.references != value.references)
      return false;
    for (const auto &[field, path] : value.references) {
      if (field >= c->fields.size() || !c->fields[field].reference ||
          path.root.kind != ObjectRoot::Parameter ||
          path.root.function != fn.name ||
          path.root.parameter >= fn.parameters.size())
        return false;
    }
  }
  if (first)
    return false;
  contract.declaration = source->declaration;
  contract.available = true;
  return true;
}
std::vector<bool> closure_window(InstrRef birth, const Cfg &cfg,
                                 const Loop &loop, Work &work) {
  std::vector<bool> forward(cfg.succ.size()), back(cfg.succ.size());
  std::deque<BlockId> queue;
  queue.push_back(birth.block);
  forward[birth.block] = true;
  while (!queue.empty()) {
    if (!work.take())
      return {};
    const auto b = queue.front();
    queue.pop_front();
    for (const auto s : cfg.succ[b])
      if (!forward[s]) {
        forward[s] = true;
        queue.push_back(s);
      }
  }
  queue.push_back(loop.header);
  back[loop.header] = true;
  while (!queue.empty()) {
    if (!work.take())
      return {};
    const auto b = queue.front();
    queue.pop_front();
    for (const auto p : cfg.pred[b])
      if (!back[p]) {
        back[p] = true;
        queue.push_back(p);
      }
  }
  std::vector<bool> result(cfg.succ.size());
  for (BlockId b = 0; b < result.size(); ++b)
    result[b] = forward[b] && back[b];
  for (const auto b : loop.blocks)
    result[b] = true;
  return result;
}
bool path_certificate(const Type &type,
                      const std::vector<semantics::PathStep> &path,
                      const Type &target, const BindingSources &sources,
                      const BindingProof &proof) {
  auto current = type;
  for (const auto &step : path) {
    if (step.kind == semantics::StepKind::Field) {
      const auto *c = layout(sources, current);
      if (!c || !c->complete)
        return false;
      const auto p =
          std::find_if(c->fields.begin(), c->fields.end(),
                       [&](const auto &f) { return f.name == step.field; });
      if (p == c->fields.end() || p->reference)
        return false;
      const auto field = static_cast<std::size_t>(p - c->fields.begin());
      const auto certificate = std::find_if(
          proof.fields.begin(), proof.fields.end(), [&](const auto &f) {
            return f.owner == c->identity && f.field == field;
          });
      // An escaped reference is not itself a field write. Class-level proof
      // availability is supplemental; the object-specific window below rejects
      // actual overlapping binding writers and every unbounded effect. A
      // present contradictory field record is stale metadata, not an alternate
      // origin.
      if (certificate != proof.fields.end() && certificate->name != p->name)
        return false;
      current = p->type;
    } else if (step.kind == semantics::StepKind::TupleElement &&
               current.kind == TypeKind::Tuple && step.index >= 0 &&
               static_cast<std::uint64_t>(step.index) <
                   current.parameters.size())
      current = current.parameters[static_cast<std::size_t>(step.index)];
    else
      return false;
  }
  return current == target;
}
} // namespace
bool InlineConstructorBirth::operator==(
    const InlineConstructorBirth &other) const {
  return declaration == other.declaration && scope == other.scope &&
         result_type == other.result_type && object_type == other.object_type &&
         allocation == other.allocation && completion == other.completion &&
         successful_returns == other.successful_returns &&
         canonical_encode_value(parameters).bytes ==
             canonical_encode_value(other.parameters).bytes &&
         canonical_encode_value(returns).bytes ==
             canonical_encode_value(other.returns).bytes;
}
ObjectIdentityFact ObjectIdentityAnalysis::at(InstrRef site,
                                              ValueId value) const {
  if (!complete || site.block >= scopes.size() || !cfg.reachable[site.block] ||
      site.index > scopes[site.block].instruction_count)
    return {};
  const auto d = definitions.find(value);
  if (d == definitions.end() || d->second == site ||
      !dominance.dominates(d->second, site))
    return {};
  const auto &s = scopes[site.block];
  if (const auto c = s.changes.find(value); c != s.changes.end()) {
    const auto end =
        std::upper_bound(c->second.begin(), c->second.end(), site.index,
                         [](auto i, const auto &p) { return i < p.first; });
    if (end != c->second.begin())
      return std::prev(end)->second;
  }
  const auto p = s.incoming.find(value);
  return p == s.incoming.end() ? ObjectIdentityFact{} : p->second;
}
ObjectIdentityAnalysis object_identities(
    const Module &module, std::size_t fn, const ModuleSummaries &summaries,
    const BindingSources &sources, const ConstructorIdentities &constructors,
    ObjectBindingOptions options) {
  Work work{0, options.maximum_operations, false};
  return Engine(module, fn, summaries, sources, constructors, work).run();
}
ObjectBindingReport
object_binding_closure(const Module &module, const ModuleSummaries &summaries,
                       const BindingSources &sources, const BindingProof &proof,
                       const std::vector<ObjectBindingRequest> &requests,
                       ObjectBindingOptions options) {
  ObjectBindingReport report;
  Work work{0, options.maximum_operations, false};
  std::map<std::string, std::size_t, std::less<>> names;
  for (const auto &fn : module.functions)
    ++names[fn.name];
  // Constructor facts are bottom-up proof contracts, never nominal births.
  bool changed = true;
  while (changed && !work.exhausted) {
    changed = false;
    for (std::size_t f = 0; f < module.functions.size(); ++f) {
      const auto *source = origin(sources, module.functions[f]);
      if (!source || names[module.functions[f].name] != 1 ||
          source->declaration.kind != DeclarationIdentity::Kind::Constructor ||
          report.constructors.contains(module.functions[f].name))
        continue;
      const auto facts =
          Engine(module, f, summaries, sources, report.constructors, work)
              .run();
      ConstructorIdentity contract;
      if (constructor(module.functions[f], sources, facts, contract)) {
        report.constructors.emplace(module.functions[f].name,
                                    std::move(contract));
        changed = true;
      }
    }
  }
  report.identities.reserve(module.functions.size());
  for (std::size_t f = 0; f < module.functions.size(); ++f) {
    if (work.exhausted)
      report.identities.emplace_back();
    else
      report.identities.push_back(
          Engine(module, f, summaries, sources, report.constructors, work)
              .run());
  }
  for (const auto &request : requests) {
    ObjectBindingResult result;
    result.request = request;
    if (!work.take()) {
      gap(result, ObjectBindingGap::Budget);
      report.results.push_back(std::move(result));
      continue;
    }
    if (request.function >= module.functions.size()) {
      gap(result, ObjectBindingGap::MissingOrigin);
      report.results.push_back(std::move(result));
      continue;
    }
    const auto &fn = module.functions[request.function];
    const auto &identities = report.identities[request.function];
    if (!identities.complete) {
      gap(result, work.exhausted ? ObjectBindingGap::Budget
                                 : ObjectBindingGap::MissingOrigin);
      report.results.push_back(std::move(result));
      continue;
    }
    if (request.loop >= identities.loops.loops.size() ||
        !identities.loops.loops[request.loop].available) {
      gap(result, ObjectBindingGap::LoopUnavailable);
      report.results.push_back(std::move(result));
      continue;
    }
    const auto &loop = identities.loops.loops[request.loop];
    const auto object = identities.at(request.use, request.object);
    if (!object.available || object.type.kind != TypeKind::Class ||
        object.conditional_carrier || !object.object || !object.allocation ||
        object.object->root != *object.allocation ||
        !object.object->steps.empty()) {
      gap(result, ObjectBindingGap::AmbiguousIdentity);
      report.results.push_back(std::move(result));
      continue;
    }
    std::optional<ScopedStoragePath> scoped_holder;
    if (object.holders.empty()) {
      for (const auto &path : object.scoped_holders) {
        if (!work.take())
          break;
        const auto *row = scope_row(module, sources, fn, path.scope);
        if (path.function != fn.name || !row ||
            !identities.dominance.dominates(row->begin,
                                            InstrRef{loop.header, 0}))
          continue;
        if (scoped_holder && *scoped_holder != path) {
          gap(result, ObjectBindingGap::MissingHolder);
          break;
        }
        scoped_holder = path;
      }
    }
    if (object.holders.size() > 1 ||
        (object.holders.empty() && !scoped_holder) || !result.gaps.empty()) {
      gap(result, ObjectBindingGap::MissingHolder);
      report.results.push_back(std::move(result));
      continue;
    }
    const auto birth = object.allocation->at;
    const InlineConstructorBirth *inline_birth = nullptr;
    for (const auto &candidate : identities.inline_births)
      if (candidate.allocation == *object.allocation) {
        if (inline_birth) {
          inline_birth = nullptr;
          break;
        }
        inline_birth = &candidate;
      }
    const bool call_birth =
        object.allocation->kind == ObjectRoot::ConstructorCall &&
        report.constructors.contains(object.allocation->constructor_function);
    const bool actual_birth =
        object.allocation->kind == ObjectRoot::Allocation && inline_birth;
    const auto completed = actual_birth ? inline_birth->completion : birth;
    if (object.allocation->function != fn.name ||
        (!call_birth && !actual_birth) ||
        !identities.dominance.dominates(birth, InstrRef{loop.header, 0}) ||
        !identities.dominance.dominates(completed, InstrRef{loop.header, 0})) {
      gap(result, ObjectBindingGap::BirthUnavailable);
      report.results.push_back(std::move(result));
      continue;
    }
    if (std::find(loop.blocks.begin(), loop.blocks.end(), birth.block) !=
        loop.blocks.end()) {
      gap(result, ObjectBindingGap::BirthInLoop);
      report.results.push_back(std::move(result));
      continue;
    }
    if (std::find(loop.blocks.begin(), loop.blocks.end(), request.use.block) ==
        loop.blocks.end()) {
      gap(result, ObjectBindingGap::LoopUnavailable);
      report.results.push_back(std::move(result));
      continue;
    }
    const auto *owner = layout(sources, object.object_type);
    std::optional<ObjectBindingFact> candidate;
    for (const auto &[field, target] : object.references) {
      if (!owner || field >= owner->fields.size() ||
          !owner->fields[field].reference || !owner->fields[field].is_private)
        continue;
      const auto model_type = path_type(sources, target);
      const auto *model = layout(sources, model_type);
      if (!model || !model->complete)
        continue;
      std::optional<std::vector<std::vector<semantics::PathStep>>> paths;
      std::vector<const FunctionBindingSource *> contexts;
      for (const auto &[name, source] : sources.functions) {
        (void)name;
        contexts.push_back(&source);
      }
      for (const auto &[host, rows] : sources.scopes) {
        const auto host_fn =
            std::find_if(module.functions.begin(), module.functions.end(),
                         [&](const auto &f) { return f.name == host; });
        if (host_fn == module.functions.end())
          continue;
        for (const auto &[id, row] : rows) {
          (void)id;
          if (!work.take())
            break;
          if (owned_scope(module, *host_fn, row))
            contexts.push_back(&row.source);
        }
      }
      for (const auto *source_ptr : contexts) {
        const auto &source = *source_ptr;
        if (!work.take()) {
          gap(result, ObjectBindingGap::Budget);
          break;
        }
        if (!source.complete || !source.availability.traversals)
          continue;
        for (const auto &traversal : source.traversals) {
          if (!work.take()) {
            gap(result, ObjectBindingGap::Budget);
            break;
          }
          if (traversal.root_type == model_type &&
              traversal.target == request.target) {
            if (!traversal.complete) {
              gap(result, ObjectBindingGap::TraversalUnavailable);
              continue;
            }
            if (paths && *paths != traversal.paths) {
              gap(result, ObjectBindingGap::TraversalUnavailable);
              continue;
            }
            paths = traversal.paths;
          }
        }
      }
      if (!paths)
        continue;
      bool closed = true;
      for (const auto &path : *paths)
        closed &=
            path_certificate(model_type, path, request.target, sources, proof);
      if (!closed) {
        gap(result, ObjectBindingGap::FieldClosureUnavailable);
        continue;
      }
      ObjectBindingFact fact;
      fact.allocation = *object.allocation;
      fact.loop_header = loop.header;
      fact.root_holder_available = !object.holders.empty();
      if (fact.root_holder_available)
        fact.holder = object.holders.front();
      fact.scoped_holder = scoped_holder;
      if (inline_birth)
        fact.inline_birth = *inline_birth;
      fact.model = target;
      fact.model_type = model_type;
      fact.model_identity = model->identity;
      fact.target = request.target;
      fact.reference_field = field;
      fact.paths = *paths;
      if (candidate) {
        gap(result, ObjectBindingGap::AmbiguousIdentity);
        continue;
      }
      candidate = std::move(fact);
    }
    if (!candidate) {
      gap(result, ObjectBindingGap::TraversalUnavailable);
      report.results.push_back(std::move(result));
      continue;
    }
    const auto window = closure_window(completed, identities.cfg, loop, work);
    if (window.empty()) {
      gap(result, ObjectBindingGap::Budget);
      report.results.push_back(std::move(result));
      continue;
    }
    auto in_window = [&](InstrRef site) {
      return site.block < window.size() && window[site.block] &&
             (site.block != completed.block || site.index > completed.index);
    };
    auto binding = *object.object;
    binding.steps.push_back(semantics::PathStep::field_named(
        owner->fields[candidate->reference_field].name));
    for (const auto *current_source :
         function_binding_sources(sources, fn.name)) {
      if (!current_source->complete ||
          !current_source->availability.rebind_events)
        continue;
      for (const auto &rebind : current_source->rebind_events) {
        if (!in_window(rebind.at))
          continue;
        if (rebind.at.block >= fn.blocks.size() ||
            rebind.at.index >= fn.blocks[rebind.at.block].instructions.size()) {
          gap(result, ObjectBindingGap::MissingOrigin);
          continue;
        }
        const auto &instruction =
            fn.blocks[rebind.at.block].instructions[rebind.at.index];
        if (rebind.kind == BindingRebindEvent::Kind::Field) {
          const auto *node = std::get_if<BindFieldReference>(&instruction);
          if (!node || node->object != rebind.object ||
              node->address != rebind.address || node->index != rebind.field) {
            gap(result, ObjectBindingGap::MissingOrigin);
            continue;
          }
          const auto target = identities.at(rebind.at, rebind.object);
          if (!target.available || !target.object) {
            gap(result, ObjectBindingGap::AmbiguousIdentity);
            continue;
          }
          if (may_prefix(*target.object, *object.object) ||
              may_prefix(*object.object, *target.object))
            gap(result, ObjectBindingGap::BindingWrite);
        } else {
          const auto *node = std::get_if<BindReference>(&instruction);
          if (!node || node->name != rebind.local ||
              node->address != rebind.address)
            gap(result, ObjectBindingGap::MissingOrigin);
        }
      }
    }
    for (const auto &event : identities.events) {
      if (!work.take()) {
        gap(result, ObjectBindingGap::Budget);
        break;
      }
      if (!in_window(event.at))
        continue;
      bool relevant = !event.object;
      if (event.object) {
        const auto &path = *event.object;
        if (event.gap == ObjectBindingGap::BindingWrite) {
          relevant = may_prefix(path, binding);
          for (const auto &leaf : candidate->paths) {
            auto target = candidate->model;
            target.steps.insert(target.steps.end(), leaf.begin(), leaf.end());
            relevant |= may_prefix(path, target);
          }
        } else
          relevant = may_prefix(path, *object.object) ||
                     may_prefix(*object.object, path) ||
                     may_prefix(path, candidate->model) ||
                     may_prefix(candidate->model, path);
      }
      if (relevant)
        gap(result, event.gap);
    }
    for (BlockId b = 0; b < fn.blocks.size(); ++b)
      for (std::uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i)
        if (const auto *call = std::get_if<Call>(&fn.blocks[b].instructions[i]);
            call && in_window({b, i})) {
          BindingCallEvidence evidence;
          evidence.function = fn.name;
          evidence.callee = call->callee;
          evidence.at = {b, i};
          const auto *summary = summaries.find(call->callee);
          const auto *target =
              exact_call_target(module, *call, [&](ValueId id) {
                return identities.at({b, i}, id);
              });
          evidence.effects_available =
              summary && summary->semantic && summary->effects_complete &&
              !(summary->effects & effect_bit::Opaque) &&
              summary->semantic_parameter_for_ir_parameter.size() ==
                  call->args.size();
          if (evidence.effects_available)
            for (std::size_t ir = 0; ir < call->args.size(); ++ir) {
              const auto mapped =
                  summary->semantic_parameter_for_ir_parameter[ir];
              if (!mapped && unmapped_scalar_argument(
                                 *call, ir, target, *summary, sources,
                                 identities.at({b, i}, call->args[ir].value)))
                continue;
              if (!mapped ||
                  *mapped >= summary->semantic->exposed_writes.size()) {
                evidence.effects_available = false;
                continue;
              }
              const auto &arg = call->args[ir];
              const auto identity = identities.at(
                  {b, i}, arg.writable_address.value_or(arg.value));
              for (const auto &path :
                   summary->semantic->exposed_writes[*mapped]) {
                if (!identity.available || !identity.object ||
                    path.root.kind != semantics::RootKind::Parameter ||
                    path.root.index != *mapped) {
                  evidence.effects_available = false;
                  continue;
                }
                auto actual = *identity.object;
                actual.steps.insert(actual.steps.end(), path.steps.begin(),
                                    path.steps.end());
                evidence.mapped_writes.push_back(std::move(actual));
              }
            }
          if (evidence.effects_available)
            for (const auto &[path, type] :
                 summary->semantic->reference_shared_write_origins) {
              (void)type;
              std::optional<std::size_t> actual;
              if (path.root.kind != semantics::RootKind::Parameter) {
                evidence.effects_available = false;
                continue;
              }
              for (std::size_t ir = 0;
                   ir < summary->semantic_parameter_for_ir_parameter.size();
                   ++ir)
                if (summary->semantic_parameter_for_ir_parameter[ir] ==
                    path.root.index) {
                  if (actual) {
                    actual.reset();
                    break;
                  }
                  actual = ir;
                }
              if (!actual) {
                evidence.effects_available = false;
                continue;
              }
              const auto &arg = call->args[*actual];
              const auto identity = identities.at(
                  {b, i}, arg.writable_address.value_or(arg.value));
              if (!identity.available || !identity.object) {
                evidence.effects_available = false;
                continue;
              }
              auto mapped = *identity.object;
              mapped.steps.insert(mapped.steps.end(), path.steps.begin(),
                                  path.steps.end());
              evidence.mapped_writes.push_back(std::move(mapped));
            }
          if (!evidence.effects_available)
            gap(result, ObjectBindingGap::MissingEffect);
          candidate->calls.push_back(std::move(evidence));
        }
    if (result.gaps.empty())
      result.fact = std::move(candidate);
    report.results.push_back(std::move(result));
  }
  report.operations = work.count;
  report.budget_exhausted = work.exhausted;
  return report;
}
} // namespace quidra::ir::analysis
