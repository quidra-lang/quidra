#pragma once

// ReplValuePrinter: the code that prints a REPL result: scalars, strings,
// bins and errors directly, arrays, class objects and unions recursively,
// depth first, with uninitialized class fields shown as <uninitialized>.
//
// Owns: the scratch slot that holds the loop index while an array prints,
// set by the REPL display around one result. Interns its literal texts in
// the module's StringPool and allocates its temporaries and labels from the
// function's TemporaryNames, in printing order, which is part of the output.

#include "quidra/ir/module.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "llvm_backend/string_pool.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_backend/union_box.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace quidra::llvm_backend {

class ReplValuePrinter {
public:
    ReplValuePrinter(llvm_text::LlvmBuilder& builder,TemporaryNames& names,StringPool& pool,
                     const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                     const ArrayLayoutPolicy& array_layout,UnionBox& union_box)
        :builder_(builder),names_(names),pool_(pool),layouts_(layouts),array_layout_(array_layout),
         union_box_(union_box){}

    // The entry-frame slot an array display uses as its loop index, for the
    // duration of one REPL display.
    void set_array_index_slot(const std::string& slot){array_index_slot_=slot;}
    void clear_array_index_slot(){array_index_slot_.clear();}

    void print_text(const std::string& text);

    void print_value(const Type& type, const std::string& raw_value,
                     const std::vector<std::string>& initialized_paths = {},
                     const std::string& path_prefix = {});

private:
    bool path_initialized(const std::vector<std::string>& initialized_paths,
                          const std::string& path) const;

    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    StringPool& pool_;
    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
    const ArrayLayoutPolicy& array_layout_;
    UnionBox& union_box_;
    std::string array_index_slot_;
};

} // namespace quidra::llvm_backend
