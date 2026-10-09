#pragma once

// UnionBox: the box that holds a value of a union type (T | error, T | none,
// a variant): from quidra_alloc, the case tag (i64) at offset 0 and the
// payload at offset 8; 16 bytes, or 24 when a case is a `real`
// (union_box_bytes). The runtime and the type helpers read and write boxes
// with this layout.
//
// Owns no state. Each operation writes one line of the function's text
// under the names the caller allocated.

#include "quidra/types.hpp"
#include "llvm_text/llvm_builder.hpp"

#include <string>

namespace quidra::llvm_backend {

class UnionBox {
public:
    explicit UnionBox(llvm_text::LlvmBuilder& builder):builder_(builder){}

    // box = a new box for a value of union_type.
    void allocate(const std::string& box,const Type& union_type);
    // Stores the case tag of the box.
    void store_tag(const std::string& box,int tag);
    // slot = the address of the box's payload.
    void payload_slot(const std::string& slot,const std::string& box);

private:
    llvm_text::LlvmBuilder& builder_;
};

} // namespace quidra::llvm_backend
