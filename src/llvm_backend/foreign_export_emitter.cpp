#include "llvm_backend/foreign_export_emitter.hpp"

#include "llvm_backend/foreign_abi.hpp"
#include "llvm_backend/module_context.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "quidra/numeric_types.hpp"

#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

std::string emit_foreign_export(const ir::Function& function, const std::string& callee_symbol,
                                bool device_reaching, const ModuleContext& context,
                                CallSiteMetadata* call_sites, std::optional<std::uint32_t> entry_id) {
    const auto source = context.user_source_index.find(function.source_file);
    const bool user = source != context.user_source_index.end();
    const unsigned long long line =
        user ? (static_cast<unsigned long long>(source->second) << 32) | function.source_line : 0;
    const unsigned long long column = user ? function.source_column : 0;
    FunctionText text;
    LlvmBuilder builder(text);
    std::optional<std::size_t> subprogram;
    std::optional<FunctionCallSites> sites;
    if (call_sites && entry_id && call_sites->mode() == CallSiteMetadata::Mode::release) {
        subprogram = call_sites->subprogram(*entry_id);
        sites.emplace(*call_sites, context, nullptr, *entry_id, *subprogram, 0, 0);
    }
    const auto result = llvm_type(function.result);
    LlvmFunction entry(result, "@" + *function.c_export_symbol);
    entry.result_attributes(export_return_attribute(function.result));
    // The parameter names, alive until the call is written. A Quidra name
    // has no '.', so none of them is the result's name.
    std::vector<std::string> arguments;
    arguments.reserve(function.parameters.size());
    for (const auto& parameter : function.parameters) {
        arguments.push_back("%" + parameter.name);
        entry.parameter(llvm_type(parameter.type), export_parameter_attribute(parameter.type),
                        arguments.back());
    }
    if (subprogram) entry.debug_subprogram(*subprogram);
    builder.define(entry);
    builder.block("entry");
    if (sites) builder.locate_calls(&*sites);
    builder.call(runtime_abi::program::register_sources,
                 {{ptr, LlvmOperand::global(source_table_symbol)}});
    // A real argument from C must not be NaN. Infinities are valid.
    // The labels and values are named after the
    // parameter, whose Quidra name has no '.'.
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        const auto& parameter = function.parameters[i];
        if (!is_fixed_real(parameter.type)) continue;
        const auto type = llvm_type(parameter.type);
        const auto bad = "%" + parameter.name + ".nonfinite";
        const auto fail = parameter.name + ".nonfinite.fail";
        const auto ok = parameter.name + ".finite";
        builder.fcmp(bad, FloatPredicate::uno, type, arguments[i], arguments[i]);
        builder.br(bad, fail, ok);
        builder.block(fail);
        builder.call(runtime_abi::failure::conversion_fail,
                     {{i32, abi::dtype_code(*numeric_kind_info(parameter.type.kind)->dtype)},
                      {i32, static_cast<int>(abi::ConversionSubject::value)},
                      {i32, static_cast<int>(abi::ConversionReason::non_finite)},
                      {i64, line},
                      {i64, column}});
        builder.unreachable();
        builder.block(ok);
    }
    const bool has_result = function.result.kind != TypeKind::Void;
    LlvmCall call(result, LlvmOperand::global(callee_symbol));
    if (has_result) call.result("%export.result");
    for (std::size_t i = 0; i < function.parameters.size(); ++i)
        call.argument({llvm_type(function.parameters[i].type), arguments[i]});
    if (sites) sites->set_synthesized(true);
    builder.call(call);
    if (sites) sites->set_synthesized(false);
    if (device_reaching) builder.call(runtime_abi::program::export_leave, {});
    builder.locate_calls(nullptr);
    if (has_result)
        builder.ret({result, "%export.result"});
    else
        builder.ret_void();
    builder.end_function();
    return text.str();
}

} // namespace quidra::llvm_backend
