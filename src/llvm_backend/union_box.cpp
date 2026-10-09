#include "llvm_backend/union_box.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/storage_layout.hpp"
#include "quidra/abi/layout.hpp"

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void UnionBox::allocate(const std::string& box,const Type& union_type){
    builder_.call(box,runtime_abi::prelude::alloc,{{i64,union_box_bytes(union_type)}});
}

void UnionBox::store_tag(const std::string& box,int tag){
    builder_.store({i64,tag},box,Align::none);
}

void UnionBox::payload_slot(const std::string& slot,const std::string& box){
    builder_.getelementptr(slot,Inbounds::yes,i8,box,{{i64,abi::union_box_layout::payload_offset}});
}

} // namespace quidra::llvm_backend
