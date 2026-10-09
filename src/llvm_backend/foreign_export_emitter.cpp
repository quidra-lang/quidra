#include "llvm_backend/foreign_export_emitter.hpp"

#include "llvm_backend/foreign_abi.hpp"
#include "llvm_backend/module_context.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

std::string emit_foreign_export(const ir::Function& function, const std::string& callee_symbol,
                                bool device_reaching) {
    FunctionText text;
    LlvmBuilder builder(text);
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
    builder.define(entry);
    builder.block("entry");
    builder.call(runtime_abi::program::register_sources,
                 {{ptr, LlvmOperand::global(source_table_symbol)}});
    const bool has_result = function.result.kind != TypeKind::Void;
    LlvmCall call(result, LlvmOperand::global(callee_symbol));
    if (has_result) call.result("%export.result");
    for (std::size_t i = 0; i < function.parameters.size(); ++i)
        call.argument({llvm_type(function.parameters[i].type), arguments[i]});
    builder.call(call);
    if (device_reaching) builder.call(runtime_abi::program::export_leave, {});
    if (has_result)
        builder.ret({result, "%export.result"});
    else
        builder.ret_void();
    builder.end_function();
    return text.str();
}

} // namespace quidra::llvm_backend
