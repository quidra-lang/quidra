#include "ir/clone_contracts.hpp"
#include "ir/summaries.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool issue(const CloneContractResult& result, CloneUnavailable reason, std::string_view function) {
    for (const auto& row : result.unavailable)
        if (row.reason == reason && row.function == function) return true;
    return false;
}
void essential_bindings() {
    Module module;
    const auto tensor = Type::tensor(Type::simple(TypeKind::Real32));
    Function source;
    source.name = "source";
    source.parameters = {{"value",tensor,false,true,false,false},
                         {"hidden",Type::simple(TypeKind::Int64)}};
    source.result = tensor;
    source.c_export_symbol = "source_export";
    source.blocks = {{"entry",{LoadLocal{1,"value",tensor},Return{1,tensor}}}};
    module.functions.push_back(source);
    auto imported = [&] {
        semantics::EffectSummaries checked;
        semantics::EffectSummary semantic;
        semantic.exposed_reads.resize(1);
        semantic.exposed_writes.resize(1);
        semantic.reference_target_escapes = {true};
        semantic.escaping_locals.insert("owned.source.local");
        semantic.result = {semantics::ResultProvenance::Kind::Parameter,0};
        checked.set("original.checked.instance",semantic);
        auto rows = adapt_summaries(module,checked,{{"source",{"original.checked.instance",{0,std::nullopt}}}});
        return apply_ir_summary_facts(module,rows,{{"source",
            {ResultAlias::ParamDerived,0,true,false,true,effect_bit::DeviceWork,true}}});
    }();
    const auto* semantic = imported.find("source")->semantic;
    module.summaries = std::make_shared<const ModuleSummaries>(std::move(imported));
    auto source_facts = std::make_shared<facts::FactSources>();
    ExtentOrigin extent;
    extent.kind = ExtentOrigin::Kind::ParameterAxis;
    extent.parameter = 0;
    extent.axis = 1;
    source_facts->functions.emplace("source",facts::FunctionFactSource{{extent},{0,std::nullopt}});
    source_facts->policies.setters["source"] = {{"policy.identity","mode"}};
    source_facts->policies.writes_policy["source"] = true;
    module.fact_sources = source_facts;
    for (const auto* name : {"default.copy","unchanged.copy","changed.copy","bad.abi","missing.source","duplicate.copy"}) {
        auto copy = source;
        copy.name = name;
        copy.c_export_symbol.reset();
        copy.source_file = "relocated.qui";
        copy.source_line = 40;
        copy.next_value = 90;
        module.functions.push_back(std::move(copy));
    }
    module.functions[3].blocks[0].instructions = {LoadLocal{1,"value",tensor},Clone{2,1,tensor},Return{2,tensor}};
    module.functions[4].parameters[0].borrowed = false;
    Function generated; generated.name = "dispatch"; generated.blocks = {{"entry",{ReturnVoid{}}}};
    module.functions.push_back(generated);
    Function omitted = generated; omitted.name = "initial.omission"; module.functions.push_back(omitted);
    const std::vector<CloneReference> clones{
        {"default.copy","source"},
        {"unchanged.copy","source",CloneIrProofs::PreserveUnchangedBody},
        {"changed.copy","source",CloneIrProofs::PreserveUnchangedBody},
        {"bad.abi","source"},
        {"missing.source","absent"},
        {"duplicate.copy","source"},{"duplicate.copy","source"}};
    const std::vector<SynthesizedFunctionReference> generated_refs{{"dispatch"}};
    auto result = bind_clone_contracts(module,clones,generated_refs);
    const auto* copy = result.summaries->find("default.copy");
    require(copy && copy->semantic == semantic && copy->semantic_key == "original.checked.instance" &&
            copy->semantic_parameter_for_ir_parameter == std::vector<std::optional<std::size_t>>{0,std::nullopt},
            "clone lost explicit source semantic binding or hidden formal coordinates");
    require(copy->result == ResultAlias::Unknown && !copy->effects_complete && !copy->pure_tensor,
            "default clone inherited stale IR storage/effect proofs");
    const auto* unchanged = result.summaries->find("unchanged.copy");
    require(unchanged && unchanged->result_complete && unchanged->result == ResultAlias::ParamDerived &&
            unchanged->result_parameter == 0 && unchanged->effects_complete,
            "unchanged-body clone lost explicitly retained IR facts");
    require(issue(result,CloneUnavailable::ChangedBody,"changed.copy") &&
            !result.summaries->find("changed.copy")->effects_complete,
            "changed body inherited an unchanged-body proof");
    require(issue(result,CloneUnavailable::AbiMismatch,"bad.abi") &&
            !result.summaries->find("bad.abi")->semantic && !result.fact_sources->find("bad.abi"),
            "borrow ABI mismatch inherited source semantic/shape contracts");
    require(issue(result,CloneUnavailable::MissingFunction,"missing.source") &&
            issue(result,CloneUnavailable::DuplicateReference,"duplicate.copy"),
            "missing or duplicate explicit sources were silently accepted");
    require(issue(result,CloneUnavailable::InitialSummaryOmission,"initial.omission") &&
            !result.summaries->find("initial.omission"),
            "unlisted producer omission was repaired from a name");
    const auto* dispatch = result.summaries->find("dispatch");
    require(dispatch && !dispatch->semantic && !dispatch->effects_complete &&
            (dispatch->effects & effect_bit::Opaque) && !dispatch->result_complete && !dispatch->pure_tensor &&
            !result.fact_sources->find("dispatch"),
            "authorized synthesized function fabricated semantic or S1 proofs");
    const auto* shape = result.fact_sources->find("default.copy");
    require(shape && shape->result_extents == std::vector<ExtentOrigin>{extent} &&
            shape->semantic_parameter_for_ir_parameter == copy->semantic_parameter_for_ir_parameter &&
            result.fact_sources->policies.setters.at("default.copy") == source_facts->policies.setters.at("source") &&
            result.fact_sources->policies.writes_policy.at("default.copy"),
            "owned S1 or policy contracts were lost");
    source_facts->functions.at("source").result_extents.clear();
    source_facts->policies.setters.at("source")[0].value = "changed.original";
    module.fact_sources.reset(); module.summaries.reset();
    require(copy->semantic->reference_target_escapes[0] &&
            copy->semantic->escaping_locals.contains("owned.source.local") &&
            shape->result_extents.size() == 1 && result.fact_sources->policies.setters.at("default.copy")[0].value == "mode",
            "clone contracts lost ownership after original snapshots were released");
    module.summaries = result.summaries;
    module.functions.pop_back(); // The unlisted initial omission remains a hard error, not a fallback.
    auto refreshed = apply_ir_summary_facts(module,*module.summaries,{{"default.copy",
        {ResultAlias::Fresh,std::nullopt,true,false,true,effect_bit::Alloc,true}}});
    require(refreshed.find("default.copy")->result == ResultAlias::Fresh &&
            refreshed.find("default.copy")->semantic == copy->semantic,
            "normal refresh could not replace clone IR proofs while retaining source ownership");
}
} // namespace
int main() {
    try { essential_bindings(); std::cout << "Clone contracts: explicit owned bindings and conservative synthesis verified\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
