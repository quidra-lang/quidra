#pragma once

// DebugEmitter: the debug information of one function: llvm.dbg.declare for
// each local and parameter that has a source name, and the statement
// locations, each attached to the text of the next instruction that writes
// any. It is also the emitter of the debug domain: a SourceLocation that
// names its source node sets the runtime's source provenance (a record in
// the module's StringPool) and opens the statement location.
//
// Owns: the variable id of each storage name (the first declaration wins),
// the statement location waiting for the next instruction, and the byte range
// of each instruction's text that carries a location (a segment). The
// segments are attached to the finished function text in reverse order, so
// the byte offsets of the earlier ones stay valid; the module emitter then
// attaches the function's own location. Ids come from the module's
// DebugModule, keyed on the function's subprogram id; with debug info off
// there is no DebugModule and nothing is recorded.

#include "quidra/ir/module.hpp"
#include "llvm_backend/debug_module.hpp"
#include "llvm_backend/statement_attribution.hpp"
#include "llvm_backend/symbol_table.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::llvm_backend {

class DebugEmitter {
public:
    DebugEmitter(llvm_text::LlvmBuilder& builder,const SymbolTable& symbols,StatementAttribution& attribution,
                 DebugModule* module,std::optional<std::size_t> subprogram,std::size_t file)
        :builder_(builder),symbols_(symbols),attribution_(attribution),module_(module),
         debug_subprogram_(subprogram),debug_file_(file){}

    // The handler of the debug domain. FunctionEmitter calls it before the
    // segment of any instruction opens, so the statement's call is part of
    // no segment. Defined in the class: it runs for every source location.
    void emit(const ir::SourceLocation& location,const ir::Instruction&){
        attribution_.begin_statement(location);
        record_location(location);
    }

    std::optional<std::size_t> subprogram_id()const{return debug_subprogram_;}
    void declare_variable(
        const std::string& storage_name,const std::string& source_name,
        const Type& type,std::uint32_t line,std::size_t argument=0);
    // A SourceLocation: the statement location of the instructions that
    // follow. Defined in the class, as is attach_pending_location: the
    // dispatcher calls them for every instruction.
    void record_location(const ir::SourceLocation& location){
        if(debug_subprogram_&&module_) {
            const auto id=module_->next_id();
            module_->record_location(DebugLocationRecord{
                id,*debug_subprogram_,
                std::max<std::uint32_t>(1,location.line),
                std::max<std::uint32_t>(1,location.column)});
            pending_debug_location_=id;
        }
    }
    // The text of one instruction, [begin, end) of the function text, takes
    // the waiting statement location, if any and if it wrote text.
    void attach_pending_location(std::size_t debug_begin,std::size_t debug_end){
        if(pending_debug_location_&&debug_end>debug_begin) {
            debug_segments_.push_back(DebugSegment{
                debug_begin,debug_end,*pending_debug_location_});
            pending_debug_location_.reset();
        }
    }
    void attach_locations(std::string& text)const;

private:
    std::optional<std::size_t> debug_variable(
        const std::string& storage_name,const std::string& source_name,
        const Type& type,std::uint32_t line,std::size_t argument=0);

    llvm_text::LlvmBuilder& builder_;
    const SymbolTable& symbols_;
    StatementAttribution& attribution_;
    DebugModule* module_;
    std::optional<std::size_t> debug_subprogram_;
    std::size_t debug_file_{};
    std::unordered_map<std::string,std::size_t> debug_variables_;
    std::optional<std::size_t> pending_debug_location_;
    struct DebugSegment {
        std::size_t begin{};
        std::size_t end{};
        std::size_t location{};
    };
    std::vector<DebugSegment> debug_segments_;
};

} // namespace quidra::llvm_backend
