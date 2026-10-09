#include "llvm_backend/self_depth_guard.hpp"

#include "quidra/abi/symbols.hpp"
#include "llvm_backend/type_lowering.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void SelfDepthGuard::emit_wrapper() {
    if(!self_depth_recursive_) return;
    const auto public_symbol=abi::user_symbol(fn_.name);
    const auto implementation_symbol=public_symbol+".depth";
    LlvmFunction wrapper(llvm_type(fn_.result),"@"+public_symbol);
    // The parameter names, alive until the call is written.
    std::vector<std::string> arguments;
    arguments.reserve(fn_.parameters.size());
    for(const auto& parameter:fn_.parameters) {
        arguments.push_back(symbols_.arg(parameter.name));
        wrapper.parameter(parameter.writable?ptr:llvm_type(parameter.type),"",arguments.back());
    }
    wrapper.attribute("alwaysinline");
    builder_.define(wrapper);
    builder_.block("entry");
    const bool has_result=
        fn_.result.kind!=TypeKind::Void&&fn_.result.kind!=TypeKind::Never;
    LlvmCall call(llvm_type(fn_.result),LlvmOperand::global(implementation_symbol));
    if(has_result) call.result("%depth.entry.result");
    for(std::size_t i=0;i<fn_.parameters.size();++i)
        call.argument({fn_.parameters[i].writable?ptr:llvm_type(fn_.parameters[i].type),arguments[i]});
    call.argument({i64,1});
    builder_.call(call);
    if(fn_.result.kind==TypeKind::Never) {
        builder_.unreachable();
    } else if(fn_.result.kind==TypeKind::Void) {
        builder_.ret_void();
    } else {
        builder_.ret({llvm_type(fn_.result),"%depth.entry.result"});
    }
    builder_.end_function();
}

} // namespace quidra::llvm_backend
