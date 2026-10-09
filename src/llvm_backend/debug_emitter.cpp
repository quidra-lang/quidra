#include "llvm_backend/debug_emitter.hpp"

#include "llvm_backend/debug_records.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

std::optional<std::size_t> DebugEmitter::debug_variable(
    const std::string& storage_name,const std::string& source_name,
    const Type& type,std::uint32_t line,std::size_t argument) {
    if(source_name.empty()||!debug_subprogram_||!module_||debug_file_==0)
        return std::nullopt;
    if(const auto found=debug_variables_.find(storage_name);found!=debug_variables_.end())
        return found->second;
    auto basic=debug_basic_type(type);
    if(!basic) return std::nullopt;
    const auto type_id=module_->next_id();
    const auto variable_id=module_->next_id();
    module_->record_variable(DebugVariableRecord{
        variable_id,type_id,*debug_subprogram_,debug_file_,source_name,std::move(*basic),
        std::max<std::uint32_t>(1,line),argument});
    debug_variables_.emplace(storage_name,variable_id);
    return variable_id;
}

void DebugEmitter::declare_variable(
    const std::string& storage_name,const std::string& source_name,
    const Type& type,std::uint32_t line,std::size_t argument) {
    if(const auto variable=debug_variable(
           storage_name,source_name,type,line,argument)) {
        const auto node="!"+std::to_string(*variable);
        builder_.debug_declare({ptr,symbols_.storage(storage_name)},node,"!DIExpression()");
    }
}

void DebugEmitter::attach_locations(std::string& text)const{
    for(auto it=debug_segments_.rbegin();it!=debug_segments_.rend();++it)attach_debug_location_range(text,it->begin,it->end,it->location);
}

} // namespace quidra::llvm_backend
