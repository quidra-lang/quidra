#include "ir/summaries.hpp"
#include "ir/analysis/summary_facts.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        Module module;
        Function worker; worker.name = "worker"; worker.parameters = {{"value", Type::simple(TypeKind::Int64)}, {"hidden", Type::simple(TypeKind::Int64)}};
        worker.blocks = {{"entry", {ReturnVoid{}}}};
        Function external; external.name = "external"; external.external_symbol = "native";
        module.functions = {worker, external};
        SummaryBindings bindings{{"worker", {"checked.worker", {0, std::nullopt}}}, {"external", {"checked.external", {}}}};
        auto snapshots = [&] {
            semantics::EffectSummaries checked;
            semantics::EffectSummary summary;
            summary.exposed_reads.resize(1); summary.exposed_writes.resize(1);
            summary.const_parameter_address_observed = {true};
            summary.reference_target_escapes = {true}; summary.reference_root_parameters = {true};
            summary.escaping_locals.insert("local");
            summary.result = {semantics::ResultProvenance::Kind::Fresh, 0};
            summary.resources.insert({semantics::ResourceKind::Stdout, semantics::ResourceUse::Exclusive, semantics::ResourceOrigin::Direct, {}});
            checked.set("checked.worker", summary); checked.set("checked.external", {});
            return adapt_summaries(module, checked, bindings);
        }();
        const auto* worker_summary = snapshots.find("worker");
        require(worker_summary && worker_summary->semantic && worker_summary->semantic->const_parameter_address_observed == std::vector<bool>{true}, "semantic snapshot must own the const-address facts after checker destruction");
        require(worker_summary->semantic->reference_target_escapes[0] && worker_summary->semantic->reference_root_parameters[0] &&
            worker_summary->semantic->escaping_locals.contains("local") && worker_summary->semantic->resources.size() == 1, "reference/resource facts survive exact semantic copy");
        require(worker_summary->semantic_parameter_for_ir_parameter == std::vector<std::optional<std::size_t>>{0, std::nullopt}, "hidden parameter remains explicitly unmapped");
        require(worker_summary->result == ResultAlias::Unknown && !worker_summary->result_complete && !worker_summary->pure_tensor, "semantic Fresh must not invent TensorStorage freshness or purity");
        const auto* unknown = snapshots.find("external");
        require(unknown && (unknown->effects & effect_bit::Opaque) && !unknown->effects_complete && !unknown->pure_tensor, "unknown extern is opaque despite an empty semantic declaration");
        bindings.at("worker").semantic_parameter_for_ir_parameter[0] = 1;
        semantics::EffectSummaries checked; semantics::EffectSummary summary; summary.exposed_reads.resize(1); checked.set("checked.worker", summary);
        bool rejected = false;
        try { (void)adapt_summaries(module, checked, bindings); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid source parameter mapping must reject");
        Module calls;
        Function caller; caller.name = "caller";
        caller.blocks = {{"entry", {Call{1, "callee", {}, Type::simple(TypeKind::Void)}, ReturnVoid{}}}};
        Function callee; callee.name = "callee"; callee.blocks = {{"entry", {ReturnVoid{}}}};
        calls.functions = {caller, callee};
        SummaryBindings call_bindings{{"caller", {"checked.caller", {}}}, {"callee", {"checked.callee", {}}}};
        IrSummaryInputs facts{{"caller", {ResultAlias::Unknown, {}, false, false, true, 0, true}},
                             {"callee", {ResultAlias::Unknown, {}, false, true, false, effect_bit::DeviceWork, true}}};
        auto propagated = [&] {
            semantics::EffectSummaries call_semantics;
            semantics::EffectSummary caller_semantics; caller_semantics.escaping_locals.insert("kept");
            call_semantics.set("checked.caller", caller_semantics); call_semantics.set("checked.callee", {});
            auto imported = adapt_summaries(calls, call_semantics, call_bindings);
            auto enriched = apply_ir_summary_facts(calls, imported, facts);
            require(!imported.find("caller")->effects_complete,
                "enrichment leaves raw imports unchanged");
            require(enriched.find("caller")->semantic == imported.find("caller")->semantic,
                "enrichment shares immutable semantic rows rather than copying heavy snapshots twice");
            return enriched;
        }();
        const auto* propagated_caller = propagated.find("caller");
        require(propagated_caller && propagated_caller->effects_complete && propagated_caller->descriptor_complete &&
            propagated_caller->pure_tensor && propagated_caller->semantic->escaping_locals.contains("kept"),
            "enrichment uses supplied facts without analyzing callees or altering raw imports");
        const auto* propagated_callee = propagated.find("callee");
        require(propagated_callee && (propagated_callee->effects & effect_bit::DeviceWork) &&
            propagated_callee->effects_complete && propagated_callee->descriptor_effects &&
            !propagated_callee->descriptor_complete && !propagated_callee->pure_tensor,
            "typed incomplete descriptor facts are preserved without inventing purity");

        Module scopes;Function dropped;dropped.name="dropped";InlineScope frame;frame.identity=2;frame.blocks={"entry"};
        frame.slots={{"object",Type::class_type("arbitrary_drop"),InlineSlotKind::OwnedLocal}};dropped.inline_scopes={frame};
        dropped.blocks={{"entry",{InlineScopeBegin{2},InlineScopeCleanup{2},InlineScopeEnd{2},ReturnVoid{}}}};
        Function primitive=dropped;primitive.name="primitive";primitive.inline_scopes[0].slots[0].type=Type::simple(TypeKind::Int64);
        Function exiting=primitive;exiting.name="exiting";exiting.blocks[0].instructions.back()=InlineExit{2,1};
        scopes.functions={dropped,primitive,exiting};semantics::EffectSummaries scoped_semantics;
        scoped_semantics.set("dropped",{});scoped_semantics.set("primitive",{});scoped_semantics.set("exiting",{});
        auto imported_scopes=adapt_summaries(scopes,scoped_semantics,{{"dropped",{"dropped",{}}},{"primitive",{"primitive",{}}},{"exiting",{"exiting",{}}}});
        auto scoped_effects=infer_ir_summary_facts(scopes,imported_scopes);
        require(!scoped_effects.at("dropped").effects_complete && !scoped_effects.at("dropped").descriptor_complete,
            "unknown scope user drop cannot be serialized as complete pure effects");
        require(scoped_effects.at("primitive").effects_complete && !(scoped_effects.at("primitive").effects & effect_bit::Opaque),
            "primitive-only scope cleanup retains its known no-user-drop contract");
        require(scoped_effects.at("exiting").effects & effect_bit::ExternalIO,"InlineExit keeps process-exit effects");
        std::puts("IR summaries: owned semantic facts, exact mapping and opaque contracts verified");
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
