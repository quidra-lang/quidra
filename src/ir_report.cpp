#include "ir_report.hpp"
#include "ir/instruction_text.hpp"

#include <algorithm>
#include <concepts>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace quidra::cli {
namespace {
using namespace optimizer;
using namespace ir::analysis;
using ir::LocalRole;
using ir::SourcePosition;
using ir::facts::TensorFacts;
void quoted(std::string& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') { out += '\\'; out += character; }
        else if (byte < 32) { out += "\\u00"; out += hex[byte >> 4]; out += hex[byte & 15]; }
        else out += character;
    }
    out += '"';
}
void put(std::string& out, const std::string& value) { quoted(out, value); }
void put(std::string& out, const char* value) { quoted(out, value); }
void put(std::string& out, bool value) { out += value ? "true" : "false"; }
template<std::integral T> requires (!std::is_same_v<T, bool>)
void put(std::string& out, T value) { out += std::to_string(value); }
template<class T> void optional(std::string& out, const std::optional<T>& value) {
    if (value) put(out, *value); else out += "null";
}
struct Object {
    std::string& out;
    bool first{true};
    explicit Object(std::string& text) : out(text) { out += '{'; }
    template<class F> void field(std::string_view key, F write) {
        if (!first) out += ',';
        first = false; quoted(out, key); out += ':'; write(out);
    }
    template<class T> void value(std::string_view key, const T& item) {
        field(key, [&](auto& text) { put(text, item); });
    }
    void end() { out += '}'; }
};
template<class R, class F> void array(std::string& out, const R& items, F write) {
    out += '['; bool first = true;
    for (const auto& item : items) {
        if (!first) out += ',';
        first = false; write(out, item);
    }
    out += ']';
}
template<class R> void values(std::string& out, const R& items) {
    array(out, items, [](auto& text, const auto& item) { put(text, item); });
}
template<class E> [[noreturn]] const char* invalid_enum(E) {
    throw std::invalid_argument("IR report contains an unknown typed enumeration");
}
const char* tag(CheckKind value) {
    switch (value) {
    case CheckKind::ShapeConstraint: return "shape_constraint";
    case CheckKind::ExtentEquality: return "extent_equality";
    case CheckKind::Initialization: return "initialization";
    case CheckKind::ForStep: return "for_step";
    case CheckKind::ArrayBounds: return "array_bounds";
    case CheckKind::ArrayInitialization: return "array_initialization";
    case CheckKind::Overflow: return "overflow";
    case CheckKind::InlineInteger: return "inline_integer";
    case CheckKind::TensorBroadcast: return "tensor_broadcast";
    case CheckKind::TensorShape: return "tensor_shape";
    }
    return invalid_enum(value);
}
const char* tag(CheckAction value) {
    switch (value) {
    case CheckAction::Erase: return "erase";
    case CheckAction::BoundsFlag: return "bounds_flag";
    case CheckAction::InitializationFlag: return "initialization_flag";
    case CheckAction::OverflowFlag: return "overflow_flag";
    case CheckAction::InlineFlag: return "inline_flag";
    case CheckAction::BroadcastFlag: return "broadcast_flag";
    case CheckAction::ShapeFlag: return "shape_flag";
    }
    return invalid_enum(value);
}
const char* tag(ProofRule value) {
    switch (value) {
    case ProofRule::BeforeShape: return "before_shape";
    case ProofRule::CanonicalExtentIdentity: return "canonical_extent_identity";
    case ProofRule::SameSsaValue: return "same_ssa_value";
    case ProofRule::LiteralTrue: return "literal_true";
    case ProofRule::LiteralStepSign: return "literal_step_sign";
    case ProofRule::TrueGuard: return "true_guard";
    case ProofRule::EvaluatedIntegerRange: return "evaluated_integer_range";
    }
    return invalid_enum(value);
}
const char* tag(CheckGap value) {
    switch (value) {
    case CheckGap::None: return "none";
    case CheckGap::Unknown: return "unknown";
    case CheckGap::LiteralFailure: return "literal_failure";
    case CheckGap::NotMaterializable: return "not_materializable";
    case CheckGap::Unreachable: return "unreachable";
    case CheckGap::InvalidSsa: return "invalid_ssa";
    case CheckGap::Budget: return "budget";
    case CheckGap::PreimageMismatch: return "preimage_mismatch";
    case CheckGap::MissingPlan: return "missing_plan";
    }
    return invalid_enum(value);
}
const char* tag(RegionReason value) {
    switch (value) {
    case RegionReason::HostSync: return "host_sync";
    case RegionReason::DeviceDataRead: return "device_data_read";
    case RegionReason::ExternalIo: return "external_io";
    case RegionReason::PolicyWrite: return "policy_write";
    case RegionReason::OpaqueCall: return "opaque_call";
    case RegionReason::MissingKernel: return "missing_kernel";
    case RegionReason::UnsupportedInstruction: return "unsupported_instruction";
    case RegionReason::TrackedAdUnavailable: return "tracked_ad_unavailable";
    case RegionReason::TensorCompare: return "tensor_compare";
    case RegionReason::Parallel: return "parallel";
    case RegionReason::NonKeyExit: return "non_key_exit";
    case RegionReason::NestedLoop: return "nested_loop";
    case RegionReason::Irreducible: return "irreducible";
    case RegionReason::InfinitePaths: return "infinite_paths";
    case RegionReason::StateIdentityUnavailable: return "state_identity_unavailable";
    case RegionReason::ExclusiveWindowUnavailable: return "exclusive_window_unavailable";
    case RegionReason::OwnershipRoleUnavailable: return "ownership_role_unavailable";
    case RegionReason::InvalidSsa: return "invalid_ssa";
    case RegionReason::BindingUnavailable: return "binding_unavailable";
    case RegionReason::Budget: return "budget";
    }
    return invalid_enum(value);
}
const char* tag(RegionScope value) {
    switch (value) {
    case RegionScope::FunctionBody: return "function_body";
    case RegionScope::LoopIteration: return "loop_iteration";
    }
    return invalid_enum(value);
}
const char* tag(LocalRole value) {
    switch (value) {
    case LocalRole::Declaration: return "declaration";
    case LocalRole::Read: return "read";
    case LocalRole::Write: return "write";
    case LocalRole::Address: return "address";
    case LocalRole::ReadWrite: return "read_write";
    case LocalRole::ReferenceRead: return "reference_read";
    case LocalRole::ReferenceWrite: return "reference_write";
    }
    return invalid_enum(value);
}
const char* tag(StateSource::Kind value) {
    switch (value) {
    case StateSource::Kind::Parameter: return "parameter";
    case StateSource::Kind::Local: return "local";
    case StateSource::Kind::Field: return "field";
    case StateSource::Kind::Element: return "element";
    case StateSource::Kind::Reference: return "reference";
    case StateSource::Kind::Call: return "call";
    }
    return invalid_enum(value);
}
const char* tag(ReachingDef::Kind value) {
    switch (value) {
    case ReachingDef::Undefined: return "undefined";
    case ReachingDef::Param: return "parameter";
    case ReachingDef::Store: return "store";
    case ReachingDef::Phi: return "phi";
    case ReachingDef::Empty: return "empty";
    }
    return invalid_enum(value);
}
const char* tag(InspectionOrigin value) {
    switch (value) {
    case InspectionOrigin::Pass: return "pass";
    case InspectionOrigin::Collector: return "collector";
    }
    return invalid_enum(value);
}
const char* tag(ir::facts::BackendKind value) {
    switch (value) {
    case ir::facts::BackendKind::Unknown: return "unknown";
    case ir::facts::BackendKind::AnyGpu: return "any_gpu";
    case ir::facts::BackendKind::Cpu: return "cpu";
    case ir::facts::BackendKind::Cuda: return "cuda";
    case ir::facts::BackendKind::Hip: return "hip";
    case ir::facts::BackendKind::Metal: return "metal";
    case ir::facts::BackendKind::Test: return "test";
    }
    return invalid_enum(value);
}
void tri(std::string& out, ir::facts::Tri value) {
    switch (value) {
    case ir::facts::Tri::Unknown: out += "null"; return;
    case ir::facts::Tri::False: put(out, false); return;
    case ir::facts::Tri::True: put(out, true); return;
    }
    invalid_enum(value);
}
void extent(std::string& out, const ShapeExtent& item, std::size_t depth = 0) {
    if (depth >= 256) throw std::invalid_argument("IR report extent nesting exceeds 256");
    Object object(out);
    const char* kind;
    switch (item.kind) {
    case ShapeExtent::Kind::Unknown: kind = "unknown"; break;
    case ShapeExtent::Kind::Known: kind = "known"; break;
    case ShapeExtent::Kind::Symbol: kind = "symbol"; break;
    case ShapeExtent::Kind::Broadcast: kind = "broadcast"; break;
    case ShapeExtent::Kind::Expr: kind = "expression"; break;
    default: invalid_enum(item.kind);
    }
    object.value("kind", kind); object.value("value", item.value);
    object.field("symbols", [&](auto& text) { values(text, item.symbols); });
    object.field("terms", [&](auto& text) { array(text, item.terms, [](auto& target, const auto& term) {
        Object value(target); value.value("coefficient", term.coefficient.text());
        value.field("factors", [&](auto& target_text) { values(target_text, term.factors); }); value.end();
    }); });
    object.field("operands", [&](auto& text) { array(text, item.operands, [&](auto& target, const auto& value) { extent(target, value, depth + 1); }); });
    object.field("atom_operands", [&](auto& text) {
        Object atoms(text);
        for (const auto& [key, operands] : item.atom_operands)
            atoms.field(key, [&](auto& target) { array(target, operands, [&](auto& target_text, const auto& value) { extent(target_text, value, depth + 1); }); });
        atoms.end();
    });
    object.field("atom_operations", [&](auto& text) {
        Object atoms(text); for (const auto& [key, operation] : item.atom_operations) atoms.value(key, operation); atoms.end();
    }); object.end();
}
void tensor(std::string& out, const TensorFacts& fact) {
    Object object(out); object.value("has_type", fact.has_type); object.value("type", type_name(fact.type));
    object.field("type_rank", [&](auto& text) { if (fact.type.length >= 0) put(text, fact.type.length); else text += "null"; });
    object.field("device", [&](auto& text) {
        Object device(text); device.value("kind", tag(fact.device.kind));
        device.field("index", [&](auto& target) { optional(target, fact.device.index); }); device.end();
    });
    object.field("contiguous", [&](auto& text) { tri(text, fact.contiguous); });
    object.field("tracked", [&](auto& text) { tri(text, fact.tracked); });
    object.field("unique", [&](auto& text) { tri(text, fact.unique); });
    object.field("fully_initialized", [&](auto& text) { tri(text, fact.fully_initialized); });
    object.value("owns_storage_legacy", fact.owns_storage);
    object.field("extents", [&](auto& text) { array(text, fact.extents, [](auto& target, const auto& value) { extent(target, value); }); });
    object.field("head_extents", [&](auto& text) { array(text, fact.head_extents, [](auto& target, const auto& value) { extent(target, value); }); });
    object.field("tail_extents", [&](auto& text) { array(text, fact.tail_extents, [](auto& target, const auto& value) { extent(target, value); }); });
    object.value("minimum_rank", fact.minimum_rank);
    object.field("rank_extent", [&](auto& text) { extent(text, fact.rank_extent); }); object.end();
}
void reference(std::string& out, InstrRef at) {
    Object object(out); object.value("block", at.block); object.value("instruction", at.index); object.end();
}
void site(std::string& out, const ir::Function& context, InstrRef at) {
    const bool block_exists = at.block < context.blocks.size();
    const bool valid = block_exists && at.index <= context.blocks[at.block].instructions.size();
    Object object(out); object.value("block", at.block); object.value("instruction", at.index); object.value("valid", valid);
    object.field("block_label", [&](auto& text) { if (block_exists) put(text, context.blocks[at.block].label); else text += "null"; });
    object.field("display_source", [&](auto& text) {
        SourcePosition position; std::string file = context.source_file;
        if (valid && at.index < context.blocks[at.block].instructions.size()) {
            const auto& instructions = context.blocks[at.block].instructions;
            for (std::size_t i = 0; i <= at.index; ++i)
                if (const auto* location = std::get_if<ir::SourceLocation>(&instructions[i])) file = location->source_file;
            std::visit([&](const auto& node) {
                if constexpr (requires { node.position; })
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(node.position)>, SourcePosition>) position = node.position;
            }, instructions[at.index]);
        }
        if (!position.line) { text += "null"; return; }
        Object display(text); display.value("file", file); display.value("line", position.line); display.value("column", position.column); display.end();
    }); object.end();
}
void declaration(std::string& out, const std::optional<DeclarationIdentity>& identity) {
    if (!identity) { out += "null"; return; }
    Object object(out); object.value("key", declaration_key(*identity));
    object.value("package", identity->package); object.value("module", identity->module_path);
    object.value("source_name", identity->source_name); object.value("owner_class", identity->owner_class);
    object.field("generic_arguments", [&](auto& text) { values(text, identity->generic_args); });
    object.value("source_instance", identity->specialization_of); object.end();
}
template<class Row> void identity(Object& object, const Row& row) {
    object.value("function", row.function); object.value("lookup", row.lookup);
    object.field("declaration", [&](auto& text) { declaration(text, row.declaration); });
    object.value("round", row.round); object.value("origin", tag(row.origin));
    object.value("site_frame", row.origin == InspectionOrigin::Pass ? "pass_input" : "collector_input");
}
void premise(std::string& out, const ProofPremise& value) {
    Object object(out); const char* kind;
    switch (value.kind) {
    case ProofPremise::Kind::TensorBefore: kind = "tensor_before"; break;
    case ProofPremise::Kind::ExtentBefore: kind = "extent_before"; break;
    case ProofPremise::Kind::IntegerLiteral: kind = "integer_literal"; break;
    case ProofPremise::Kind::BooleanLiteral: kind = "boolean_literal"; break;
    default: invalid_enum(value.kind);
    }
    object.value("kind", kind); object.value("value", value.value);
    object.field("definition", [&](auto& text) { reference(text, value.definition); });
    object.field("tensor", [&](auto& text) { if (value.tensor) tensor(text, *value.tensor); else text += "null"; });
    object.field("extent", [&](auto& text) { if (value.extent) extent(text, *value.extent); else text += "null"; });
    object.field("magnitude", [&](auto& text) { if (value.kind == ProofPremise::Kind::IntegerLiteral) put(text, value.magnitude); else text += "null"; });
    object.field("negative", [&](auto& text) { if (value.kind == ProofPremise::Kind::IntegerLiteral) put(text, value.negative); else text += "null"; });
    object.field("boolean", [&](auto& text) { if (value.kind == ProofPremise::Kind::BooleanLiteral) put(text, value.boolean); else text += "null"; }); object.end();
}
const char* proof_decision(const ProofInspection& row) {
    if (row.application_observed && row.application && row.application->failure == CheckGap::PreimageMismatch) return "stale";
    if (row.report.failure == CheckGap::Budget || (row.application_observed && row.application && row.application->failure == CheckGap::Budget)) return "budget";
    if (!row.ready || row.report.failure != CheckGap::None) return "unavailable";
    if (!row.application_observed) return "certified_not_applied";
    if (!row.application) return "application_not_recorded";
    return row.application->applied ? "applied" : "not_applied";
}
template<class T> std::vector<const T*> rows(const std::vector<T>& source) {
    std::vector<const T*> result; result.reserve(source.size());
    for (const auto& row : source) result.push_back(&row);
    std::stable_sort(result.begin(), result.end(), [](const auto* a, const auto* b) {
        return std::tie(a->function, a->round, a->lookup, a->origin) < std::tie(b->function, b->round, b->lookup, b->origin);
    }); return result;
}
void proof(std::string& out, const ProofInspection& row) {
    Object object(out); identity(object, row); object.value("ready", row.ready);
    object.value("decision", proof_decision(row)); object.value("failure", tag(row.report.failure));
    object.value("operations", row.report.operations); object.value("planned_removed", row.report.removed); object.value("planned_flagged", row.report.flagged);
    object.value("application_observed", row.application_observed);
    object.field("application", [&](auto& text) {
        if (!row.application_observed || !row.application) { text += "null"; return; }
        Object application(text); application.value("applied", row.application->applied); application.value("failure", tag(row.application->failure));
        application.value("removed", row.application->removed); application.value("flagged", row.application->flagged);
        application.value("operations", row.application->operations); application.end();
    });
    std::vector<const ProofCertificate*> certificates;
    for (const auto& value : row.report.certificates) certificates.push_back(&value);
    std::stable_sort(certificates.begin(), certificates.end(), [](const auto* a, const auto* b) {
        return std::tie(a->at.block, a->at.index, a->kind, a->action, a->rule) < std::tie(b->at.block, b->at.index, b->kind, b->action, b->rule);
    });
    object.field("certificates", [&](auto& text) { array(text, certificates, [&](auto& target, const auto* value) {
        Object certificate(target); certificate.value("kind", tag(value->kind)); certificate.value("action", tag(value->action)); certificate.value("rule", tag(value->rule));
        certificate.value("decision", proof_decision(row)); certificate.field("site", [&](auto& target_text) { site(target_text, row.site_context, value->at); });
        certificate.value("original", ir::instruction_text(value->original)); certificate.value("raw_preimage_bytes", value->original_fields.size());
        certificate.field("premises", [&](auto& target_text) { array(target_text, value->premises, [](auto& destination, const auto& item) { premise(destination, item); }); }); certificate.end();
    }); });
    auto unavailable = row.report.unavailable;
    std::stable_sort(unavailable.begin(), unavailable.end(), [](const auto& a, const auto& b) {
        return std::tie(a.at.block, a.at.index, a.kind, a.gap) < std::tie(b.at.block, b.at.index, b.kind, b.gap);
    });
    object.field("unavailable", [&](auto& text) { array(text, unavailable, [&](auto& target, const auto& item) {
        Object value(target); value.value("kind", tag(item.kind)); value.value("reason", tag(item.gap));
        value.field("site", [&](auto& target_text) { site(target_text, row.site_context, item.at); }); value.end();
    }); }); object.end();
}
void reaching(std::string& out, const ReachingDef& item) {
    Object object(out); object.value("kind", tag(item.kind)); object.value("available", item.available);
    object.field("value", [&](auto& text) { if (item.kind == ReachingDef::Store) put(text, item.value); else text += "null"; });
    object.field("definition", [&](auto& text) { if (item.kind == ReachingDef::Store || item.kind == ReachingDef::Empty) reference(text, item.store); else text += "null"; });
    object.field("phi", [&](auto& text) { if (item.kind == ReachingDef::Phi) put(text, item.phi.value); else text += "null"; });
    object.field("parameter", [&](auto& text) { if (item.kind == ReachingDef::Param) put(text, item.parameter); else text += "null"; });
    object.field("empty_type", [&](auto& text) { if (item.kind == ReachingDef::Empty && item.empty_type) put(text, type_name(*item.empty_type)); else text += "null"; }); object.end();
}
void local(std::string& out, const RegionLocal& item) {
    Object object(out); object.value("name", item.name);
    object.field("roles", [&](auto& text) { array(text, item.roles, [](auto& target, const auto& role) { put(target, tag(role)); }); });
    object.field("incoming", [&](auto& text) { if (item.incoming) reaching(text, *item.incoming); else text += "null"; });
    object.field("outgoing", [&](auto& text) { if (item.outgoing) reaching(text, *item.outgoing); else text += "null"; });
    object.field("parameter", [&](auto& text) { optional(text, item.parameter); });
    object.field("type", [&](auto& text) { if (item.type) put(text, type_name(*item.type)); else text += "null"; });
    object.field("facts", [&](auto& text) {
        if (!item.facts) { text += "null"; return; }
        const auto& facts = *item.facts; Object value(text);
        value.value("writable_parameter", facts.writable_parameter); value.value("borrowed_parameter", facts.borrowed_parameter); value.value("borrowed_local", facts.borrowed_local);
        value.value("address_taken", facts.address_taken); value.value("reference_bound", facts.reference_bound); value.value("reference_binding", facts.reference_binding);
        value.value("move_out", facts.move_out); value.value("opaque_access", facts.opaque_access); value.end();
    }); object.end();
}
void state(std::string& out, const RegionState& item) {
    Object object(out); object.value("storage", item.storage); object.value("read", item.read); object.value("written", item.written);
    object.value("identity_available", item.identity_available); object.value("exclusive_window_available", item.exclusive_window_available);
    object.field("sources", [&](auto& text) { array(text, item.sources, [](auto& target, const auto& source) {
        Object value(target); value.value("kind", tag(source.kind)); value.value("local", source.local);
        value.field("parameter", [&](auto& destination) { optional(destination, source.parameter); });
        value.field("member", [&](auto& destination) { optional(destination, source.member); });
        value.value("identity_available", source.identity_available); value.end();
    }); }); object.end();
}
void regions(std::string& out, const StepRegionInspection& row) {
    Object object(out); identity(object, row); object.value("ssa_valid", row.report.ssa_valid);
    object.value("budget_exhausted", row.report.budget_exhausted); object.value("operations", row.report.operations);
    std::vector<const StepRegion*> ordered;
    for (const auto& value : row.report.regions) ordered.push_back(&value);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->id < b->id; });
    object.field("regions", [&](auto& text) { array(text, ordered, [&](auto& target, const auto* value) {
        Object region(target); region.value("id", value->id); region.value("scope", tag(value->scope));
        region.field("loop", [&](auto& target_text) { optional(target_text, value->loop); });
        region.field("entry", [&](auto& target_text) { site(target_text, row.site_context, value->entry); });
        region.field("exit", [&](auto& target_text) { if (value->exit) site(target_text, row.site_context, *value->exit); else target_text += "null"; });
        region.field("instructions", [&](auto& target_text) { array(target_text, value->instructions, [&](auto& destination, const auto& instruction) { site(destination, row.site_context, instruction); }); });
        region.field("inputs", [&](auto& target_text) { values(target_text, value->inputs); }); region.field("outputs", [&](auto& target_text) { values(target_text, value->outputs); });
        region.field("locals", [&](auto& target_text) { array(target_text, value->locals, [](auto& destination, const auto& item) { local(destination, item); }); });
        region.field("state", [&](auto& target_text) { array(target_text, value->state, [](auto& destination, const auto& item) { state(destination, item); }); });
        region.field("operations", [&](auto& target_text) { values(target_text, value->operations); }); region.field("kernels", [&](auto& target_text) { values(target_text, value->kernels); });
        region.value("boundaries_complete", value->boundaries_complete); region.value("finite_exit_only", value->finite_exit_only); region.value("internal_acyclic", value->internal_acyclic);
        region.value("physical_ownership_proven", value->physical_ownership_proven); region.end();
    }); });
    auto exclusions = row.report.exclusions;
    const auto key = [](const auto& value) { return std::tuple{value.at.has_value(), value.at ? value.at->block : 0U, value.at ? value.at->index : 0U, value.reason, value.loop}; };
    std::stable_sort(exclusions.begin(), exclusions.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    object.field("exclusions", [&](auto& text) { array(text, exclusions, [&](auto& target, const auto& item) {
        Object exclusion(target); exclusion.value("reason", tag(item.reason));
        exclusion.field("site", [&](auto& target_text) { if (item.at) site(target_text, row.site_context, *item.at); else target_text += "null"; });
        exclusion.field("loop", [&](auto& target_text) { optional(target_text, item.loop); }); exclusion.end();
    }); }); object.end();
}
} // namespace
std::string ir_proofs_report(const optimizer::OptimizerInspection& inspection) {
    std::string out; Object document(out); document.value("schema", "quidra.ir.proofs/1");
    document.value("pass_observed", inspection.proofs_observed);
    const auto ordered = rows(inspection.proofs);
    document.field("functions", [&](auto& text) { array(text, ordered, [](auto& target, const auto* row) { proof(target, *row); }); });
    document.end(); return out;
}
std::string ir_step_regions_report(const optimizer::OptimizerInspection& inspection) {
    std::string out; Object document(out); document.value("schema", "quidra.ir.step-regions/1");
    document.value("pass_observed", inspection.step_regions_observed);
    document.field("annotation", [&](auto& text) {
        Object annotation(text);
        annotation.value("ready", inspection.step_regions_ready);
        annotation.value("published", inspection.step_regions_published);
        annotation.field("failure", [&](auto& destination) { optional(destination, inspection.step_regions_gap); });
        annotation.value("operations", inspection.step_regions_operations);
        annotation.value("encoded_bytes", inspection.step_regions_bytes);
        annotation.value("upstream_budget_complete", inspection.step_regions_upstream_budget_complete);
        annotation.end();
    });
    const auto ordered = rows(inspection.step_regions);
    document.field("functions", [&](auto& text) { array(text, ordered, [](auto& target, const auto* row) { regions(target, *row); }); });
    document.end(); return out;
}
} // namespace quidra::cli
