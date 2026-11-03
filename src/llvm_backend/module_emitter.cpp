#include "llvm_backend/module_emitter.hpp"

#include "quidra/llvm_backend.hpp"
#include "ir/analysis/call_graph.hpp"
#include "ir/analysis/device_reach.hpp"
#include "llvm_backend/debug_records.hpp"
#include "llvm_backend/diagnostic_constants.hpp"
#include "llvm_backend/foreign_export_emitter.hpp"
#include "llvm_backend/finite_check_support.hpp"
#include "llvm_backend/half_precision.hpp"
#include "llvm_backend/complex_arithmetic.hpp"
#include "llvm_backend/real_power.hpp"
#include "llvm_backend/function_emitter.hpp"
#include "llvm_backend/parallel_node_emission.hpp"
#include "llvm_backend/runtime_prelude.hpp"
#include "llvm_backend/shape_variable_check.hpp"
#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/small_rational.hpp"
#include "llvm_backend/symbol_resolver.hpp"
#include "llvm_text/llvm_value.hpp"

#include "quidra/abi/symbols.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace quidra::llvm_backend {

using llvm_text::LlvmOperand;
using namespace llvm_text::types;
using Section = llvm_text::LlvmModule::Section;

std::string ModuleEmitter::emit() {
    analyze();
    assign_debug_ids();
    emit_functions();
    emit_foreign_exports();
    collect_array_casts_and_clones();
    emit_header();
    emit_prelude();
    emit_constants();
    emit_helpers();
    emit_metadata();
    auto text=text_.str();
    return call_sites_ ? call_sites_->resolve_tail_operands(std::move(text)) : std::move(text);
}

void ModuleEmitter::analyze() {
    for (const auto& c : module_.classes) context_.layouts[c.name] = c;
    for (const auto& f : module_.functions) if(f.external_symbol) context_.external_symbols[f.name]=*f.external_symbol;
    for (const auto& f : module_.functions) {
        if (f.entrypoint) continue;
        context_.callable_functions[f.name] = &f;
    }
    for(const auto& function:module_.functions)if(function.parallel_node)
        context_.address_taken_functions.insert(function.name);
    context_.array_layout = collect_array_layout_policy(module_);
    context_.recursive_functions = ir::analysis::recursive_functions(module_);
    for (const auto& function : module_.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (const auto* ref = std::get_if<ir::FunctionRef>(&instruction))
                    context_.address_taken_functions.insert(ref->function);
    context_.node_cleanup_functions = ir::analysis::node_cleanup_functions(module_);
    context_.self_depth_recursive_functions =
        debug_info_ ? std::unordered_set<std::string>{}
                    : ir::analysis::self_depth_recursive_functions(module_,context_.recursive_functions,context_.address_taken_functions);
    for (std::size_t i = 0; i < module_.user_sources.size(); ++i)
        context_.user_source_index.emplace(module_.user_sources[i].absolute_path, i);
    for (const auto& f : module_.functions) {
        if (f.entrypoint) continue;
        if (f.external_symbol) {
            const bool callback = std::any_of(
                f.parameters.begin(), f.parameters.end(),
                [](const ir::Parameter& p) { return p.type.kind == TypeKind::Function; });
            context_.call_targets.emplace(
                *f.external_symbol,
                callback ? CallTarget::foreign_with_callback : CallTarget::foreign);
            continue;
        }
        const auto target = context_.user_source_index.contains(f.source_file)
                                ? CallTarget::user_function
                                : CallTarget::package_function;
        const auto symbol = abi::user_symbol(f.name);
        context_.call_targets.emplace(symbol, target);
        context_.call_targets.emplace(symbol + ".depth", target);
    }
    if (artifact_ != LlvmArtifact::none && tail_records_) {
        std::unordered_set<std::string> guarded;
        for (const auto& name : context_.recursive_functions)
            if (!context_.self_depth_recursive_functions.contains(name)) guarded.insert(name);
        guarded.insert(context_.address_taken_functions.begin(), context_.address_taken_functions.end());
        std::unordered_set<std::string> user_sources;
        for (const auto& [name, _] : context_.user_source_index) user_sources.insert(name);
        context_.tail_calls = ir::analysis::tail_calls(module_, guarded,
                                                      context_.address_taken_functions, user_sources);
    }
}

void ModuleEmitter::assign_debug_ids() {
    if(debug_info_) debug_.emplace(module_);
    if(artifact_!=LlvmArtifact::none)
        call_sites_.emplace(module_,context_,
                            debug_?CallSiteMetadata::Mode::debug:CallSiteMetadata::Mode::release,
                            artifact_==LlvmArtifact::library,debug_?&*debug_:nullptr);
}

void ModuleEmitter::emit_functions() {
    text_.append(Section::constants,emit_parallel_node_tables(module_,pool_));
    std::unordered_map<std::string, std::string> emitted_external_declarations;
    for (std::size_t i=0;i<module_.functions.size();++i) {
        const auto& f=module_.functions[i];
        const auto debug_file=debug_?debug_->file_id(i):0;
        const bool self_depth=context_.self_depth_recursive_functions.contains(f.name);
        auto emitted=emit_function(
            f, context_, pool_,
            (context_.recursive_functions.contains(f.name) || context_.address_taken_functions.contains(f.name)) && !self_depth,
            self_depth,
            debug_?&*debug_:nullptr,
            debug_?debug_->subprogram_id(i):std::nullopt,
            debug_file,
            call_sites_?&*call_sites_:nullptr,
            call_sites_?call_sites_->function_id(i):std::nullopt);
        if(const auto location=debug_?debug_->location_id(i):std::nullopt)
            emitted=attach_debug_location(std::move(emitted),*location);
        if (f.external_symbol) {
            const auto [existing, inserted] =
                emitted_external_declarations.emplace(*f.external_symbol, emitted);
            if (!inserted) {
                if (existing->second != emitted) {
                    throw std::logic_error(
                        "conflicting LLVM declarations for external C symbol '" +
                        *f.external_symbol + "'");
                }
                continue;
            }
        }
        text_.append(Section::bodies, emitted);
    }
}

// The C entry points, after every body, so that a module without exports is
// unchanged. Device reach decides which of them drain device work before
// they return to C.
void ModuleEmitter::emit_foreign_exports() {
    const bool exports = std::any_of(module_.functions.begin(), module_.functions.end(),
                                     [](const ir::Function& f) { return f.c_export_symbol.has_value(); });
    if (!exports) return;
    const auto device_reaching = ir::analysis::device_reaching_functions(module_);
    for (std::size_t i = 0; i < module_.functions.size(); ++i) {
        const auto& f = module_.functions[i];
        if (!f.c_export_symbol) continue;
        const SymbolResolver resolver(f, context_.external_symbols, false);
        std::optional<std::uint32_t> entry;
        if (call_sites_ && call_sites_->mode() == CallSiteMetadata::Mode::release)
            entry = call_sites_->export_entry_id(i, *f.c_export_symbol);
        text_.append(Section::bodies,
                     emit_foreign_export(f, resolver.call_symbol(f.name), device_reaching.contains(f.name),
                                         context_, call_sites_ ? &*call_sites_ : nullptr, entry));
    }
}

void ModuleEmitter::collect_array_casts_and_clones() {
    type_helpers_.collect_array_casts_and_clones();
}

void ModuleEmitter::emit_header() {
    DebugModule::emit_source_filename(module_,text_);
}

void ModuleEmitter::emit_prelude() {
    text_.append(Section::prelude, runtime_prelude_text());
    if(debug_&&debug_->has_variables())
        text_.append(Section::prelude, "declare void @llvm.dbg.declare(metadata, metadata, metadata)\n");
    text_.internal_global(".quidra.repl.replaying", {i1, "false"});
}

void ModuleEmitter::emit_constants() {
    emit_diagnostic_constants(text_);
    bool reference_fields=false;
    for(const auto& layout:module_.classes)if(!layout.reference_fields.empty()) {
        reference_fields=true;
        for(auto index:layout.reference_fields)
            text_.string_constant(".msg.reference.unbound."+type_identifier(Type::class_type(layout.name))+"."+std::to_string(index),"reference '"+layout.field_names[index]+"' is not bound");
    }
    if(reference_fields)text_.string_constant(".code.reference.unbound","UNBOUND_REFERENCE");
    // The source table's strings, interned before the pool is written: a
    // file's absolute path and revision are its provenance records' own
    // constants.
    struct TableStrings { std::string display, absolute, revision; };
    std::vector<TableStrings> table;
    table.reserve(module_.user_sources.size());
    for (const auto& source : module_.user_sources)
        table.push_back({pool_.intern(source.display_path), pool_.intern(source.absolute_path),
                         pool_.intern(source.revision)});
    for (const auto& [name, value] : pool_.entries) text_.string_constant(name, value);
    // The word of each big integer literal, made once at its first use.
    for (const auto& name : pool_.integer_literal_caches) text_.internal_global(name, {i64, "0"});
    for (const auto& [name, single] : pool_.exact_slots)
        text_.internal_global(name, {single ? float_type : double_type, "0.0"});
    // Each record is an abi::SourceProvenanceRecord, field by field.
    for (const auto& provenance : pool_.provenance_entries) {
        text_.private_constant(provenance.name,
                               {{ptr, LlvmOperand::global(provenance.file)},
                                {ptr, LlvmOperand::global(provenance.revision)},
                                {ptr, LlvmOperand::global(provenance.node_id)},
                                {ptr, LlvmOperand::global(provenance.node_kind)},
                                {i64, provenance.line},
                                {i64, provenance.column}});
    }
    // An abi::SourceTable: the count, then one abi::SourceTableEntry per user
    // file.
    constexpr std::string_view entry_type = "{ ptr, ptr, ptr, i64, i64 }";
    std::string text = "@" + std::string(source_table_symbol) + " = private constant { i64, [" +
                       std::to_string(table.size()) + " x " + std::string(entry_type) +
                       "] } { i64 " + std::to_string(table.size()) + ", [" +
                       std::to_string(table.size()) + " x " + std::string(entry_type) + "] ";
    if (table.empty()) {
        text += "zeroinitializer";
    } else {
        text += "[";
        for (std::size_t i = 0; i < table.size(); ++i) {
            const auto& source = module_.user_sources[i];
            if (i != 0) text += ", ";
            text += std::string(entry_type) + " { ptr @" + table[i].display + ", ptr @" +
                    table[i].absolute + ", ptr @" + table[i].revision + ", i64 " +
                    std::to_string(source.line_count) + ", i64 " +
                    std::to_string(source.byte_size) + " }";
        }
        text += "]";
    }
    text += " }\n";
    text_.append(Section::constants, text);
    // The constant error records of the module's error sites
    // (abi::ErrorRecord, quidra/abi/error_record.hpp) and the texts they
    // name.
    for (const auto& [name, value] : pool_.error_texts) text_.string_constant(name, value);
    for (const auto& [name, constant] : pool_.error_records)
        text_.append(Section::constants, "@" + name + " = private unnamed_addr constant " + constant + "\n");
    for (const auto& [name, constant] : pool_.hop_sites)
        text_.append(Section::constants, "@" + name + " = private unnamed_addr constant " + constant + "\n");
    if (call_sites_) call_sites_->emit_anchor_reference(text_);
}

void ModuleEmitter::emit_helpers() {
    type_helpers_.emit(text_);
    // The exact-real extension intrinsics' block names helpers of `real`, so
    // it comes before the support block of `real`.
    if (text_.contains(Section::bodies, exact_extension_helper_prefix))
        text_.append(Section::helpers, exact_extension_support_text());
    // A module that names a helper of the exact type `real` gets the
    // support block once, after the type helpers.
    if (text_.contains(Section::bodies, exact_real_helper_prefix) ||
        text_.contains(Section::helpers, exact_real_helper_prefix))
        text_.append(Section::helpers, exact_real_support_text());
    // Likewise for the arbitrary-precision integer words; the exact real
    // support block itself names their helpers, so this comes after it.
    if (text_.contains(Section::bodies, bare_integer_helper_prefix) ||
        text_.contains(Section::helpers, bare_integer_helper_prefix) ||
        text_.contains(Section::bodies, natural_helper_prefix))
        text_.append(Section::helpers, bare_integer_support_text());
    // The entries of the complex values. Before the 16-bit reals' block:
    // nothing in it names them, while complex code names their helpers.
    if (text_.contains(Section::bodies, complex_entry_prefix) ||
        text_.contains(Section::bodies, com_entry_prefix) ||
        text_.contains(Section::helpers, complex_entry_prefix) ||
        text_.contains(Section::helpers, com_entry_prefix))
        text_.append(Section::helpers, complex_support_text());
    // The helpers and entries of the 16-bit reals.
    if (text_.contains(Section::bodies, half_helper_prefix) ||
        text_.contains(Section::bodies, half_entry_prefix) ||
        text_.contains(Section::bodies, real16_entry_prefix) ||
        text_.contains(Section::bodies, real16b_entry_prefix) ||
        text_.contains(Section::helpers, half_helper_prefix) ||
        text_.contains(Section::helpers, half_entry_prefix) ||
        text_.contains(Section::helpers, real16_entry_prefix) ||
        text_.contains(Section::helpers, real16b_entry_prefix))
        text_.append(Section::helpers, half_support_text());
    // The power helpers of fixed-width reals, where `^` is used on them.
    if (text_.contains(Section::bodies, real_power_helper_prefix))
        text_.append(Section::helpers, real_power_support_text());
    // The failure entries of the finite checks.
    if (text_.contains(Section::bodies, finite_check_callee) ||
        text_.contains(Section::bodies, extern_check_callee) ||
        text_.contains(Section::bodies, package_check_callee))
        text_.append(Section::helpers, finite_check_support_text());
    // The check of axes that shape variables fix, where one is checked.
    if (text_.contains(Section::bodies, shape_variable_check_symbol))
        text_.append(Section::helpers, shape_variable_support_text());
}

void ModuleEmitter::emit_metadata() {
    if(debug_) debug_->emit_metadata(text_);
    if(call_sites_) call_sites_->emit_metadata(text_);
}

} // namespace quidra::llvm_backend

namespace quidra {

std::string emit_llvm(const ir::Module& module, bool debug_info) {
    return llvm_backend::ModuleEmitter(module, debug_info).emit();
}

LlvmEmission emit_native_llvm(const ir::Module& module, const LlvmEmitOptions& options) {
    llvm_backend::ModuleEmitter emitter(module, options.debug_info, options.artifact, options.tail_records);
    LlvmEmission emission;
    emission.text = emitter.emit();
    emission.call_sites = emitter.take_call_sites();
    return emission;
}

} // namespace quidra
