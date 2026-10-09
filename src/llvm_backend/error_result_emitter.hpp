#pragma once

// ErrorResultEmitter: recoverable results of the generated program, a
// T | error union box (UnionBox). void_result builds void | error from a
// success flag, string_result string | error from a string that is null on
// failure, output_result the void | error of print and flush from the stream
// status.
//
// Owns no state. Each result gives its IR value its type (SymbolTable) and
// allocates its labels and temporaries from the function's TemporaryNames,
// in an order fixed per helper.

#include "quidra/ir/module.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <string>

namespace quidra::llvm_backend {

class ErrorResultEmitter {
public:
    ErrorResultEmitter(llvm_text::LlvmBuilder& builder,TemporaryNames& names,SymbolTable& symbols,UnionBox& union_box)
        :builder_(builder),names_(names),symbols_(symbols),union_box_(union_box){}

    void void_result(ir::ValueId out_id, const Type& result_type,
                     const std::string& success, const std::string& prefix);

    void string_result(ir::ValueId out_id, const Type& result_type,
                       const std::string& raw, const std::string& prefix);

    // The void | error result of print and flush: `status` is a
    // nonzero i32 when the stream has failed.
    void output_result(ir::ValueId out_id, const Type& result_type, const std::string& status);

private:
    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    SymbolTable& symbols_;
    UnionBox& union_box_;
};

} // namespace quidra::llvm_backend
