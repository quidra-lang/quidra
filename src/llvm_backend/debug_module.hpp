#pragma once

// DebugModule: the debug-information metadata of one module (DWARF through
// LLVM's !DI* nodes). The module emitter creates it only when debug info is
// on.
//
// Owns: the metadata ids (!N) and every record that becomes a metadata node.
// Ids 0 to 6 are the fixed nodes of the compile unit, and !1 is the primary
// source file (the entry function's, else the first function's that has
// one). The constructor numbers, from 7 on, the other source files in
// function order, then a subprogram and a location for each function that
// has a source file and is not external. The function emitters number the
// statement locations and variables as they emit (next_id). All ids come from
// one counter, so the order of allocation is part of the output. Source files
// print in path order. Without a primary source no id is assigned and nothing
// is printed.

#include "quidra/ir/module.hpp"
#include "llvm_backend/debug_records.hpp"
#include "llvm_text/llvm_module.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace quidra::llvm_backend {

class DebugModule {
public:
    explicit DebugModule(const ir::Module& module);

    // By the function's index in the module: its subprogram and location
    // ids, and the id of its source file (0 without a subprogram).
    std::optional<std::size_t> subprogram_id(std::size_t function)const{return debug_subprograms_[function];}
    std::optional<std::size_t> location_id(std::size_t function)const{return debug_locations_[function];}
    std::size_t file_id(std::size_t function)const;

    std::size_t next_id(){return next_debug_metadata_++;}
    void record_location(DebugLocationRecord&& record){debug_statement_locations_.push_back(std::move(record));}
    void record_variable(DebugVariableRecord&& record){debug_variables_.push_back(std::move(record));}
    bool has_variables()const{return !debug_variables_.empty();}

    // source_filename, in the module's header.
    void emit_source_filename(llvm_text::LlvmModule& module)const;
    // Every metadata node, in the module's metadata.
    void emit_metadata(llvm_text::LlvmModule& module)const;

private:
    const ir::Module& module_;
    std::string primary_source_;
    std::map<std::string,std::size_t> debug_files_;
    std::size_t next_debug_metadata_=7;
    std::vector<std::optional<std::size_t>> debug_subprograms_;
    std::vector<std::optional<std::size_t>> debug_locations_;
    std::vector<DebugLocationRecord> debug_statement_locations_;
    std::vector<DebugVariableRecord> debug_variables_;
};

} // namespace quidra::llvm_backend
